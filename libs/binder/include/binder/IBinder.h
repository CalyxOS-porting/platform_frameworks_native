/*
 * Copyright (C) 2008 The Android Open Source Project
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

#pragma once

#include <binder/unique_fd.h>
#include <utils/Errors.h>
#include <utils/RefBase.h>
#include <utils/String16.h>
#include <utils/Vector.h>

#include <functional>

// linux/binder.h defines this, but we don't want to include it here in order to
// avoid exporting the kernel headers
#ifndef B_PACK_CHARS
#define B_PACK_CHARS(c1, c2, c3, c4) \
    ((((c1)<<24)) | (((c2)<<16)) | (((c3)<<8)) | (c4))
#endif  // B_PACK_CHARS

// ---------------------------------------------------------------------------
namespace android {

class BBinder;
class BpBinder;
class IInterface;
class Parcel;
class IResultReceiver;
class IShellCallback;

/**
 * Base class and low-level protocol for a remotable object.
 * You can derive from this class to create an object for which other
 * processes can hold references to it.  Communication between processes
 * (method calls, property get and set) is down through a low-level
 * protocol implemented on top of the transact() API.
 */
class [[clang::lto_visibility_public]] IBinder : public virtual RefBase
{
public:
    enum {
        FIRST_CALL_TRANSACTION = 0x00000001,
        LAST_CALL_TRANSACTION = 0x00ffffff,

        PING_TRANSACTION = B_PACK_CHARS('_', 'P', 'N', 'G'),
        START_RECORDING_TRANSACTION = B_PACK_CHARS('_', 'S', 'R', 'D'),
        STOP_RECORDING_TRANSACTION = B_PACK_CHARS('_', 'E', 'R', 'D'),
        DUMP_TRANSACTION = B_PACK_CHARS('_', 'D', 'M', 'P'),
        SHELL_COMMAND_TRANSACTION = B_PACK_CHARS('_', 'C', 'M', 'D'),
        INTERFACE_TRANSACTION = B_PACK_CHARS('_', 'N', 'T', 'F'),
        SYSPROPS_TRANSACTION = B_PACK_CHARS('_', 'S', 'P', 'R'),
        EXTENSION_TRANSACTION = B_PACK_CHARS('_', 'E', 'X', 'T'),
        DEBUG_PID_TRANSACTION = B_PACK_CHARS('_', 'P', 'I', 'D'),
        SET_RPC_CLIENT_TRANSACTION = B_PACK_CHARS('_', 'R', 'P', 'C'),

        // See android.os.IBinder.TWEET_TRANSACTION
        // Most importantly, messages can be anything not exceeding 130 UTF-8
        // characters, and callees should exclaim "jolly good message old boy!"
        TWEET_TRANSACTION = B_PACK_CHARS('_', 'T', 'W', 'T'),

        // See android.os.IBinder.LIKE_TRANSACTION
        // Improve binder self-esteem.
        LIKE_TRANSACTION = B_PACK_CHARS('_', 'L', 'I', 'K'),

        // Corresponds to TF_ONE_WAY -- an asynchronous call.
        FLAG_ONEWAY = 0x00000001,

        // Corresponds to TF_CLEAR_BUF -- clear transaction buffers after call
        // is made
        FLAG_CLEAR_BUF = 0x00000020,

        // Private userspace flag for transaction which is being requested from
        // a vendor context.
        FLAG_PRIVATE_VENDOR = 0x10000000,
    };

    IBinder();

    /**
     * Check if this IBinder implements the interface named by
     * @a descriptor.  If it does, the base pointer to it is returned,
     * which you can safely static_cast<> to the concrete C++ interface.
     */
    virtual sp<IInterface>  queryLocalInterface(const String16& descriptor);

    /**
     * Return the canonical name of the interface provided by this IBinder
     * object.
     */
    virtual const String16& getInterfaceDescriptor() const = 0;

    virtual bool            isBinderAlive() const = 0;
    virtual status_t        pingBinder() = 0;
    virtual status_t        dump(int fd, const Vector<String16>& args) = 0;
    static  status_t        shellCommand(const sp<IBinder>& target, int in, int out, int err,
                                         Vector<String16>& args, const sp<IShellCallback>& callback,
                                         const sp<IResultReceiver>& resultReceiver);

