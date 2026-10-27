#pragma once

#include <jni.h>

#include <cstdint>
#include <memory>
#include <string>

namespace armbat {

class GuestContainer;

// The two directions between the framework and the guest world.
//
// The Java part of an archive runs on the host runtime, but its native methods
// live in the archive's ARMv7 libraries. A host call into guest code arrives
// through a generated arm64 trampoline: one per method, because the runtime binds
// every native method to its own address. A guest call back into the framework
// goes through a JNIEnv table of guest stubs that trap into the host.
//
// The guest cannot hold a host pointer, and the tables make sure it never has to.
class JniBridge {
public:
    explicit JniBridge(GuestContainer& container);
    ~JniBridge();

    JniBridge(const JniBridge&) = delete;
    JniBridge& operator=(const JniBridge&) = delete;

    // Builds the guest-side JNIEnv and JavaVM tables and hooks the trap handler.
    bool Prepare(std::string* error);

    // Registers every Java_* entry point of the mapped libraries as a native
    // method of the class it names. `loader` is the archive's class loader.
    // Problems with individual classes are reported, not fatal: an archive may
    // carry code for SDKs the container does not have.
    std::string RegisterExports(JNIEnv* env, jobject loader);

    // The JNIEnv a guest native method sees, and a JavaVM for JNI_OnLoad.
    std::uint32_t GuestEnv() const;
    std::uint32_t GuestVm() const;

    // The host environment, and the host reference behind a guest object handle.
    // Platform entry points that take a Java object receive one of these handles
    // -- ANativeWindow_fromSurface is the one every Java-based engine calls, and
    // without it the engine never gets a window to draw on -- so only the bridge
    // can turn it back into something the host can use.
    JNIEnv* HostEnvironment() const;
    jobject ResolveGuestObject(std::uint32_t handle) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A guest thread that reached the framework was attached to the Java runtime by
// the first JNI call it made. A host thread that exits while attached leaves the
// runtime waiting for it, so the thread's last act is to detach.
void DetachHostThread();

// The bridge of the running container. There is one guest world per process, and
// a platform import that is handed a Java object has no other way to reach the
// tables that give the object meaning.
JniBridge* CurrentJniBridge();

}  // namespace armbat
