/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"

// #define LOG_NDEBUG 0
#undef LOG_TAG
#define LOG_TAG "DisplayDevice"

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include <common/FlagManager.h>
#include <compositionengine/CompositionEngine.h>
#include <compositionengine/Display.h>
#include <compositionengine/DisplayColorProfile.h>
#include <compositionengine/DisplayColorProfileCreationArgs.h>
#include <compositionengine/DisplayCreationArgs.h>
#include <compositionengine/DisplaySurface.h>
#include <compositionengine/ProjectionSpace.h>
#include <compositionengine/RenderSurface.h>
#include <compositionengine/RenderSurfaceCreationArgs.h>
#include <compositionengine/impl/OutputCompositionState.h>
#include <configstore/Utils.h>
#include <log/log.h>
#include <system/window.h>

#include <fcntl.h>
#include <termios.h>
#include <linux/kd.h>
#include <linux/vt.h>

#include "DisplayDevice.h"
#include "FrontEnd/DisplayInfo.h"
#include "HdrSdrRatioOverlay.h"
#include "Layer.h"
#include "RefreshRateOverlay.h"
#include "SurfaceFlinger.h"

// Duplicate enum values here, this avoids making SurfaceFlinger module
// depend on drm_gralloc, which is obsolete and will eventually go away.

enum {
    GRALLOC_MODULE_PERFORM_ENTER_VT                  = 0x80000005,
    GRALLOC_MODULE_PERFORM_LEAVE_VT                  = 0x80000006,
};

namespace android {

namespace hal = hardware::graphics::composer::hal;

#ifdef CONSOLE_MANAGER
class ConsoleManagerThread : public Thread {
public:
            ConsoleManagerThread(const sp<SurfaceFlinger>&, const wp<IBinder>&);
    virtual ~ConsoleManagerThread();

    status_t releaseScreen() const;

private:
    sp<SurfaceFlinger> mFlinger;
    wp<IBinder> mDisplayToken;
    int consoleFd;
    long prev_vt_num;
    vt_mode vm;
    virtual void onFirstRef();
    virtual status_t readyToRun();
    virtual void requestExit();
    virtual bool threadLoop();
    static void sigHandler(int sig);
    static pid_t sSignalCatcherPid;
};

ConsoleManagerThread::ConsoleManagerThread(const sp<SurfaceFlinger>& flinger, const wp<IBinder>& token)
    : Thread(false), mFlinger(flinger), mDisplayToken(token), consoleFd(-1)
{
    sSignalCatcherPid = 0;

    // create a new console
    char const * const ttydev = "/dev/tty0";
    int fd = open(ttydev, O_RDWR | O_SYNC);
    if (fd < 0) {
        ALOGE("Can't open %s, errno=%d (%s)", ttydev, errno, strerror(errno));
        consoleFd = -errno;
        return;
    }
    ALOGD("Open /dev/tty0 OK");

    // to make sure that we are in text mode
    int res = ioctl(fd, KDSETMODE, (void*) KD_TEXT);
    if (res < 0) {
        ALOGE("ioctl(%d, KDSETMODE, ...) failed, res %d (%s)",
                fd, res, strerror(errno));
    }

    // get the current console
    struct vt_stat vs;
    res = ioctl(fd, VT_GETSTATE, &vs);
    if (res < 0) {
        ALOGE("ioctl(%d, VT_GETSTATE, ...) failed, res %d (%s)",
                fd, res, strerror(errno));
        consoleFd = -errno;
        return;
    }

    // switch to console 7 (which is what X normaly uses)
    do {
        res = ioctl(fd, VT_ACTIVATE, ANDROID_VT);
    } while(res < 0 && errno == EINTR);
    if (res < 0) {
        ALOGE("ioctl(%d, VT_ACTIVATE, ...) failed, %d (%s) for vt %d",
                fd, errno, strerror(errno), ANDROID_VT);
        consoleFd = -errno;
        return;
    }

    do {
        res = ioctl(fd, VT_WAITACTIVE, ANDROID_VT);
    } while (res < 0 && errno == EINTR);
    if (res < 0) {
        ALOGE("ioctl(%d, VT_WAITACTIVE, ...) failed, %d %d %s for vt %d",
                fd, res, errno, strerror(errno), ANDROID_VT);
        consoleFd = -errno;
        return;
    }

    // open the new console
    close(fd);
    fd = open(ttydev, O_RDWR | O_SYNC);
    if (fd < 0) {
        ALOGE("Can't open new console %s", ttydev);
        consoleFd = -errno;
        return;
    }

    /* disable console line buffer, echo, ... */
    struct termios ttyarg;
    ioctl(fd, TCGETS , &ttyarg);
    ttyarg.c_iflag = 0;
    ttyarg.c_lflag = 0;
    ioctl(fd, TCSETS , &ttyarg);

    // set up signals so we're notified when the console changes
    // we can't use SIGUSR1 because it's used by the java-vm
    vm.mode = VT_PROCESS;
    vm.waitv = 0;
    vm.relsig = SIGUSR2;
    vm.acqsig = SIGUNUSED;
    vm.frsig = 0;

    struct sigaction act;
    sigemptyset(&act.sa_mask);
    act.sa_handler = sigHandler;
    act.sa_flags = 0;
    sigaction(vm.relsig, &act, NULL);

    sigemptyset(&act.sa_mask);
    act.sa_handler = sigHandler;
    act.sa_flags = 0;
    sigaction(vm.acqsig, &act, NULL);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, vm.relsig);
    sigaddset(&mask, vm.acqsig);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    // switch to graphic mode
    res = ioctl(fd, KDSETMODE, (void*)KD_GRAPHICS);
    ALOGW_IF(res < 0,
            "ioctl(%d, KDSETMODE, KD_GRAPHICS) failed, res %d", fd, res);

