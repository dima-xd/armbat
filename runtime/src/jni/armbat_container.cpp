#include <jni.h>

#include <android/log.h>

#include "armbat/log.h"

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "armbat/guest_container.h"
#include "armbat/guest_input.h"
#include "armbat/guest_stdio.h"
#include "armbat/jni_bridge.h"

namespace {

std::mutex g_mutex;
std::unique_ptr<armbat::GuestContainer> g_container;

// Whether the archive's native world is up. A touch can only arrive once its
// window exists, and the flag keeps the UI thread out of the container's mutex:
// a lifecycle callback may be running guest code on another thread, and the
// finger must not wait for it.
std::atomic<bool> g_input_live{false};

// A zygote child's standard streams go to /dev/null, so guest printf output is
// only visible in logcat if we copy it there ourselves. The sink is installed
// here rather than in the launch path because every guest write goes through it.
void InstallLogSink() {
    static std::once_flag installed;
    std::call_once(installed, [] {
        armbat::SetStdioSink([](int fd, std::string_view text) {
            armbat::LogText(fd == 2 ? armbat::LogLevel::Error : armbat::LogLevel::Info, "Guest",
                            text);
        });
    });
}
std::string g_archive;
std::string g_cache;
std::string g_loaded;
std::string g_last_report;

// logcat splits on newlines anyway; doing it here keeps one guest line per
// record instead of one giant blob.
void LogLines(const std::string& text) {
    ARMBAT_LOG_TEXT(armbat::LogLevel::Info, "Armbat", text);
}

std::string StringOf(JNIEnv* env, jstring value) {
    if (value == nullptr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) {
        return {};
    }
    std::string text(chars);
    env->ReleaseStringUTFChars(value, chars);
    return text;
}

// Maps every ARMv7 library of the archive once. A second visit would map a
// second copy and run its initializers again.
std::string EnsureLoaded(const std::string& apk, const std::string& cache,
                         std::uint32_t target_sdk, const std::string& data_root,
                         const std::string& obb_root, const std::string& package_name) {
    if (g_container && apk == g_loaded) {
        return g_last_report;
    }
    if (!g_container) {
        g_container = std::make_unique<armbat::GuestContainer>();
    }
    // Before Start: the kernel's path mapper and the native activity's paths are
    // built from these, and neither is rebuilt once the guest is up.
    g_container->SetGuestStorage(data_root, obb_root, package_name);
    std::string error;
    std::string text;
    try {
        if (!g_container->Start(&error)) {
            text = "error: " + error + "\n";
        } else {
            text = g_container->LoadApk(apk, cache, target_sdk, &error);
            g_loaded = error.empty() ? apk : std::string();
        }
    } catch (const std::exception& failure) {
        text = std::string("error: ") + failure.what() + "\n";
    }
    g_last_report = text;
    LogLines(text);
    return text;
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_dev_armbat_GuestNatives_loadArchive(JNIEnv* env, jobject /*self*/, jstring apk_path,
                                         jstring cache_dir, jint target_sdk, jstring data_root,
                                         jstring obb_root, jstring package_name) {
    const std::string apk = StringOf(env, apk_path);
    const std::string cache = StringOf(env, cache_dir);
    if (apk.empty()) {
        return env->NewStringUTF("error: missing APK path\n");
    }

    InstallLogSink();

    const std::lock_guard lock(g_mutex);
    g_archive = apk;
    g_cache = cache;
    const std::string text = EnsureLoaded(
        apk, cache, static_cast<std::uint32_t>(target_sdk < 0 ? 0 : target_sdk),
        StringOf(env, data_root), StringOf(env, obb_root), StringOf(env, package_name));
    return env->NewStringUTF(text.c_str());
}

// Binds the archive's Java_* entry points to its classes. The guest loader is
// the one that defines them, so it is the oracle for class lookup as well.
extern "C" JNIEXPORT jstring JNICALL
Java_dev_armbat_GuestNatives_registerNatives(JNIEnv* env, jobject /*self*/, jobject loader) {
    const std::lock_guard lock(g_mutex);
    if (!g_container) {
        return env->NewStringUTF("natives: nothing loaded\n");
    }
    std::string text;
    try {
        const auto started = std::chrono::steady_clock::now();
        text = g_container->Jni().RegisterExports(env, loader);
        const auto registered = std::chrono::steady_clock::now();
        text += g_container->CallOnLoad();
        const auto on_load_done = std::chrono::steady_clock::now();
        ARMBAT_LOG_INFO("Armbat",
            "startup: natives registered at %lld ms, JNI_OnLoad done at %lld ms",
            static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(registered - started).count()),
            static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(on_load_done - started).count()));
    } catch (const std::exception& failure) {
        text = std::string("natives: error: ") + failure.what() + "\n";
    }
    LogLines(text);
    return env->NewStringUTF(text.c_str());
}

