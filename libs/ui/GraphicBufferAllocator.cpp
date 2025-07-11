/*
**
** Copyright 2009, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "GraphicBufferAllocator"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include <ui/GraphicBufferAllocator.h>

#include <limits.h>
#include <stdio.h>

#include <grallocusage/GrallocUsageConversion.h>

#include <android-base/stringprintf.h>
#include <cutils/properties.h>
#include <log/log.h>
#include <utils/Singleton.h>
#include <utils/Trace.h>

#include <ui/Gralloc.h>
#include <ui/Gralloc2.h>
#include <ui/Gralloc3.h>
#include <ui/Gralloc4.h>
#include <ui/Gralloc5.h>
#include <ui/GraphicBufferMapper.h>

namespace android {
// ---------------------------------------------------------------------------

using base::StringAppendF;

ANDROID_SINGLETON_STATIC_INSTANCE( GraphicBufferAllocator )

Mutex GraphicBufferAllocator::sLock;
KeyedVector<buffer_handle_t,
    GraphicBufferAllocator::alloc_rec_t> GraphicBufferAllocator::sAllocList;

GraphicBufferAllocator::GraphicBufferAllocator() : mMapper(GraphicBufferMapper::getInstance()) {
    char default_gralloc[PROPERTY_VALUE_MAX]; 
    property_get("debug.ui.default_mapper", default_gralloc, "");

    if (std::string(default_gralloc).empty()) {
        switch (mMapper.getMapperVersion()) {
            case GraphicBufferMapper::GRALLOC_5:
                mAllocator = std::make_unique<const Gralloc5Allocator>(
                        reinterpret_cast<const Gralloc5Mapper&>(mMapper.getGrallocMapper()));
                break;
            case GraphicBufferMapper::GRALLOC_4:
                mAllocator = std::make_unique<const Gralloc4Allocator>(
                        reinterpret_cast<const Gralloc4Mapper&>(mMapper.getGrallocMapper()));
                break;
            case GraphicBufferMapper::GRALLOC_3:
                mAllocator = std::make_unique<const Gralloc3Allocator>(
                        reinterpret_cast<const Gralloc3Mapper&>(mMapper.getGrallocMapper()));
                break;
            case GraphicBufferMapper::GRALLOC_2:
                mAllocator = std::make_unique<const Gralloc2Allocator>(
                        reinterpret_cast<const Gralloc2Mapper&>(mMapper.getGrallocMapper()));
                break;
        }
    } else {
        if (atoi(default_gralloc) == 5) {
            mAllocator = std::make_unique<const Gralloc5Allocator>(
                    reinterpret_cast<const Gralloc5Mapper&>(mMapper.getGrallocMapper()));
            if (mAllocator->isLoaded()) {
                return;
            }
        } else if (atoi(default_gralloc) == 4) {
            mAllocator = std::make_unique<const Gralloc4Allocator>(
                    reinterpret_cast<const Gralloc4Mapper&>(mMapper.getGrallocMapper()));
            if (mAllocator->isLoaded()) {
                return;
            }
        } else if (atoi(default_gralloc) == 3) {
            mAllocator = std::make_unique<const Gralloc3Allocator>(
                    reinterpret_cast<const Gralloc3Mapper&>(mMapper.getGrallocMapper()));
            if (mAllocator->isLoaded()) {
                return;
            }
        //TODO: need to fix ?
        } else {
            mAllocator = std::make_unique<const Gralloc2Allocator>(
                    reinterpret_cast<const Gralloc2Mapper&>(mMapper.getGrallocMapper()));
            if (mAllocator->isLoaded()) {
                return;
            }
        }
    }

    LOG_ALWAYS_FATAL_IF(!mAllocator->isLoaded(),
                        "Failed to load matching allocator for mapper version %d",
                        mMapper.getMapperVersion());
}

GraphicBufferAllocator::~GraphicBufferAllocator() {}

uint64_t GraphicBufferAllocator::getTotalSize() const {
    Mutex::Autolock _l(sLock);
    uint64_t total = 0;
    for (size_t i = 0; i < sAllocList.size(); ++i) {
        total += sAllocList.valueAt(i).size;
    }
    return total;
}

void GraphicBufferAllocator::dump(std::string& result, bool less) const {
    Mutex::Autolock _l(sLock);
    KeyedVector<buffer_handle_t, alloc_rec_t>& list(sAllocList);
    uint64_t total = 0;
    result.append("GraphicBufferAllocator buffers:\n");
    const size_t count = list.size();
    StringAppendF(&result, "%14s | %11s | %18s | %s | %8s | %10s | %s\n", "Handle", "Size",
                  "W (Stride) x H", "Layers", "Format", "Usage", "Requestor");
    for (size_t i = 0; i < count; i++) {
        const alloc_rec_t& rec(list.valueAt(i));
        std::string sizeStr = (rec.size)
                ? base::StringPrintf("%7.2f KiB", static_cast<double>(rec.size) / 1024.0)
                : "unknown";
        StringAppendF(&result, "%14p | %11s | %4u (%4u) x %4u | %6u | %8X | 0x%8" PRIx64 " | %s\n",
                      list.keyAt(i), sizeStr.c_str(), rec.width, rec.stride, rec.height,
                      rec.layerCount, rec.format, rec.usage, rec.requestorName.c_str());
        total += rec.size;
    }
    StringAppendF(&result, "Total allocated by GraphicBufferAllocator (estimate): %.2f KB\n",
                  static_cast<double>(total) / 1024.0);

    result.append(mAllocator->dumpDebugInfo(less));
}

void GraphicBufferAllocator::dumpToSystemLog(bool less) {
    std::string s;
    GraphicBufferAllocator::getInstance().dump(s, less);
    ALOGD("%s", s.c_str());
}

auto GraphicBufferAllocator::allocate(const AllocationRequest& request) -> AllocationResult {
    ATRACE_CALL();
    if (!request.width || !request.height) {
        return AllocationResult(BAD_VALUE);
    }

    const auto width = request.width;
    const auto height = request.height;

    const uint32_t bpp = bytesPerPixel(request.format);
    if (std::numeric_limits<size_t>::max() / width / height < static_cast<size_t>(bpp)) {
        ALOGE("Failed to allocate (%u x %u) layerCount %u format %d "
              "usage %" PRIx64 ": Requesting too large a buffer size",
              request.width, request.height, request.layerCount, request.format, request.usage);
        return AllocationResult(BAD_VALUE);
    }

    if (request.layerCount < 1) {
        return AllocationResult(BAD_VALUE);
    }

    auto result = mAllocator->allocate(request);
    if (result.status == UNKNOWN_TRANSACTION) {
        if (!request.extras.empty()) {
            ALOGE("Failed to allocate with additional options, allocator version mis-match? "
                  "gralloc version = %d",
                  (int)mMapper.getMapperVersion());
            return result;
        }
        // If there's no additional options, fall back to previous allocate version
        result.status = mAllocator->allocate(request.requestorName, request.width, request.height,
                                             request.format, request.layerCount, request.usage,
                                             &result.stride, &result.handle, request.importBuffer);
    }

    if (result.status != NO_ERROR) {
        ALOGE("Failed to allocate (%u x %u) layerCount %u format %d "
              "usage %" PRIx64 ": %d",
              request.width, request.height, request.layerCount, request.format, request.usage,
              result.status);
        return result;
    }

    if (!request.importBuffer) {
        return result;
    }
    size_t bufSize;

    // if stride has no meaning or is too large,
    // approximate size with the input width instead
    if ((result.stride) != 0 &&
        std::numeric_limits<size_t>::max() / height / (result.stride) < static_cast<size_t>(bpp)) {
        bufSize = static_cast<size_t>(width) * height * bpp;
    } else {
        bufSize = static_cast<size_t>((result.stride)) * height * bpp;
    }

    Mutex::Autolock _l(sLock);
    KeyedVector<buffer_handle_t, alloc_rec_t>& list(sAllocList);
    alloc_rec_t rec;
    rec.width = width;
    rec.height = height;
    rec.stride = result.stride;
    rec.format = request.format;
    rec.layerCount = request.layerCount;
    rec.usage = request.usage;
    rec.size = bufSize;
    rec.requestorName = request.requestorName;
    list.add(result.handle, rec);

    return result;
}

status_t GraphicBufferAllocator::allocateHelper(uint32_t width, uint32_t height, PixelFormat format,
                                                uint32_t layerCount, uint64_t usage,
                                                buffer_handle_t* handle, uint32_t* stride,
                                                std::string requestorName, bool importBuffer) {
    ATRACE_CALL();

    // make sure to not allocate a N x 0 or 0 x N buffer, since this is
    // allowed from an API stand-point allocate a 1x1 buffer instead.
    if (!width || !height)
        width = height = 1;

    const uint32_t bpp = bytesPerPixel(format);
    if (std::numeric_limits<size_t>::max() / width / height < static_cast<size_t>(bpp)) {
        ALOGE("Failed to allocate (%u x %u) layerCount %u format %d "
              "usage %" PRIx64 ": Requesting too large a buffer size",
              width, height, layerCount, format, usage);
        return BAD_VALUE;
    }

    // Ensure that layerCount is valid.
    if (layerCount < 1) {
        layerCount = 1;
    }

    // TODO(b/72323293, b/72703005): Remove these invalid bits from callers
    usage &= ~static_cast<uint64_t>((1 << 10) | (1 << 13));

    status_t error = mAllocator->allocate(requestorName, width, height, format, layerCount, usage,
                                          stride, handle, importBuffer);
    if (error != NO_ERROR) {
        ALOGE("Failed to allocate (%u x %u) layerCount %u format %d "
              "usage %" PRIx64 ": %d",
              width, height, layerCount, format, usage, error);
        return error;
    }

    if (!importBuffer) {
        return NO_ERROR;
    }
    size_t bufSize;

    // if stride has no meaning or is too large,
    // approximate size with the input width instead
    if ((*stride) != 0 &&
        std::numeric_limits<size_t>::max() / height / (*stride) < static_cast<size_t>(bpp)) {
        bufSize = static_cast<size_t>(width) * height * bpp;
    } else {
        bufSize = static_cast<size_t>((*stride)) * height * bpp;
    }

    Mutex::Autolock _l(sLock);
    KeyedVector<buffer_handle_t, alloc_rec_t>& list(sAllocList);
    alloc_rec_t rec;
    rec.width = width;
    rec.height = height;
    rec.stride = *stride;
    rec.format = format;
    rec.layerCount = layerCount;
    rec.usage = usage;
    rec.size = bufSize;
    rec.requestorName = std::move(requestorName);
    list.add(*handle, rec);

    return NO_ERROR;
}
status_t GraphicBufferAllocator::allocate(uint32_t width, uint32_t height, PixelFormat format,
                                          uint32_t layerCount, uint64_t usage,
                                          buffer_handle_t* handle, uint32_t* stride,
                                          std::string requestorName) {
    return allocateHelper(width, height, format, layerCount, usage, handle, stride, requestorName,
                          true);
}

status_t GraphicBufferAllocator::allocateRawHandle(uint32_t width, uint32_t height,
                                                   PixelFormat format, uint32_t layerCount,
                                                   uint64_t usage, buffer_handle_t* handle,
                                                   uint32_t* stride, std::string requestorName) {
    return allocateHelper(width, height, format, layerCount, usage, handle, stride, requestorName,
                          false);
}

// DEPRECATED
status_t GraphicBufferAllocator::allocate(uint32_t width, uint32_t height, PixelFormat format,
                                          uint32_t layerCount, uint64_t usage,
                                          buffer_handle_t* handle, uint32_t* stride,
                                          uint64_t /*graphicBufferId*/, std::string requestorName) {
    return allocateHelper(width, height, format, layerCount, usage, handle, stride, requestorName,
                          true);
}

status_t GraphicBufferAllocator::free(buffer_handle_t handle)
{
    ATRACE_CALL();

    // We allocated a buffer from the allocator and imported it into the
    // mapper to get the handle.  We just need to free the handle now.
    mMapper.freeBuffer(handle);

    Mutex::Autolock _l(sLock);
    KeyedVector<buffer_handle_t, alloc_rec_t>& list(sAllocList);
    list.removeItem(handle);

    return NO_ERROR;
}

// ---------------------------------------------------------------------------
}; // namespace android