    prev_vt_num = vs.v_active;
    consoleFd = fd;
}

ConsoleManagerThread::~ConsoleManagerThread()
{
    if (consoleFd >= 0) {
        int fd = consoleFd;
        int res;
        ioctl(fd, KDSETMODE, (void*)KD_TEXT);
        do {
            res = ioctl(fd, VT_ACTIVATE, prev_vt_num);
        } while(res < 0 && errno == EINTR);
        do {
            res = ioctl(fd, VT_WAITACTIVE, prev_vt_num);
        } while(res < 0 && errno == EINTR);
        close(fd);
        char const * const ttydev = "/dev/tty0";
        fd = open(ttydev, O_RDWR | O_SYNC);
        ioctl(fd, VT_DISALLOCATE, 0);
        close(fd);
    }
}

status_t ConsoleManagerThread::releaseScreen() const
{
    int err = ioctl(consoleFd, VT_RELDISP, (void*)1);
    ALOGE_IF(err < 0, "ioctl(%d, VT_RELDISP, 1) failed %d (%s)",
        consoleFd, errno, strerror(errno));
    return (err < 0) ? (-errno) : status_t(NO_ERROR);
}

void ConsoleManagerThread::onFirstRef()
{
    run("ConsoleManagerThread", PRIORITY_URGENT_DISPLAY);
}

status_t ConsoleManagerThread::readyToRun()
{
    if (consoleFd >= 0) {
        sSignalCatcherPid = gettid();

        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, vm.relsig);
        sigaddset(&mask, vm.acqsig);
        sigprocmask(SIG_BLOCK, &mask, NULL);

        int res = ioctl(consoleFd, VT_SETMODE, &vm);
        if (res < 0) {
            ALOGE("ioctl(%d, VT_SETMODE, ...) failed, %d (%s)",
                    consoleFd, errno, strerror(errno));
        }
        return NO_ERROR;
    }
    return consoleFd;
}

void ConsoleManagerThread::requestExit()
{
    Thread::requestExit();
    if (sSignalCatcherPid != 0) {
        // wake the thread up
        kill(sSignalCatcherPid, SIGINT);
        // wait for it...
    }
}