// Called by the container's stand-in for android.app.NativeActivity once the// guest's Java part has reached onCreate. This is where the archive's own
// ANativeActivity_onCreate will run, under the JIT, with the surface and the
// asset manager the container hands it.
extern "C" JNIEXPORT void JNICALL
Java_dev_armbat_GuestNatives_create(JNIEnv* env, jobject /*self*/, jobject /*surface*/) {
    const std::lock_guard lock(g_mutex);
    ARMBAT_LOG_INFO("Armbat", "guest native activity requested");
    if (g_container == nullptr || !g_container->Started()) {
        ARMBAT_LOG_ERROR("Armbat", "native activity: no container");
        return;
    }
    const std::string report = g_container->StartNativeActivity(std::string());
    LogLines(report);
    g_input_live.store(true, std::memory_order_relaxed);
}

// The host window behind the guest's surface. ANativeWindow_fromSurface is the
// platform's own way in, and it keeps the surface's lifetime in the framework's
// hands instead of the container's.
extern "C" JNIEXPORT void JNICALL
Java_dev_armbat_GuestNatives_windowCreated(JNIEnv* env, jobject /*self*/, jobject surface) {
    using FromSurface = void* (*)(JNIEnv*, jobject);
    static const auto from_surface = [] {
        void* library = ::dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        return library != nullptr
                   ? reinterpret_cast<FromSurface>(::dlsym(library, "ANativeWindow_fromSurface"))
                   : nullptr;
    }();

    const std::lock_guard lock(g_mutex);
    if (g_container == nullptr || !g_container->Started()) {
        return;
    }
    void* window = (surface != nullptr && from_surface != nullptr)
                       ? from_surface(env, surface)
                       : nullptr;
    ARMBAT_LOG_INFO("Armbat", "guest window: %p", window);
    LogLines(g_container->SetNativeWindow(window));
}

extern "C" JNIEXPORT void JNICALL
Java_dev_armbat_GuestNatives_windowDestroyed(JNIEnv*, jobject /*self*/) {
    const std::lock_guard lock(g_mutex);
    if (g_container == nullptr || !g_container->Started()) {
        return;
    }
    LogLines(g_container->SetNativeWindow(nullptr));
}