    /**
     * This allows someone to add their own additions to an interface without
     * having to modify the original interface.
     *
     * For instance, imagine if we have this interface:
     *     interface IFoo { void doFoo(); }
     *
     * If an unrelated owner (perhaps in a downstream codebase) wants to make a
     * change to the interface, they have two options:
     *
     * A). Historical option that has proven to be BAD! Only the original
     *     author of an interface should change an interface. If someone
     *     downstream wants additional functionality, they should not ever
     *     change the interface or use this method.
     *
     *    BAD TO DO:  interface IFoo {                       BAD TO DO
     *    BAD TO DO:      void doFoo();                      BAD TO DO
     *    BAD TO DO: +    void doBar(); // adding a method   BAD TO DO
     *    BAD TO DO:  }                                      BAD TO DO
     *
     * B). Option that this method enables!
     *     Leave the original interface unchanged (do not change IFoo!).
     *     Instead, create a new interface in a downstream package:
     *
     *         package com.<name>; // new functionality in a new package
     *         interface IBar { void doBar(); }
     *
     *     When registering the interface, add:
     *         sp<MyFoo> foo = new MyFoo; // class in AOSP codebase
     *         sp<MyBar> bar = new MyBar; // custom extension class
     *         foo->setExtension(bar);    // use method in BBinder
     *
     *     Then, clients of IFoo can get this extension:
     *         sp<IBinder> binder = ...;
     *         sp<IFoo> foo = interface_cast<IFoo>(binder); // handle if null
     *         sp<IBinder> barBinder;
     *         ... handle error ... = binder->getExtension(&barBinder);
     *         sp<IBar> bar = interface_cast<IBar>(barBinder);
     *         // if bar is null, then there is no extension or a different
     *         // type of extension
     */
    status_t                getExtension(sp<IBinder>* out);

    /**
     * Dump PID for a binder, for debugging.
     */
    status_t                getDebugPid(pid_t* outPid);

    /**
     * Set the RPC client fd to this binder service, for debugging. This is only available on
     * debuggable builds.
     *
     * When this is called on a binder service, the service:
     * 1. sets up RPC server
     * 2. spawns 1 new thread that calls RpcServer::join()
     *    - join() spawns some number of threads that accept() connections; see RpcServer
     *
     * setRpcClientDebug() may be called multiple times. Each call will add a new RpcServer
     * and opens up a TCP port.
     *
     * Note: A thread is spawned for each accept()'ed fd, which may call into functions of the
     * interface freely. See RpcServer::join(). To avoid such race conditions, implement the service
     * functions with multithreading support.
     *
     * On death of @a keepAliveBinder, the RpcServer shuts down.
     */
    [[nodiscard]] status_t setRpcClientDebug(binder::unique_fd socketFd,
                                             const sp<IBinder>& keepAliveBinder);

    // NOLINTNEXTLINE(google-default-arguments)
    virtual status_t        transact(   uint32_t code,
                                        const Parcel& data,
                                        Parcel* reply,
                                        uint32_t flags = 0) = 0;

    // DeathRecipient is pure abstract, there is no virtual method
    // implementation to put in a translation unit in order to silence the
    // weak vtables warning.
    #if defined(__clang__)
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wweak-vtables"
    #endif

    class DeathRecipient : public virtual RefBase
    {
    public:
        virtual void binderDied(const wp<IBinder>& who) = 0;
    };

    class FrozenStateChangeCallback : public virtual RefBase {
    public:
        enum class State {
            FROZEN,
            UNFROZEN,
        };
        virtual void onStateChanged(const wp<IBinder>& who, State state) = 0;
    };

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

    /**
     * Register the @a recipient for a notification if this binder
     * goes away.  If this binder object unexpectedly goes away
     * (typically because its hosting process has been killed),
     * then DeathRecipient::binderDied() will be called with a reference
     * to this.
     *
     * The @a cookie is optional -- if non-NULL, it should be a
     * memory address that you own (that is, you know it is unique).
     *
     * @note When all references to the binder being linked to are dropped, the
     * recipient is automatically unlinked. So, you must hold onto a binder in
     * order to receive death notifications about it.
     *
     * @note You will only receive death notifications for remote binders,
     * as local binders by definition can't die without you dying as well.
     * Trying to use this function on a local binder will result in an
     * INVALID_OPERATION code being returned and nothing happening.
     *
     * @note This link always holds a weak reference to its recipient.
     *
     * @note You will only receive a weak reference to the dead
     * binder.  You should not try to promote this to a strong reference.
     * (Nor should you need to, as there is nothing useful you can
     * directly do with it now that it has passed on.)
     */
    // NOLINTNEXTLINE(google-default-arguments)
    virtual status_t        linkToDeath(const sp<DeathRecipient>& recipient,
                                        void* cookie = nullptr,
                                        uint32_t flags = 0) = 0;