bool ConsoleManagerThread::threadLoop()
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, vm.relsig);
    sigaddset(&mask, vm.acqsig);

    int sig = 0;
    sigwait(&mask, &sig);

    hw_module_t const* mod;
    gralloc_module_t const* gr = NULL;
    status_t err = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &mod);
    if (!err) {
        gr = reinterpret_cast<gralloc_module_t const*>(mod);
        if (!gr->perform)
            gr = NULL;
    }

    if (sig == vm.relsig) {
        if (gr)
            gr->perform(gr, GRALLOC_MODULE_PERFORM_LEAVE_VT);
        mFlinger->screenReleased(mDisplayToken.promote());
    } else if (sig == vm.acqsig) {
        mFlinger->screenAcquired(mDisplayToken.promote());
        if (gr)
            gr->perform(gr, GRALLOC_MODULE_PERFORM_ENTER_VT);
    }

    return true;
}

void ConsoleManagerThread::sigHandler(int sig)
{
    // resend the signal to our signal catcher thread
    ALOGW("received signal %d in thread %d, resending to %d",
            sig, gettid(), sSignalCatcherPid);

    // we absolutely need the delays below because without them
    // our main thread never gets a chance to handle the signal.
    usleep(10000);
    kill(sSignalCatcherPid, sig);
    usleep(10000);
}

pid_t ConsoleManagerThread::sSignalCatcherPid;
#endif

DisplayDeviceCreationArgs::DisplayDeviceCreationArgs(
        const sp<SurfaceFlinger>& flinger, HWComposer& hwComposer, const wp<IBinder>& displayToken,
        std::shared_ptr<compositionengine::Display> compositionDisplay)
      : flinger(flinger),
        hwComposer(hwComposer),
        displayToken(displayToken),
        compositionDisplay(compositionDisplay) {}

DisplayDevice::DisplayDevice(DisplayDeviceCreationArgs& args)
      : mFlinger(args.flinger),
        mHwComposer(args.hwComposer),
        mDisplayToken(args.displayToken),
        mSequenceId(args.sequenceId),
        mCompositionDisplay{args.compositionDisplay},
        mPendingModeFpsTrace(concatId("PendingModeFps")),
        mActiveModeFpsTrace(concatId("ActiveModeFps")),
        mRenderRateFpsTrace(concatId("RenderRateFps")),
        mPhysicalOrientation(args.physicalOrientation),
        mPowerMode(ftl::Concat("PowerMode ", getId().value).c_str(), args.initialPowerMode),
#ifdef CONSOLE_MANAGER
        mConsoleManagerThread(0),
#endif
        mIsPrimary(args.isPrimary),
        mRequestedRefreshRate(args.requestedRefreshRate),
        mRefreshRateSelector(std::move(args.refreshRateSelector)),
        mHasDesiredModeTrace(concatId("HasDesiredMode"), false) {
    mCompositionDisplay->editState().isSecure = args.isSecure;
    mCompositionDisplay->editState().isProtected = args.isProtected;
    mCompositionDisplay->createRenderSurface(
            compositionengine::RenderSurfaceCreationArgsBuilder()
                    .setDisplayWidth(ANativeWindow_getWidth(args.nativeWindow.get()))
                    .setDisplayHeight(ANativeWindow_getHeight(args.nativeWindow.get()))
                    .setNativeWindow(std::move(args.nativeWindow))
                    .setDisplaySurface(std::move(args.displaySurface))
                    .setMaxTextureCacheSize(
                            static_cast<size_t>(SurfaceFlinger::maxFrameBufferAcquiredBuffers))
                    .build());

    if (!mFlinger->mDisableClientCompositionCache &&
        SurfaceFlinger::maxFrameBufferAcquiredBuffers > 0) {
        mCompositionDisplay->createClientCompositionCache(
                static_cast<uint32_t>(SurfaceFlinger::maxFrameBufferAcquiredBuffers));
    }

    mCompositionDisplay->setPredictCompositionStrategy(mFlinger->mPredictCompositionStrategy);
    mCompositionDisplay->setTreat170mAsSrgb(mFlinger->mTreat170mAsSrgb);
    mCompositionDisplay->createDisplayColorProfile(
            compositionengine::DisplayColorProfileCreationArgsBuilder()
                    .setHasWideColorGamut(args.hasWideColorGamut)
                    .setHdrCapabilities(std::move(args.hdrCapabilities))
                    .setSupportedPerFrameMetadata(args.supportedPerFrameMetadata)
                    .setHwcColorModes(std::move(args.hwcColorModes))
                    .Build());

    if (!mCompositionDisplay->isValid()) {
        ALOGE("Composition Display did not validate!");
    }

    mCompositionDisplay->getRenderSurface()->initialize();

    setPowerMode(args.initialPowerMode);

    // initialize the display orientation transform.
    setProjection(ui::ROTATION_0, Rect::INVALID_RECT, Rect::INVALID_RECT);
}