namespace {

// MotionEvent's accessors, looked up once. Touches arrive up to 120 times a
// second and each one crosses JNI a dozen times, so the lookups cannot repeat.
struct MotionAccessors {
    jmethodID action = nullptr;
    jmethodID pointer_count = nullptr;
    jmethodID pointer_id = nullptr;
    jmethodID x = nullptr;
    jmethodID y = nullptr;
    jmethodID raw_x = nullptr;
    jmethodID raw_y = nullptr;
    jmethodID pressure = nullptr;
    jmethodID size = nullptr;
    jmethodID source = nullptr;
    jmethodID device_id = nullptr;
    jmethodID meta_state = nullptr;
    jmethodID flags = nullptr;
    jmethodID edge_flags = nullptr;
    jmethodID down_time = nullptr;
    jmethodID event_time = nullptr;
};

const MotionAccessors& Accessors(JNIEnv* env, jobject event) {
    static MotionAccessors accessors;
    if (accessors.pointer_count != nullptr) {
        return accessors;
    }
    jclass type = env->GetObjectClass(event);
    if (type == nullptr) {
        return accessors;
    }
    accessors.action = env->GetMethodID(type, "getAction", "()I");
    accessors.pointer_count = env->GetMethodID(type, "getPointerCount", "()I");
    accessors.pointer_id = env->GetMethodID(type, "getPointerId", "(I)I");
    accessors.x = env->GetMethodID(type, "getX", "(I)F");
    accessors.y = env->GetMethodID(type, "getY", "(I)F");
    accessors.raw_x = env->GetMethodID(type, "getRawX", "(I)F");
    accessors.raw_y = env->GetMethodID(type, "getRawY", "(I)F");
    accessors.pressure = env->GetMethodID(type, "getPressure", "(I)F");
    accessors.size = env->GetMethodID(type, "getSize", "(I)F");
    accessors.source = env->GetMethodID(type, "getSource", "()I");
    accessors.device_id = env->GetMethodID(type, "getDeviceId", "()I");
    accessors.meta_state = env->GetMethodID(type, "getMetaState", "()I");
    accessors.flags = env->GetMethodID(type, "getFlags", "()I");
    accessors.edge_flags = env->GetMethodID(type, "getEdgeFlags", "()I");
    accessors.down_time = env->GetMethodID(type, "getDownTime", "()J");
    accessors.event_time = env->GetMethodID(type, "getEventTime", "()J");
    env->DeleteLocalRef(type);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    return accessors;
}

}  // namespace

// Every touch the guest's window receives, already translated into the
// archive's own pixels by the guest frame. A native activity reads its input
// through AInputQueue, never through the view it drew on, so the event is
// queued there and the archive's own loop picks it up -- the same path the
// platform gives a native activity on a device.
extern "C" JNIEXPORT void JNICALL
Java_dev_armbat_GuestNatives_touch(JNIEnv* env, jobject /*self*/, jobject event) {
    if (event == nullptr || !g_input_live.load(std::memory_order_relaxed)) {
        return;
    }
    const MotionAccessors& methods = Accessors(env, event);
    if (methods.action == nullptr || methods.x == nullptr || methods.raw_x == nullptr ||
        methods.pointer_count == nullptr) {
        return;
    }
    armbat::InputEvent input;
    input.type = 2;  // AINPUT_EVENT_TYPE_MOTION
    input.action = env->CallIntMethod(event, methods.action);
    input.source = env->CallIntMethod(event, methods.source);
    input.device_id = env->CallIntMethod(event, methods.device_id);
    input.meta_state = env->CallIntMethod(event, methods.meta_state);
    input.flags = env->CallIntMethod(event, methods.flags);
    input.edge_flags = env->CallIntMethod(event, methods.edge_flags);
    input.down_time = env->CallLongMethod(event, methods.down_time);
    input.event_time = env->CallLongMethod(event, methods.event_time);
    const jint count = env->CallIntMethod(event, methods.pointer_count);
    for (jint index = 0; index < count && index < 10; ++index) {
        armbat::InputPointer pointer;
        pointer.id = env->CallIntMethod(event, methods.pointer_id, index);
        pointer.x = env->CallFloatMethod(event, methods.x, index);
        pointer.y = env->CallFloatMethod(event, methods.y, index);
        // A native activity whose window covers the display sees raw and local
        // coordinates in the same space; the guest's window is the whole virtual
        // screen, so they agree here as well. The event's own raw coordinates
        // describe the phone and are of no use to the guest: forwarding them
        // made every position outside the letterbox land somewhere else.
        pointer.raw_x = pointer.x;
        pointer.raw_y = pointer.y;
        pointer.pressure = env->CallFloatMethod(event, methods.pressure, index);
        pointer.size = env->CallFloatMethod(event, methods.size, index);
        input.pointers.push_back(pointer);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return;
    }
    if (input.pointers.empty()) {
        return;
    }
    armbat::GuestInputQueue::Instance().Push(std::move(input));
}