    /**
     * Remove a previously registered death notification.
     * The @a recipient will no longer be called if this object
     * dies.  The @a cookie is optional.  If non-NULL, you can
     * supply a NULL @a recipient, and the recipient previously
     * added with that cookie will be unlinked.
     *
     * If the binder is dead, this will return DEAD_OBJECT. Deleting
     * the object will also unlink all death recipients.
     */
    // NOLINTNEXTLINE(google-default-arguments)
    virtual status_t        unlinkToDeath(  const wp<DeathRecipient>& recipient,
                                            void* cookie = nullptr,
                                            uint32_t flags = 0,
                                            wp<DeathRecipient>* outRecipient = nullptr) = 0;

    /**
     * addFrozenStateChangeCallback provides a callback mechanism to notify
     * about process frozen/unfrozen events. Upon registration and any
     * subsequent state changes, the callback is invoked with the latest process
     * frozen state.
     *
     * If the listener process (the one using this API) is itself frozen, state
     * change events might be combined into a single one with the latest state.
     * (meaning 'frozen, unfrozen' might just be 'unfrozen'). This single event
     * would then be delivered when the listener process becomes unfrozen.
     * Similarly, if an event happens before the previous event is consumed,
     * they might be combined. This means the callback might not be called for
     * every single state change, so don't rely on this API to count how many
     * times the state has changed.
     *
     * @note When all references to the binder are dropped, the callback is
     * automatically removed. So, you must hold onto a binder in order to
     * receive notifications about it.
     *
     * @note You will only receive freeze notifications for remote binders, as
     * local binders by definition can't be frozen without you being frozen as
     * well. Trying to use this function on a local binder will result in an
     * INVALID_OPERATION code being returned and nothing happening.
     *
     * @note This binder always holds a weak reference to the callback.
     *
     * @note You will only receive a weak reference to the binder object. You
     * should not try to promote this to a strong reference. (Nor should you
     * need to, as there is nothing useful you can directly do with it now that
     * it has passed on.)
     */
    [[nodiscard]] status_t addFrozenStateChangeCallback(
            const wp<FrozenStateChangeCallback>& callback);

    /**
     * Remove a previously registered freeze callback.
     * The @a callback will no longer be called if this object
     * changes its frozen state.
     */
    [[nodiscard]] status_t removeFrozenStateChangeCallback(
            const wp<FrozenStateChangeCallback>& callback);

    virtual bool            checkSubclass(const void* subclassID) const;

    typedef void (*object_cleanup_func)(const void* id, void* obj, void* cleanupCookie);

    /**
     * This object is attached for the lifetime of this binder object. When
     * this binder object is destructed, the cleanup function of all attached
     * objects are invoked with their respective objectID, object, and
     * cleanupCookie. Access to these APIs can be made from multiple threads,
     * but calls from different threads are allowed to be interleaved.
     *
     * This returns the object which is already attached. If this returns a
     * non-null value, it means that attachObject failed (a given objectID can
     * only be used once).
     */
    [[nodiscard]] virtual void* attachObject(const void* objectID, void* object,
                                             void* cleanupCookie, object_cleanup_func func) = 0;
    /**
     * Returns object attached with attachObject.
     */
    [[nodiscard]] virtual void* findObject(const void* objectID) const = 0;
    /**
     * Returns object attached with attachObject, and detaches it. This does not
     * delete the object.
     */
    [[nodiscard]] virtual void* detachObject(const void* objectID) = 0;

    /**
     * Use the lock that this binder contains internally. For instance, this can
     * be used to modify an attached object without needing to add an additional
     * lock (though, that attached object must be retrieved before calling this
     * method). Calling (most) IBinder methods inside this will deadlock.
     */
    void withLock(const std::function<void()>& doWithLock);

    virtual BBinder*        localBinder();
    virtual BpBinder*       remoteBinder();
    typedef sp<IBinder> (*object_make_func)(const void* makeArgs);
    sp<IBinder> lookupOrCreateWeak(const void* objectID, object_make_func make,
                                   const void* makeArgs);

protected:
    virtual          ~IBinder();

private:
};

} // namespace android

// ---------------------------------------------------------------------------