#ifdef CONSOLE_MANAGER
DisplayDevice::~DisplayDevice() {
    if (mConsoleManagerThread != 0) {
        mConsoleManagerThread->requestExitAndWait();
        ALOGD("ConsoleManagerThread: destroy primary DisplayDevice");
    }
}
#else
DisplayDevice::~DisplayDevice() = default;
#endif

void DisplayDevice::disconnect() {
    mCompositionDisplay->disconnect();
}

int DisplayDevice::getWidth() const {
    return mCompositionDisplay->getState().displaySpace.getBounds().width;
}

int DisplayDevice::getHeight() const {
    return mCompositionDisplay->getState().displaySpace.getBounds().height;
}

void DisplayDevice::setDisplayName(const std::string& displayName) {
    if (!displayName.empty()) {
        // never override the name with an empty name
        mDisplayName = displayName;
        mCompositionDisplay->setName(displayName);
    }
}

auto DisplayDevice::getFrontEndInfo() const -> frontend::DisplayInfo {
    gui::DisplayInfo info;
    info.displayId = getLayerStack().id;

    // The physical orientation is set when the orientation of the display panel is
    // different than the default orientation of the device. Other services like
    // InputFlinger do not know about this, so we do not need to expose the physical
    // orientation of the panel outside of SurfaceFlinger.
    const ui::Rotation inversePhysicalOrientation = ui::ROTATION_0 - mPhysicalOrientation;
    auto width = getWidth();
    auto height = getHeight();
    if (inversePhysicalOrientation == ui::ROTATION_90 ||
        inversePhysicalOrientation == ui::ROTATION_270) {
        std::swap(width, height);
    }
    const ui::Transform undoPhysicalOrientation(ui::Transform::toRotationFlags(
                                                        inversePhysicalOrientation),
                                                width, height);
    const auto& displayTransform = undoPhysicalOrientation * getTransform();
    // Send the inverse display transform to input so it can convert display coordinates to
    // logical display.
    info.transform = displayTransform.inverse();

    info.logicalWidth = getLayerStackSpaceRect().width();
    info.logicalHeight = getLayerStackSpaceRect().height();

    return {.info = info,
            .transform = displayTransform,
            .receivesInput = receivesInput(),
            .isSecure = isSecure(),
            .isPrimary = isPrimary(),
            .isVirtual = isVirtual(),
            .rotationFlags = ui::Transform::toRotationFlags(mOrientation),
            .transformHint = getTransformHint()};
}

void DisplayDevice::setPowerMode(hal::PowerMode mode) {
    // TODO(b/241285876): Skip this for virtual displays.
    if (mode == hal::PowerMode::OFF || mode == hal::PowerMode::ON) {
        if (mStagedBrightness && mBrightness != mStagedBrightness) {
            getCompositionDisplay()->setNextBrightness(*mStagedBrightness);
            mBrightness = *mStagedBrightness;
        }
        mStagedBrightness = std::nullopt;
        getCompositionDisplay()->applyDisplayBrightness(true);
    }

    mPowerMode = mode;

    getCompositionDisplay()->setCompositionEnabled(isPoweredOn());

#ifdef CONSOLE_MANAGER
    if (mode != hal::PowerMode::ON && mConsoleManagerThread != 0) {
        mConsoleManagerThread->releaseScreen();
    }
#endif
}

void DisplayDevice::tracePowerMode() {
    // Assign the same value for tracing.
    mPowerMode = mPowerMode.get();
}

void DisplayDevice::enableLayerCaching(bool enable) {
    getCompositionDisplay()->setLayerCachingEnabled(enable);
}