// Android's activity lifecycle, forwarded to the archive's callbacks in the
// order the platform sends them.
extern "C" JNIEXPORT void JNICALL
Java_dev_armbat_GuestNatives_lifecycle(JNIEnv* env, jobject /*self*/, jint event) {
    const std::lock_guard lock(g_mutex);
    if (g_container == nullptr || !g_container->Started()) {
        return;
    }
    if (event == 6) {
        // onWindowFocusChanged takes a boolean.
        LogLines(g_container->NotifyNative(6, 1, true));
    } else if (event >= 0 && event <= 5) {
        LogLines(g_container->NotifyNative(event, 0, false));
    } else {
        ARMBAT_LOG_INFO("Armbat", "lifecycle: unknown event %d",
                            static_cast<int>(event));
    }
    (void)env;
}

namespace {

// Writes a reference field the platform would normally have set on an Activity.
//
// The platform denies reflection on these fields to a caller that targets a
// current SDK (max-target-o). The write is therefore made from a thread
// attached to the VM: ART builds a JNI access context from the first Java frame
// on the stack and treats a thread without one as trusted, which the non-SDK
// policy lets through. Called from a class of this application the same lookup
// and write are denied.
bool WriteTrustedField(JNIEnv* env, jobject target, const char* name, const char* signature,
                       jobject value) {
    if (target == nullptr || value == nullptr) {
        return false;
    }
    JavaVM* vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK) {
        return false;
    }
    jclass target_class = env->GetObjectClass(target);
    if (target_class == nullptr) {
        return false;
    }
    auto target_class_ref = static_cast<jclass>(env->NewGlobalRef(target_class));
    jobject target_ref = env->NewGlobalRef(target);
    jobject value_ref = env->NewGlobalRef(value);
    env->DeleteLocalRef(target_class);
    std::atomic<bool> written{false};
    std::thread worker([&] {
        JNIEnv* worker_env = nullptr;
        if (vm->AttachCurrentThreadAsDaemon(&worker_env, nullptr) != JNI_OK) {
            return;
        }
        jfieldID field = worker_env->GetFieldID(target_class_ref, name, signature);
        if (field != nullptr) {
            worker_env->SetObjectField(target_ref, field, value_ref);
            written = worker_env->ExceptionCheck() == JNI_FALSE;
        }
        if (worker_env->ExceptionCheck()) {
            worker_env->ExceptionDescribe();
            worker_env->ExceptionClear();
        }
        vm->DetachCurrentThread();
    });
    worker.join();
    env->DeleteGlobalRef(value_ref);
    env->DeleteGlobalRef(target_ref);
    env->DeleteGlobalRef(target_class_ref);
    return written;
}

}  // namespace

// Writes Activity.mUiThread.
//
// Activity.runOnUiThread compares against it: with the field null every
// runOnUiThread only posts. Work an archive queues from onCreate then lands
// after onResume, and an archive that resumes with what that work was supposed
// to publish dies there -- Jetpack Joyride's Facebook client dereferences the
// singleton its posted runnable creates.
extern "C" JNIEXPORT jboolean JNICALL
Java_dev_armbat_GuestNatives_setActivityUiThread(JNIEnv* env, jclass /*self*/, jobject activity,
                                                 jobject thread) {
    return WriteTrustedField(env, activity, "mUiThread", "Ljava/lang/Thread;", thread)
               ? JNI_TRUE
               : JNI_FALSE;
}

// Writes Activity.mParent, which is what makes an archive's activity a child of
// the container's one.
extern "C" JNIEXPORT jboolean JNICALL
Java_dev_armbat_GuestNatives_setActivityParent(JNIEnv* env, jclass /*self*/, jobject activity,
                                               jobject parent) {
    return WriteTrustedField(env, activity, "mParent", "Landroid/app/Activity;", parent)
               ? JNI_TRUE
               : JNI_FALSE;
}