hal::PowerMode DisplayDevice::getPowerMode() const {
    return mPowerMode;
}

bool DisplayDevice::isPoweredOn() const {
    return mPowerMode != hal::PowerMode::OFF;
}

void DisplayDevice::setActiveMode(DisplayModeId modeId, Fps vsyncRate, Fps renderFps) {
    ATRACE_INT(mActiveModeFpsTrace.c_str(), vsyncRate.getIntValue());
    ATRACE_INT(mRenderRateFpsTrace.c_str(), renderFps.getIntValue());

    mRefreshRateSelector->setActiveMode(modeId, renderFps);
    updateRefreshRateOverlayRate(vsyncRate, renderFps);
}

bool DisplayDevice::initiateModeChange(display::DisplayModeRequest&& desiredMode,
                                       const hal::VsyncPeriodChangeConstraints& constraints,
                                       hal::VsyncPeriodChangeTimeline& outTimeline) {
    // TODO(b/255635711): Flow the DisplayModeRequest through the desired/pending/active states. For
    // now, `desiredMode` and `mDesiredModeOpt` are one and the same, but the latter is not cleared
    // until the next `SF::initiateDisplayModeChanges`. However, the desired mode has been consumed
    // at this point, so clear the `force` flag to prevent an endless loop of `initiateModeChange`.
    if (FlagManager::getInstance().connected_display()) {
        std::scoped_lock lock(mDesiredModeLock);
        if (mDesiredModeOpt) {
            mDesiredModeOpt->force = false;
        }
    }

    mPendingModeOpt = std::move(desiredMode);
    mIsModeSetPending = true;

    const auto& mode = *mPendingModeOpt->mode.modePtr;

    if (mHwComposer.setActiveModeWithConstraints(getPhysicalId(), mode.getHwcId(), constraints,
                                                 &outTimeline) != OK) {
        return false;
    }

    ATRACE_INT(mPendingModeFpsTrace.c_str(), mode.getVsyncRate().getIntValue());
    return true;
}

void DisplayDevice::finalizeModeChange(DisplayModeId modeId, Fps vsyncRate, Fps renderFps) {
    setActiveMode(modeId, vsyncRate, renderFps);
    mIsModeSetPending = false;
}

nsecs_t DisplayDevice::getVsyncPeriodFromHWC() const {
    const auto physicalId = getPhysicalId();
    if (!mHwComposer.isConnected(physicalId)) {
        return 0;
    }

    if (const auto vsyncPeriodOpt = mHwComposer.getDisplayVsyncPeriod(physicalId).value_opt()) {
        return *vsyncPeriodOpt;
    }

    return refreshRateSelector().getActiveMode().modePtr->getVsyncRate().getPeriodNsecs();
}

ui::Dataspace DisplayDevice::getCompositionDataSpace() const {
    return mCompositionDisplay->getState().dataspace;
}

void DisplayDevice::setLayerFilter(ui::LayerFilter filter) {
    mCompositionDisplay->setLayerFilter(filter);
    if (mRefreshRateOverlay) {
        mRefreshRateOverlay->setLayerStack(filter.layerStack);
    }
    if (mHdrSdrRatioOverlay) {
        mHdrSdrRatioOverlay->setLayerStack(filter.layerStack);
    }
}

void DisplayDevice::setFlags(uint32_t flags) {
    mFlags = flags;
}

void DisplayDevice::setDisplaySize(int width, int height) {
    LOG_FATAL_IF(!isVirtual(), "Changing the display size is supported only for virtual displays.");
    const auto size = ui::Size(width, height);
    mCompositionDisplay->setDisplaySize(size);
    if (mRefreshRateOverlay) {
        mRefreshRateOverlay->setViewport(size);
    }
    if (mHdrSdrRatioOverlay) {
        mHdrSdrRatioOverlay->setViewport(size);
    }
}

void DisplayDevice::setProjection(ui::Rotation orientation, Rect layerStackSpaceRect,
                                  Rect orientedDisplaySpaceRect) {
    mIsOrientationChanged = mOrientation != orientation;
    mOrientation = orientation;

#ifdef CONSOLE_MANAGER
    if (isPrimary()) {
        mConsoleManagerThread = sp<ConsoleManagerThread>::make(mFlinger, mDisplayToken);
    }
#endif

    // We need to take care of display rotation for globalTransform for case if the panel is not
    // installed aligned with device orientation.
    const auto transformOrientation = orientation + mPhysicalOrientation;

    const auto& state = getCompositionDisplay()->getState();

    // If the layer stack and destination frames have never been set, then configure them to be the
    // same as the physical device, taking into account the total transform.
    if (!orientedDisplaySpaceRect.isValid()) {
        ui::Size bounds = state.displaySpace.getBounds();
        bounds.rotate(transformOrientation);
        orientedDisplaySpaceRect = Rect(bounds);
    }
    if (layerStackSpaceRect.isEmpty()) {
        ui::Size bounds = state.framebufferSpace.getBounds();
        bounds.rotate(transformOrientation);
        layerStackSpaceRect = Rect(bounds);
    }
    getCompositionDisplay()->setProjection(transformOrientation, layerStackSpaceRect,
                                           orientedDisplaySpaceRect);
}

void DisplayDevice::stageBrightness(float brightness) {
    mStagedBrightness = brightness;
}

void DisplayDevice::persistBrightness(bool needsComposite) {
    if (mStagedBrightness && mBrightness != mStagedBrightness) {
        if (needsComposite) {
            getCompositionDisplay()->setNextBrightness(*mStagedBrightness);
        }
        mBrightness = *mStagedBrightness;
    }
    mStagedBrightness = std::nullopt;
}

std::optional<float> DisplayDevice::getStagedBrightness() const {
    return mStagedBrightness;
}

void DisplayDevice::dump(utils::Dumper& dumper) const {
    using namespace std::string_view_literals;

    dumper.dump("name"sv, '"' + mDisplayName + '"');
    dumper.dump("powerMode"sv, mPowerMode);

    if (mRefreshRateSelector) {
        mRefreshRateSelector->dump(dumper);
    }
}

bool DisplayDevice::hasRenderIntent(ui::RenderIntent intent) const {
    return mCompositionDisplay->getDisplayColorProfile()->hasRenderIntent(intent);
}

DisplayId DisplayDevice::getId() const {
    return mCompositionDisplay->getId();
}

bool DisplayDevice::isSecure() const {
    return mCompositionDisplay->isSecure();
}

void DisplayDevice::setSecure(bool secure) {
    mCompositionDisplay->setSecure(secure);
}

const Rect DisplayDevice::getBounds() const {
    return mCompositionDisplay->getState().displaySpace.getBoundsAsRect();
}

const Region& DisplayDevice::getUndefinedRegion() const {
    return mCompositionDisplay->getState().undefinedRegion;
}

ui::LayerStack DisplayDevice::getLayerStack() const {
    return mCompositionDisplay->getState().layerFilter.layerStack;
}

ui::Transform::RotationFlags DisplayDevice::getTransformHint() const {
    return mCompositionDisplay->getTransformHint();
}

const ui::Transform& DisplayDevice::getTransform() const {
    return mCompositionDisplay->getState().transform;
}

const Rect& DisplayDevice::getLayerStackSpaceRect() const {
    return mCompositionDisplay->getState().layerStackSpace.getContent();
}

const Rect& DisplayDevice::getOrientedDisplaySpaceRect() const {
    return mCompositionDisplay->getState().orientedDisplaySpace.getContent();
}

bool DisplayDevice::hasWideColorGamut() const {
    return mCompositionDisplay->getDisplayColorProfile()->hasWideColorGamut();
}

bool DisplayDevice::hasHDR10PlusSupport() const {
    return mCompositionDisplay->getDisplayColorProfile()->hasHDR10PlusSupport();
}

bool DisplayDevice::hasHDR10Support() const {
    return mCompositionDisplay->getDisplayColorProfile()->hasHDR10Support();
}

bool DisplayDevice::hasHLGSupport() const {
    return mCompositionDisplay->getDisplayColorProfile()->hasHLGSupport();
}

bool DisplayDevice::hasDolbyVisionSupport() const {
    return mCompositionDisplay->getDisplayColorProfile()->hasDolbyVisionSupport();
}

int DisplayDevice::getSupportedPerFrameMetadata() const {
    return mCompositionDisplay->getDisplayColorProfile()->getSupportedPerFrameMetadata();
}

void DisplayDevice::overrideHdrTypes(const std::vector<ui::Hdr>& hdrTypes) {
    mOverrideHdrTypes = hdrTypes;
}

HdrCapabilities DisplayDevice::getHdrCapabilities() const {
    const HdrCapabilities& capabilities =
            mCompositionDisplay->getDisplayColorProfile()->getHdrCapabilities();
    std::vector<ui::Hdr> hdrTypes = capabilities.getSupportedHdrTypes();
    if (!mOverrideHdrTypes.empty()) {
        hdrTypes = mOverrideHdrTypes;
    }
    return HdrCapabilities(hdrTypes, capabilities.getDesiredMaxLuminance(),
                           capabilities.getDesiredMaxAverageLuminance(),
                           capabilities.getDesiredMinLuminance());
}

void DisplayDevice::enableHdrSdrRatioOverlay(bool enable) {
    if (!enable) {
        mHdrSdrRatioOverlay.reset();
        return;
    }

    mHdrSdrRatioOverlay = HdrSdrRatioOverlay::create();
    if (mHdrSdrRatioOverlay) {
        mHdrSdrRatioOverlay->setLayerStack(getLayerStack());
        mHdrSdrRatioOverlay->setViewport(getSize());
        updateHdrSdrRatioOverlayRatio(mHdrSdrRatio);
    }
}

void DisplayDevice::updateHdrSdrRatioOverlayRatio(float currentHdrSdrRatio) {
    ATRACE_CALL();
    mHdrSdrRatio = currentHdrSdrRatio;
    if (mHdrSdrRatioOverlay) {
        mHdrSdrRatioOverlay->changeHdrSdrRatio(currentHdrSdrRatio);
    }
}

void DisplayDevice::enableRefreshRateOverlay(bool enable, bool setByHwc, bool showSpinner,
                                             bool showRenderRate, bool showInMiddle) {
    if (!enable) {
        mRefreshRateOverlay.reset();
        return;
    }

    ftl::Flags<RefreshRateOverlay::Features> features;
    if (showSpinner) {
        features |= RefreshRateOverlay::Features::Spinner;
    }

    if (showRenderRate) {
        features |= RefreshRateOverlay::Features::RenderRate;
    }

    if (showInMiddle) {
        features |= RefreshRateOverlay::Features::ShowInMiddle;
    }

    if (setByHwc) {
        features |= RefreshRateOverlay::Features::SetByHwc;
    }

    // TODO(b/296636258) Update to use the render rate range in VRR mode.
    const auto fpsRange = mRefreshRateSelector->getSupportedRefreshRateRange();
    mRefreshRateOverlay = RefreshRateOverlay::create(fpsRange, features);
    if (mRefreshRateOverlay) {
        mRefreshRateOverlay->setLayerStack(getLayerStack());
        mRefreshRateOverlay->setViewport(getSize());
        updateRefreshRateOverlayRate(getActiveMode().modePtr->getVsyncRate(), getActiveMode().fps,
                                     setByHwc);
    }
}

void DisplayDevice::updateRefreshRateOverlayRate(Fps vsyncRate, Fps renderFps, bool setByHwc) {
    ATRACE_CALL();
    if (mRefreshRateOverlay) {
        if (!mRefreshRateOverlay->isSetByHwc() || setByHwc) {
            mRefreshRateOverlay->changeRefreshRate(vsyncRate, renderFps);
        } else {
            mRefreshRateOverlay->changeRenderRate(renderFps);
        }
    }
}

bool DisplayDevice::onKernelTimerChanged(std::optional<DisplayModeId> desiredModeId,
                                         bool timerExpired) {
    if (mRefreshRateSelector && mRefreshRateOverlay) {
        const auto newMode =
                mRefreshRateSelector->onKernelTimerChanged(desiredModeId, timerExpired);
        if (newMode) {
            updateRefreshRateOverlayRate(newMode->modePtr->getVsyncRate(), newMode->fps);
            return true;
        }
    }

    return false;
}

void DisplayDevice::animateOverlay() {
    if (mRefreshRateOverlay) {
        mRefreshRateOverlay->animate();
    }
    if (mHdrSdrRatioOverlay) {
        // hdr sdr ratio is designed to be on the top right of the screen,
        // therefore, we need to re-calculate the display's width and height
        if (mIsOrientationChanged) {
            auto width = getWidth();
            auto height = getHeight();
            if (mOrientation == ui::ROTATION_90 || mOrientation == ui::ROTATION_270) {
                std::swap(width, height);
            }
            mHdrSdrRatioOverlay->setViewport({width, height});
        }
        mHdrSdrRatioOverlay->animate();
    }
}

auto DisplayDevice::setDesiredMode(display::DisplayModeRequest&& desiredMode) -> DesiredModeAction {
    ATRACE_NAME(concatId(__func__).c_str());
    ALOGD("%s %s", concatId(__func__).c_str(), to_string(desiredMode).c_str());

    std::scoped_lock lock(mDesiredModeLock);
    if (mDesiredModeOpt) {
        // A mode transition was already scheduled, so just override the desired mode.
        const bool emitEvent = mDesiredModeOpt->emitEvent;
        const bool force = mDesiredModeOpt->force;
        mDesiredModeOpt = std::move(desiredMode);
        mDesiredModeOpt->emitEvent |= emitEvent;
        if (FlagManager::getInstance().connected_display()) {
            mDesiredModeOpt->force |= force;
        }
        return DesiredModeAction::None;
    }

    // If the desired mode is already active...
    const auto activeMode = refreshRateSelector().getActiveMode();
    if (const auto& desiredModePtr = desiredMode.mode.modePtr;
        !desiredMode.force && activeMode.modePtr->getId() == desiredModePtr->getId()) {
        if (activeMode == desiredMode.mode) {
            return DesiredModeAction::None;
        }

        // ...but the render rate changed:
        setActiveMode(desiredModePtr->getId(), desiredModePtr->getVsyncRate(),
                      desiredMode.mode.fps);
        return DesiredModeAction::InitiateRenderRateSwitch;
    }

    // Set the render frame rate to the active physical refresh rate to schedule the next
    // frame as soon as possible.
    setActiveMode(activeMode.modePtr->getId(), activeMode.modePtr->getVsyncRate(),
                  activeMode.modePtr->getVsyncRate());

    // Initiate a mode change.
    mDesiredModeOpt = std::move(desiredMode);
    mHasDesiredModeTrace = true;
    return DesiredModeAction::InitiateDisplayModeSwitch;
}

auto DisplayDevice::getDesiredMode() const -> DisplayModeRequestOpt {
    std::scoped_lock lock(mDesiredModeLock);
    return mDesiredModeOpt;
}

void DisplayDevice::clearDesiredMode() {
    std::scoped_lock lock(mDesiredModeLock);
    mDesiredModeOpt.reset();
    mHasDesiredModeTrace = false;
}

void DisplayDevice::adjustRefreshRate(Fps pacesetterDisplayRefreshRate) {
    using fps_approx_ops::operator<=;
    if (mRequestedRefreshRate <= 0_Hz) {
        return;
    }

    using fps_approx_ops::operator>;
    if (mRequestedRefreshRate > pacesetterDisplayRefreshRate) {
        mAdjustedRefreshRate = pacesetterDisplayRefreshRate;
        return;
    }

    unsigned divisor = static_cast<unsigned>(
            std::floor(pacesetterDisplayRefreshRate.getValue() / mRequestedRefreshRate.getValue()));
    if (divisor == 0) {
        mAdjustedRefreshRate = 0_Hz;
        return;
    }

    mAdjustedRefreshRate = pacesetterDisplayRefreshRate / divisor;
}

std::atomic<int32_t> DisplayDeviceState::sNextSequenceId(1);

}  // namespace android

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic pop // ignored "-Wconversion"
