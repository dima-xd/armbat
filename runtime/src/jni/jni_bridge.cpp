#include "armbat/jni_bridge.h"

#include <android/log.h>

#include "armbat/log.h"
#include <unistd.h>

#include <atomic>
#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "armbat/a32_machine.h"
#include "armbat/dynamic_loader.h"
#include "armbat/elf32_loader.h"
#include "armbat/guest_arena.h"
#include "armbat/guest_container.h"
#include "armbat/guest_heap.h"
#include "armbat/guest_modules.h"
#include "armbat/hle.h"

namespace armbat {
namespace {

// Trap numbers used by the guest-side JNI tables. The HLE registry owns the low
// numbers; everything above this base is the bridge.
constexpr std::uint32_t kJniEnvBase = 0x8000;
constexpr std::uint32_t kJniVmBase = 0x9000;

constexpr std::uint32_t kJniTableSize = 240;

// One arm64 trampoline is 72 bytes; the stride keeps them 16-byte aligned so the
// literal pools stay aligned as well.
constexpr std::size_t kTrampolineStride = 96;
constexpr std::size_t kTrampolineCount = 512;

JavaVM* g_vm = nullptr;

// The bridge the container created. Platform imports are registered on the
// container's registry, not on the bridge, and they need the bridge's object
// tables to make sense of a Java reference the guest is holding.
std::atomic<JniBridge*> g_bridge{nullptr};

struct PendingRef {
    std::uint32_t handle = 0;
    // A slot can be freed by the guest and handed out again inside the same
    // native call; the generation tells the two apart.
    std::uint32_t generation = 0;
    // Whether the guest owns this reference (NewGlobalRef) or the call does.
    bool guest_owned = false;
};

// Handles the bridge hands out while one native call runs. They are the guest's
// local references: they die when that call returns, as they would in a real
// JNI frame. Each entry is a handle index; the stack nests because a Java
// method the guest called can call back into another native.
thread_local std::vector<std::vector<PendingRef>> g_local_frames;

// The handle a native just returned. Its reference has to outlive the native
// call itself -- ART turns it into a Java value after the call returns -- so it
// is released when this thread enters the next native call.
thread_local PendingRef g_deferred_return;

void Log(const std::string& line) {
    ARMBAT_LOG_TEXT(LogLevel::Info, "Armbat", line);
}

JNIEnv* HostEnv() {
    if (g_vm == nullptr) {
        return nullptr;
    }
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
        return env;
    }
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        return nullptr;
    }
    return env;
}

std::string ArgString(HleCall& call, std::uint32_t address) {
    std::string text;
    call.ReadCString(address, text, 8192);
    return text;
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

// Array access is type-dispatched once, here, so the registrations below stay
// one line per entry point. The guest never sees a Java array: it gets a block
// of the HLE data area, and Release copies it back.
template <typename T>
void HostGetArrayRegion(JNIEnv* env, jarray array, jsize start, jsize length, T* buffer) {
    if constexpr (std::is_same_v<T, jboolean>) {
        env->GetBooleanArrayRegion(static_cast<jbooleanArray>(array), start, length, buffer);
    } else if constexpr (std::is_same_v<T, jbyte>) {
        env->GetByteArrayRegion(static_cast<jbyteArray>(array), start, length, buffer);
    } else if constexpr (std::is_same_v<T, jchar>) {
        env->GetCharArrayRegion(static_cast<jcharArray>(array), start, length, buffer);
    } else if constexpr (std::is_same_v<T, jshort>) {
        env->GetShortArrayRegion(static_cast<jshortArray>(array), start, length, buffer);
    } else if constexpr (std::is_same_v<T, jint>) {
        env->GetIntArrayRegion(static_cast<jintArray>(array), start, length, buffer);
    } else if constexpr (std::is_same_v<T, jlong>) {
        env->GetLongArrayRegion(static_cast<jlongArray>(array), start, length, buffer);
    } else if constexpr (std::is_same_v<T, jfloat>) {
        env->GetFloatArrayRegion(static_cast<jfloatArray>(array), start, length, buffer);
    } else if constexpr (std::is_same_v<T, jdouble>) {
        env->GetDoubleArrayRegion(static_cast<jdoubleArray>(array), start, length, buffer);
    }
}

template <typename T>
void HostSetArrayRegion(JNIEnv* env, jarray array, jsize start, jsize length, const T* buffer) {
    if constexpr (std::is_same_v<T, jboolean>) {
        env->SetBooleanArrayRegion(static_cast<jbooleanArray>(array), start, length, buffer);
    } else if constexpr (std::is_same_v<T, jbyte>) {
        env->SetByteArrayRegion(static_cast<jbyteArray>(array), start, length, buffer);
    } else if constexpr (std::is_same_v<T, jchar>) {
        env->SetCharArrayRegion(static_cast<jcharArray>(array), start, length, buffer);
    } else if constexpr (std::is_same_v<T, jshort>) {
        env->SetShortArrayRegion(static_cast<jshortArray>(array), start, length, buffer);
    } else if constexpr (std::is_same_v<T, jint>) {
        env->SetIntArrayRegion(static_cast<jintArray>(array), start, length, buffer);
    } else if constexpr (std::is_same_v<T, jlong>) {
        env->SetLongArrayRegion(static_cast<jlongArray>(array), start, length, buffer);
    } else if constexpr (std::is_same_v<T, jfloat>) {
        env->SetFloatArrayRegion(static_cast<jfloatArray>(array), start, length, buffer);
    } else if constexpr (std::is_same_v<T, jdouble>) {
        env->SetDoubleArrayRegion(static_cast<jdoubleArray>(array), start, length, buffer);
    }
}

template <typename T>
jarray HostNewArray(JNIEnv* env, jsize length) {
    if constexpr (std::is_same_v<T, jboolean>) {
        return env->NewBooleanArray(length);
    } else if constexpr (std::is_same_v<T, jbyte>) {
        return env->NewByteArray(length);
    } else if constexpr (std::is_same_v<T, jchar>) {
        return env->NewCharArray(length);
    } else if constexpr (std::is_same_v<T, jshort>) {
        return env->NewShortArray(length);
    } else if constexpr (std::is_same_v<T, jint>) {
        return env->NewIntArray(length);
    } else if constexpr (std::is_same_v<T, jlong>) {
        return env->NewLongArray(length);
    } else if constexpr (std::is_same_v<T, jfloat>) {
        return env->NewFloatArray(length);
    } else if constexpr (std::is_same_v<T, jdouble>) {
        return env->NewDoubleArray(length);
    }
}

// JNI function index inside struct JNINativeInterface, computed from the header
// so the table cannot drift from the ABI.
#define JNI_IDX(name) (offsetof(struct JNINativeInterface, name) / sizeof(void*))
#define JNI_VM_IDX(name) (offsetof(struct JNIInvokeInterface, name) / sizeof(void*))

std::string BinaryName(const std::string& descriptor) {
    std::string name = descriptor;
    if (!name.empty() && name[0] == 'L' && name.back() == ';') {
        name = name.substr(1, name.size() - 2);
    }
    // JNI FindClass takes "java/lang/String", loadClass takes
    // "java.lang.String". Arrays keep their descriptor form and are resolved by
    // Class.forName below.
    std::replace(name.begin(), name.end(), '/', '.');
    return name;
}

std::string DecodeMangled(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '_') {
            out += text[i];
            continue;
        }
        if (i + 1 < text.size()) {
            const char next = text[i + 1];
            if (next == '1') {
                out += '_';
                i += 1;
                continue;
            }
            if (next == '2') {
                out += ';';
                i += 1;
                continue;
            }
            if (next == '3') {
                out += '[';
                i += 1;
                continue;
            }
        }
        out += '/';
    }
    return out;
}

// Splits a Java_* symbol into its package segments, the method name and, when the
// method is overloaded, the mangled signature. `_1`, `_2` and `_3` are escapes,
// a plain `_` separates names and `__` introduces the signature.
bool SplitJavaSymbol(const std::string& symbol, std::vector<std::string>* segments,
                     std::string* signature) {
    if (symbol.rfind("Java_", 0) != 0) {
        return false;
    }
    std::string current;
    std::size_t i = 5;
    while (i < symbol.size()) {
        const char c = symbol[i];
        if (c != '_') {
            current += c;
            ++i;
            continue;
        }
        if (i + 1 < symbol.size()) {
            const char next = symbol[i + 1];
            if (next == '1') {
                current += '_';
                i += 2;
                continue;
            }
            if (next == '2') {
                current += ';';
                i += 2;
                continue;
            }
            if (next == '3') {
                current += '[';
                i += 2;
                continue;
            }
            if (next == '_') {
                // Overloaded method: the rest is the mangled signature.
                if (!current.empty()) {
                    segments->push_back(current);
                }
                *signature = DecodeMangled(symbol.substr(i + 2));
                return !segments->empty();
            }
        }
        if (!current.empty()) {
            segments->push_back(current);
        }
        current.clear();
        ++i;
    }
    if (!current.empty()) {
        segments->push_back(current);
    }
    return segments->size() >= 2;
}

}  // namespace

void DetachHostThread() {
    if (g_vm == nullptr) {
        return;
    }
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        return;
    }
    g_vm->DetachCurrentThread();
}

struct JniBridge::Impl {
    explicit Impl(GuestContainer& world)
        : container(world),
          machine(world.Machine()),
          arena(world.Arena()),
          imports(world.Imports()) {}

    struct NativeMethod {
        std::uint32_t entry = 0;
        std::string signature;
    };

    struct MethodRef {
        jmethodID method = nullptr;
        std::string name;
        std::string signature;
        bool is_static = false;
    };

    struct FieldRef {
        jfieldID field = nullptr;
        std::string signature;
        bool is_static = false;
    };

    // The bridge whose trampolines are executing. There is one guest world per
    // process, so one instance.
    static Impl* current;

    GuestContainer& container;
    A32Machine& machine;
    GuestArena& arena;
    HleRegistry& imports;

    // Guest references. Index 0 is null, so a handle is 1-based. A slot whose
    // object is null is free and can be reused; a global entry belongs to the
    // guest until it calls DeleteGlobalRef.
    struct Ref {
        jobject object = nullptr;
        bool guest_owned = false;
        std::uint32_t generation = 0;
        bool deleted = false;
        // Set when the guest resolves the handle, cleared by every budget pass.
        // A reference the guest keeps using survives; one it made and left
        // behind is dropped on the second pass.
        mutable bool touched = false;
    };
    std::vector<Ref> refs;
    std::vector<std::uint32_t> free_refs;
    std::uint32_t next_generation = 0;
    std::unordered_map<jobject, std::uint32_t> ref_ids;

    // Local references the guest has handed out and not deleted. The platform
    // frees them when the native call that made them returns; the container
    // cannot see every such point -- a native activity's own event loop makes
    // JNI calls without ever entering Java -- and code that counts on the frame
    // (Unity builds two arrays per touch event and deletes only the class
    // references) would otherwise fill ART's global table and abort the process.
    // The budget gives those references the lifetime the platform gives them:
    // when too many are alive, the ones the guest has stopped resolving go,
    // and a cache it keeps using survives.
    static constexpr std::size_t kLocalRefBudget = 16384;
    // References made in the last few calls are never dropped: a routine creates
    // an argument array and uses it two calls later, and the budget runs before
    // that use.
    static constexpr std::size_t kLocalRefRecent = 4096;
    std::vector<std::uint32_t> local_refs;   // guarded by refs_mutex
    std::vector<std::uint32_t> dropped_refs_;   // guarded by refs_mutex
    std::atomic<std::size_t> local_ref_count{0};
    std::atomic<std::size_t> local_ref_history{0};
    std::vector<MethodRef> method_refs;
    // Real JNI answers the same jmethodID for the same method for the life of the
    // VM. Callers key their own caches by it (Unity's proxy bridge does), so a
    // fresh index per call turns a cached method into an unknown one.
    std::unordered_map<jmethodID, std::uint32_t> method_ids;
    std::vector<FieldRef> field_refs;

    jobject loader = nullptr;
    jmethodID loader_load_class = nullptr;

    std::vector<std::function<void(HleCall&)>> jni_calls;
    std::vector<std::function<void(HleCall&)>> vm_calls;
    std::vector<bool> warned;
    std::vector<bool> seen;
    int main_calls = 0;

    std::uint32_t env_pointer = 0;
    std::uint32_t vm_pointer = 0;

    std::uint8_t* trampolines = nullptr;
    std::size_t trampolines_used = 0;
    bool trampolines_sealed = false;
    // A deque, not a vector: a native method that is running can make its own
    // library register more natives (Unity's libunity does exactly this the
    // second time JNI_OnLoad runs), and a vector reallocation would leave the
    // caller holding a reference to freed storage. Registration then decided
    // the return convention from a dangling string and ART received 0 instead
    // of the guest's answer.
    std::deque<NativeMethod> natives;

    // A direct buffer Java allocated lives in host memory that only the JVM can
    // address, while the guest can address nothing but the arena. The guest
    // therefore gets an arena copy of it, refreshed when it asks for the
    // address and written back once control passes to Java. The key is the host
    // address: it names the memory itself, so a buffer the collector frees and a
    // later one reuses the same block still maps to the right contents.
    struct DirectMirror {
        std::byte* host = nullptr;
        std::uint32_t guest = 0;
        std::uint64_t capacity = 0;
    };
    std::vector<DirectMirror> mirrors;
    std::mutex mirrors_mutex;
    // Read without the lock on every JNI call: the answer to "is there anything
    // to write back" is no nearly always, and a lock there would sit on the path
    // every guest JNI call takes.
    std::atomic<std::size_t> mirror_count{0};
    bool mirror_refused = false;

    std::mutex mutex;

    // Guest threads call JNI concurrently: the reference tables are shared, and
    // an unsynchronized push_back here reallocates host memory under another
    // thread's feet (that is how a guest pointer ended up inside the loader).
    mutable std::recursive_mutex refs_mutex;

    // ---- references -------------------------------------------------------

    // Called from an env stub; diagnostics name the stub that produced a null.
    // Guest threads call JNI at the same time, so these are per-thread: a shared
    // string names a null after whichever call happened to run last elsewhere,
    // which is worse than no name at all.
    inline static thread_local std::size_t current_env_index = 0;
    inline static thread_local std::uint32_t current_env_lr = 0;
    inline static thread_local std::string current_call;

    // Names the throwable the framework left pending, if any. A pending
    // exception makes every later JNI call return zero, so it has to show up in
    // the log next to the value it spoiled.
    static std::string PendingException(JNIEnv* env) {
        if (!env->ExceptionCheck()) {
            return std::string();
        }
        jthrowable thrown = env->ExceptionOccurred();
        std::string name = "?";
        jclass type = thrown != nullptr ? env->GetObjectClass(thrown) : nullptr;
        if (type != nullptr) {
            // getName belongs to Class; on the throwable it raises
            // NoSuchMethodError, which would replace the exception being named.
            jclass class_class = env->GetObjectClass(type);
            jmethodID get_name =
                class_class != nullptr
                    ? env->GetMethodID(class_class, "getName", "()Ljava/lang/String;")
                    : nullptr;
            if (get_name != nullptr) {
                auto text = static_cast<jstring>(env->CallObjectMethod(type, get_name));
                if (text != nullptr) {
                    const char* chars = env->GetStringUTFChars(text, nullptr);
                    if (chars != nullptr) {
                        name = chars;
                        env->ReleaseStringUTFChars(text, chars);
                    }
                }
            }
            if (class_class != nullptr) {
                env->DeleteLocalRef(class_class);
            }
            env->DeleteLocalRef(type);
        }
        // The guest decides what to do with a framework failure, exactly as on a
        // device: keep the exception pending, only describe it here.
        return name;
    }

    // Guest native code cannot handle a Java exception: there is no catch block
    // in C, and code of the archive's era almost never checks for one. Left
    // pending, the throwable follows the guest into its next JNI call, where
    // CheckJNI ends the whole process -- and that next call could not have done
    // anything useful with an exception pending anyway. The framework's refusal
    // is therefore logged and dropped, and the call that follows answers what
    // the platform of that era answered: null. ExceptionCheck and the other
    // three inspection entries are exempt, so a guest that looks first still
    // sees it.
    static void DropPendingException(JNIEnv* env, std::size_t next_index) {
        if (!env->ExceptionCheck()) {
            return;
        }
        jthrowable thrown = env->ExceptionOccurred();
        env->ExceptionClear();
        if (thrown == nullptr) {
            return;
        }
        // With the throwable no longer pending, the calls below are ordinary
        // JNI. `getName` is a method of Class, not of the throwable, and asking
        // for it on the throwable itself raises a NoSuchMethodError that would
        // replace the exception this log line is about.
        std::string type_name = "?";
        std::string message;
        jclass type = env->GetObjectClass(thrown);
        if (type != nullptr) {
            jclass class_class = env->GetObjectClass(type);
            if (class_class != nullptr) {
                jmethodID get_name =
                    env->GetMethodID(class_class, "getName", "()Ljava/lang/String;");
                if (get_name != nullptr) {
                    auto text = static_cast<jstring>(env->CallObjectMethod(type, get_name));
                    type_name = StringOf(env, text);
                    if (text != nullptr) {
                        env->DeleteLocalRef(text);
                    }
                }
                env->DeleteLocalRef(class_class);
            }
            jmethodID get_message = env->GetMethodID(type, "getMessage", "()Ljava/lang/String;");
            if (get_message != nullptr) {
                auto text = static_cast<jstring>(env->CallObjectMethod(thrown, get_message));
                message = StringOf(env, text);
                if (text != nullptr) {
                    env->DeleteLocalRef(text);
                }
            }
            env->DeleteLocalRef(type);
        }
        if (LogAllowed("jni-pending", 16)) {
            ARMBAT_LOG_WARN("Armbat",
                                "jni: dropped %s%s%s from the guest's native side before call #%zu",
                                type_name.c_str(), message.empty() ? "" : ": ", message.c_str(),
                                next_index);
        }
        env->DeleteLocalRef(thrown);
    }

    std::uint32_t Handle(JNIEnv* env, jobject object) {
        if (env->ExceptionCheck()) {
            // CheckJNI kills the process when a global ref is made with a pending
            // exception; a device running the same app non-debuggable would just
            // keep it pending. Hand the exception back to the guest and answer
            // with a null handle.
            jthrowable thrown = env->ExceptionOccurred();
            env->ExceptionClear();
            if (thrown != nullptr) {
                env->Throw(thrown);
                env->DeleteLocalRef(thrown);
            }
            return 0;
        }
        if (object == nullptr) {
            // A null from ExceptionOccurred means "no exception", which is the
            // normal answer, not a failure worth a log line. The rest is a
            // bounded sample: a guest that reads optional files produces them by
            // the dozen and each one buries a line that matters.
            if (current_env_index != 0 && current_env_index != JNI_IDX(ExceptionOccurred) &&
                LogAllowed("jni-null", 40)) {
                const std::string thrown = PendingException(env);
                if (thrown.empty()) {
                    ARMBAT_LOG_ERROR("Armbat",
                                        "jni: #%zu at 0x%08x produced a null object%s%s",
                                        current_env_index, current_env_lr,
                                        current_call.empty() ? "" : " from ", current_call.c_str());
                } else {
                    ARMBAT_LOG_ERROR("Armbat",
                                        "jni: #%zu at 0x%08x produced a null object%s%s with %s",
                                        current_env_index, current_env_lr,
                                        current_call.empty() ? "" : " from ", current_call.c_str(),
                                        thrown.c_str());
                }
            }
            return 0;
        }
        const std::lock_guard lock(refs_mutex);
        const auto known = ref_ids.find(object);
        if (known != ref_ids.end()) {
            // ART answers a different reference for the same object on different
            // calls, and a reference slot can name another object once the frame
            // that owned it pops, so a key is verified and not trusted.
            if (env->IsSameObject(refs[known->second - 1].object, object)) {
                return known->second;
            }
            ref_ids.erase(known);
        }
        // Every handle is a real global reference. The guest hands handles to
        // other threads and gives them back to Java after the call that made
        // them (Unity's proxy return does exactly that); a local reference is
        // dead in both cases.
        jobject kept = env->NewGlobalRef(object);
        if (kept == nullptr) {
            ARMBAT_LOG_ERROR("Armbat",
                                "jni: #%zu could not make a ref of %p (refs=%zu)",
                                current_env_index, static_cast<void*>(object), refs.size());
            return 0;
        }
        const std::uint32_t handle = InsertRef(kept, false);
        ref_ids.emplace(kept, handle);
        if (!g_local_frames.empty()) {
            g_local_frames.back().push_back(
                PendingRef{handle, refs[handle - 1].generation, false});
        }
        local_refs.push_back(handle);
        local_ref_count.fetch_add(1, std::memory_order_relaxed);
        local_ref_history.store(local_refs.size(), std::memory_order_relaxed);
        return handle;
    }

    // A handle the guest keeps for the life of the process. The reference is
    // made in the caller's frame but never released by it: the platform hands
    // an activity object to native code once and the archive uses it from every
    // thread afterwards.
    std::uint32_t PersistentHandle(JNIEnv* env, jobject object) {
        if (object == nullptr) {
            return 0;
        }
        const std::lock_guard lock(refs_mutex);
        const auto known = ref_ids.find(object);
        if (known != ref_ids.end() &&
            env->IsSameObject(refs[known->second - 1].object, object)) {
            return known->second;
        }
        jobject kept = env->NewGlobalRef(object);
        if (kept == nullptr) {
            return 0;
        }
        const std::uint32_t handle = InsertRef(kept, true);
        ref_ids.emplace(kept, handle);
        return handle;
    }

    // Caller holds refs_mutex.
    std::uint32_t InsertRef(jobject object, bool guest_owned) {
        ++next_generation;
        if (!free_refs.empty()) {
            const std::uint32_t handle = free_refs.back();
            free_refs.pop_back();
            refs[handle - 1] = Ref{object, guest_owned, next_generation, false};
            return handle;
        }
        refs.push_back(Ref{object, guest_owned, next_generation, false});
        return static_cast<std::uint32_t>(refs.size());
    }

    // Frees one handle. A handle of the other kind was not ours to free: the
    // guest is deleting a reference it never owned. A generation of zero means
    // "whatever occupies the slot now", which is what an explicit
    // DeleteLocalRef/DeleteGlobalRef from the guest asks for.
    void ReleaseRef(std::uint32_t handle, bool guest_owned, std::uint32_t generation = 0) {
        JNIEnv* env = HostEnv();
        if (env == nullptr) {
            return;
        }
        const std::lock_guard lock(refs_mutex);
        ReleaseRefLocked(handle, guest_owned, generation);
    }

    // Caller holds refs_mutex. A local reference leaves the live count as it
    // goes: the budget is about how many are alive, not how many were ever made.
    void ReleaseRefLocked(std::uint32_t handle, bool guest_owned,
                          std::uint32_t generation = 0) {
        JNIEnv* env = HostEnv();
        if (env == nullptr || handle == 0 || handle > refs.size()) {
            return;
        }
        Ref& entry = refs[handle - 1];
        if (entry.object == nullptr || entry.guest_owned != guest_owned) {
            return;
        }
        if (generation != 0 && entry.generation != generation) {
            return;
        }
        env->DeleteGlobalRef(entry.object);
        ref_ids.erase(entry.object);
        entry = Ref{};
        if (!guest_owned) {
            const std::size_t live = local_ref_count.load(std::memory_order_relaxed);
            if (live > 0) {
                local_ref_count.store(live - 1, std::memory_order_relaxed);
            }
        }
        // The slot is deliberately not recycled. A guest that keeps a handle
        // past the call that made it is broken, and the cheapest way to say so
        // is a null reference: a recycled slot would hand it a live but
        // unrelated object, which reads as a random type error deep inside the
        // app. The table grows by one Ref per handle the guest asked for, which
        // a normal session never notices.
        (void)free_refs;
    }

    // A reference the guest deletes is released when the call that deleted it
    // returns, not at the delete itself. Unity's proxy return deletes the boxed
    // result and then hands the same reference back to Java; a device without
    // CheckJNI reads the freed slot and gets away with it, so the container has
    // to keep the reference alive until the call is over.
    void MarkDeleted(std::uint32_t handle, bool guest_owned) {
        const std::lock_guard lock(refs_mutex);
        if (handle == 0 || handle > refs.size()) {
            return;
        }
        Ref& entry = refs[handle - 1];
        if (entry.object == nullptr || entry.guest_owned != guest_owned || entry.deleted) {
            return;
        }
        entry.deleted = true;
        ref_ids.erase(entry.object);
        if (!g_local_frames.empty()) {
            g_local_frames.back().push_back(PendingRef{handle, entry.generation, guest_owned});
            return;
        }
        ReleaseRef(handle, guest_owned, entry.generation);
    }

    // Drops the local references the guest stopped using once its list is over
    // budget, oldest first. Pure global references are never touched: the guest
    // owns those. Nothing is dropped while the guest holds fewer than the budget,
    // so a normal session never sees this run at all.
    void TrimLocalRefs() {
        // Trimmed only once the table is well past the budget, so the scan is
        // rare: a call that runs every time the count sits at the limit turns
        // the whole JNI path into a table walk. The history list is compacted
        // on its own, larger limit: released slots stay in it until then.
        static constexpr std::size_t kSlack = 4096;
        static constexpr std::size_t kListLimit = 65536;
        const std::size_t live_now = local_ref_count.load(std::memory_order_relaxed);
        const bool over_budget = live_now > kLocalRefBudget + kSlack;
        if (!over_budget &&
            local_ref_history.load(std::memory_order_relaxed) <= kListLimit) {
            return;
        }
        std::size_t kept_count = 0;
        std::size_t dropped_count = 0;
        dropped_refs_.clear();
        {
            const std::lock_guard lock(refs_mutex);
            // One pass, in place: the list keeps its order (creation order) and
            // the dropped handles are released below the same lock.
            std::size_t write = 0;
            const std::size_t recent_from =
                local_refs.size() > kLocalRefRecent ? local_refs.size() - kLocalRefRecent : 0;
            for (std::size_t i = 0; i < local_refs.size(); ++i) {
                const std::uint32_t handle = local_refs[i];
                if (handle == 0 || handle > refs.size()) {
                    continue;
                }
                Ref& entry = refs[handle - 1];
                if (entry.object == nullptr || entry.guest_owned) {
                    continue;   // already released by the guest
                }
                // A compaction pass drops nothing: only a table over budget may
                // retire a reference the guest has not resolved lately.
                if (!over_budget || i >= recent_from || entry.touched) {
                    entry.touched = false;
                    local_refs[write++] = handle;
                } else {
                    dropped_refs_.push_back(handle);
                }
            }
            local_refs.resize(write);
            for (const std::uint32_t handle : dropped_refs_) {
                ReleaseRefLocked(handle, false);
            }
            kept_count = local_refs.size();
            dropped_count = dropped_refs_.size();
            local_ref_count.store(kept_count, std::memory_order_relaxed);
            local_ref_history.store(kept_count, std::memory_order_relaxed);
        }
        if (dropped_count != 0 && LogAllowed("jni-budget", 8)) {
            ARMBAT_LOG_INFO("Armbat",
                            "jni: released %zu local references the guest never deleted (%zu "
                            "kept)",
                            dropped_count, kept_count);
        }
    }

    // Drops every reference the native call that just returned had handed out.
    // Without this each proxy callback left a Method, an Object[], a Long and a
    // Class behind until ART's global table overflowed. `base` is the depth the
    // call started at: a guest that pushed a frame and never popped it is
    // cleaned up here as well, and its references cannot outlive the call's.
    void ReleaseLocalFrame(std::size_t base) {
        while (g_local_frames.size() > base) {
            std::vector<PendingRef> pending = std::move(g_local_frames.back());
            g_local_frames.pop_back();
            for (const PendingRef& ref : pending) {
                ReleaseRef(ref.handle, ref.guest_owned, ref.generation);
            }
        }
    }

    // Takes the returned handle out of the call's frame: it must stay valid one
    // step longer than everything else this call created, because ART turns it
    // into a Java value after the call returns.
    void DeferReturn(std::uint32_t handle) {
        if (handle == 0 || g_local_frames.empty()) {
            return;
        }
        std::vector<PendingRef>& frame = g_local_frames.back();
        bool found = false;
        for (auto it = frame.begin(); it != frame.end();) {
            if (it->handle == handle) {
                g_deferred_return = *it;
                it = frame.erase(it);
                found = true;
                continue;
            }
            ++it;
        }
        (void)found;
    }

    jobject Resolve(std::uint32_t handle) const {
        const std::lock_guard lock(refs_mutex);
        if (handle == 0 || handle > refs.size()) {
            if (handle != 0) {
                ARMBAT_LOG_ERROR("Armbat",
                                    "jni: #%zu at 0x%08x has a bad handle %u (refs=%zu)",
                                    current_env_index, current_env_lr, handle, refs.size());
            }
            return nullptr;
        }
        refs[handle - 1].touched = true;
        return refs[handle - 1].object;
    }

    // ---- guest tables -----------------------------------------------------

    std::uint32_t MakeTrap(std::uint32_t svc) {
        const std::uint32_t stub = imports.AllocateData(8);
        if (stub == 0) {
            return 0;
        }
        auto* code = reinterpret_cast<std::uint32_t*>(arena.Ptr(stub));
        code[0] = 0xEF000000u | (svc & 0x00FFFFFFu);
        code[1] = 0xE12FFF1Eu;  // bx lr
        return stub;
    }

    std::uint32_t BuildTable(std::uint32_t base, std::size_t count) {
        const std::uint32_t table = imports.AllocateData(static_cast<std::uint32_t>((4 + count) * 4));
        if (table == 0) {
            return 0;
        }
        auto* slots = reinterpret_cast<std::uint32_t*>(arena.Ptr(table));
        // Element n of the array is function n of the interface: the guest
        // indexes the table directly, and a trap must dispatch the handler
        // registered under that same number.
        for (std::size_t i = 0; i < count; ++i) {
            slots[i] = MakeTrap(base + static_cast<std::uint32_t>(i));
        }
        return table;
    }

    // ---- direct buffers ---------------------------------------------------

    // The arena address of a direct buffer's contents. A buffer that already
    // lives in the arena (one the guest made with NewDirectByteBuffer) answers
    // with its own offset; a buffer Java allocated is copied into an arena block
    // the guest can read, and remembered so the copy is written back before Java
    // looks at it again.
    //
    // Copying in on every request, not once per buffer, is what keeps the two
    // sides in step: Java refills its buffer between two native calls, and the
    // guest has to see the bytes Java put there. A buffer of the same size that
    // the collector frees and the allocator reuses is the same memory, so the
    // existing mirror is refreshed and reused instead of leaking a new one.
    std::uint32_t DirectBufferAddress(JNIEnv* env, jobject buffer) {
        auto* host = static_cast<std::byte*>(env->GetDirectBufferAddress(buffer));
        if (host == nullptr) {
            return 0;
        }
        const auto bytes = reinterpret_cast<std::uintptr_t>(host);
        const auto base = reinterpret_cast<std::uintptr_t>(arena.Base());
        if (bytes >= base && bytes < base + arena.Size()) {
            return static_cast<std::uint32_t>(bytes - base);
        }
        const jlong capacity = env->GetDirectBufferCapacity(buffer);
        if (capacity <= 0) {
            return 0;
        }
        const std::uint64_t bytes_needed = DirectBufferBytes(env, buffer, capacity);
        if (bytes_needed == 0 || bytes_needed > 0x7fffffffULL) {
            return 0;
        }
        const std::lock_guard lock(mirrors_mutex);
        for (const DirectMirror& mirror : mirrors) {
            if (mirror.host != host || mirror.capacity != bytes_needed) {
                continue;
            }
            std::memcpy(arena.Ptr(mirror.guest), host, mirror.capacity);
            return mirror.guest;
        }
        const std::uint32_t guest = container.Heap().Allocate(static_cast<std::uint32_t>(bytes_needed));
        if (guest == 0) {
            // Answering null is what the old code did for every host buffer, and
            // the guest dereferences it; there is nothing better to hand out.
            if (!mirror_refused) {
                mirror_refused = true;
                ARMBAT_LOG_WARN("Armbat",
                                "jni: no arena room to mirror a %lld-byte direct buffer",
                                static_cast<long long>(bytes_needed));
            }
            return 0;
        }
        std::memcpy(arena.Ptr(guest), host, static_cast<std::size_t>(bytes_needed));
        mirrors.push_back(DirectMirror{host, guest, bytes_needed});
        mirror_count.store(mirrors.size(), std::memory_order_release);
        return guest;
    }

    // JNI counts a direct buffer's capacity in elements, not bytes: an IntBuffer
    // of 4096 names a 16 KiB region. A guest native that receives one writes
    // four bytes per element, so a mirror sized by the capacity alone would be
    // overrun. The class name is the only place the element width is visible.
    std::uint64_t DirectBufferBytes(JNIEnv* env, jobject buffer, jlong capacity) {
        const std::uint64_t elements = static_cast<std::uint64_t>(capacity);
        jclass type = env->GetObjectClass(buffer);
        if (type == nullptr) {
            return elements;
        }
        const std::string name = ClassDescriptor(env, type);
        env->DeleteLocalRef(type);
        std::uint64_t width = 1;
        if (name.find("Buffer") != std::string::npos) {
            if (name.find("ShortBuffer") != std::string::npos ||
                name.find("CharBuffer") != std::string::npos) {
                width = 2;
            } else if (name.find("IntBuffer") != std::string::npos ||
                       name.find("FloatBuffer") != std::string::npos) {
                width = 4;
            } else if (name.find("LongBuffer") != std::string::npos ||
                       name.find("DoubleBuffer") != std::string::npos) {
                width = 8;
            }
        }
        return elements * width;
    }

    // Writes every mirrored buffer back into Java's memory. Called where control
    // passes to Java: at the end of a native method Java called and before a
    // Java method the guest calls. Without a boundary the guest's writes would
    // stay in the arena copy and Java would read the data from before them.
    void FlushDirectBuffers() {
        if (mirror_count.load(std::memory_order_acquire) == 0) {
            return;
        }
        const std::lock_guard lock(mirrors_mutex);
        for (const DirectMirror& mirror : mirrors) {
            std::memcpy(mirror.host, arena.Ptr(mirror.guest), mirror.capacity);
        }
    }

    bool Prepare(std::string* error) {
        jni_calls.assign(kJniTableSize, nullptr);
        seen.assign(kJniTableSize, false);
        vm_calls.assign(8, nullptr);
        warned.assign(kJniTableSize, false);

        const std::uint32_t env_table = BuildTable(kJniEnvBase, kJniTableSize);
        if (env_table == 0) {
            *error = "cannot build the guest JNIEnv table";
            return false;
        }
        env_pointer = imports.AllocateData(4);
        *reinterpret_cast<std::uint32_t*>(arena.Ptr(env_pointer)) = env_table;

        const std::uint32_t vm_table = BuildTable(kJniVmBase, vm_calls.size());
        if (vm_table == 0) {
            *error = "cannot build the guest JavaVM table";
            return false;
        }
        vm_pointer = imports.AllocateData(4);
        *reinterpret_cast<std::uint32_t*>(arena.Ptr(vm_pointer)) = vm_table;

        void* code = mmap(nullptr, kTrampolineStride * kTrampolineCount, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (code == MAP_FAILED) {
            *error = "cannot reserve trampolines";
            return false;
        }
        trampolines = static_cast<std::uint8_t*>(code);
        ARMBAT_LOG_INFO("Armbat",
                            "jni: trampolines at %p (%zu x %zu bytes)", code,
                            static_cast<std::size_t>(kTrampolineCount),
                            static_cast<std::size_t>(kTrampolineStride));

        InstallFunctions();
        return true;
    }

    // ---- host side of the guest environment --------------------------------

    void InstallFunctions() {
        const auto add = [this](std::size_t index, std::function<void(HleCall&)> function) {
            if (index < jni_calls.size()) {
                jni_calls[index] = std::move(function);
            }
        };
        const auto add_vm = [this](std::size_t index, std::function<void(HleCall&)> function) {
            if (index < vm_calls.size()) {
                vm_calls[index] = std::move(function);
            }
        };

        add(JNI_IDX(GetVersion), [](HleCall& call) { call.Return(JNI_VERSION_1_6); });

        add(JNI_IDX(FindClass), [this](HleCall& call) {
            const std::string name = ArgString(call, call.Arg(1));
            const jclass type = GuestClass(name);
            if (type == nullptr) {
                ARMBAT_LOG_INFO("Armbat", "jni: no class %s", name.c_str());
            }
            call.Return(Handle(HostEnv(), type));
        });

        add(JNI_IDX(GetObjectClass), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            jobject object = Resolve(call.Arg(1));
            if (object == nullptr) {
                call.Return(0);
                return;
            }
            call.Return(Handle(env, env->GetObjectClass(object)));
        });

        add(JNI_IDX(GetSuperclass), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            jclass type = static_cast<jclass>(Resolve(call.Arg(1)));
            if (type == nullptr) {
                call.Return(0);
                return;
            }
            call.Return(Handle(env, env->GetSuperclass(type)));
        });

        add(JNI_IDX(IsAssignableFrom), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            call.Return(env->IsAssignableFrom(static_cast<jclass>(Resolve(call.Arg(1))),
                                              static_cast<jclass>(Resolve(call.Arg(2))))
                            ? 1
                            : 0);
        });

        add(JNI_IDX(IsInstanceOf), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            call.Return(env->IsInstanceOf(Resolve(call.Arg(1)),
                                          static_cast<jclass>(Resolve(call.Arg(2))))
                            ? 1
                            : 0);
        });

        add(JNI_IDX(IsSameObject), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            call.Return(env->IsSameObject(Resolve(call.Arg(1)), Resolve(call.Arg(2))) ? 1 : 0);
        });

        add(JNI_IDX(NewGlobalRef), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            jobject object = Resolve(call.Arg(1));
            if (object == nullptr) {
                call.Return(0);
                return;
            }
            // The guest owns this one until DeleteGlobalRef, so it must not be
            // collected with the locals of the call that created it.
            jobject global = env->NewGlobalRef(object);
            if (global == nullptr) {
                call.Return(0);
                return;
            }
            const std::lock_guard lock(refs_mutex);
            const std::uint32_t handle = InsertRef(global, true);
            ref_ids.emplace(global, handle);
            call.Return(handle);
        });
        add(JNI_IDX(NewLocalRef), [this](HleCall& call) {
            call.Return(Handle(HostEnv(), Resolve(call.Arg(1))));
        });
        add(JNI_IDX(DeleteGlobalRef), [this](HleCall& call) {
            MarkDeleted(call.Arg(1), true);
        });
        add(JNI_IDX(DeleteLocalRef), [this](HleCall& call) {
            MarkDeleted(call.Arg(1), false);
        });
        add(JNI_IDX(EnsureLocalCapacity), [](HleCall& call) { call.Return(0); });
        add(JNI_IDX(PushLocalFrame), [](HleCall& call) {
            // A frame of the guest's own: everything it creates until the pop is
            // released there, the way ART releases it. Without it a routine that
            // counts on the frame -- native input code builds a couple of arrays
            // per event and relies on exactly this -- leaves every reference
            // behind until the global table overflows.
            g_local_frames.emplace_back();
            call.Return(static_cast<std::uint32_t>(JNI_OK));
        });
        add(JNI_IDX(PopLocalFrame), [this](HleCall& call) {
            // PopLocalFrame returns the object it was asked to keep, and that one
            // must outlive the frame it came from: it is handed back in the frame
            // below, which is where a device puts it too.
            jobject kept = Resolve(call.Arg(1));
            if (g_local_frames.empty()) {
                call.Return(call.Arg(1));
                return;
            }
            ReleaseLocalFrame(g_local_frames.size() - 1);
            call.Return(kept == nullptr ? 0u : Handle(HostEnv(), kept));
        });

        add(JNI_IDX(NewStringUTF), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            call.Return(Handle(env, env->NewStringUTF(ArgString(call, call.Arg(1)).c_str())));
        });

        add(JNI_IDX(GetStringUTFLength), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            call.Return(static_cast<std::uint32_t>(env->GetStringUTFLength(
                static_cast<jstring>(Resolve(call.Arg(1))))));
        });

        add(JNI_IDX(GetStringLength), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            call.Return(
                static_cast<std::uint32_t>(env->GetStringLength(static_cast<jstring>(Resolve(call.Arg(1))))));
        });

        add(JNI_IDX(GetStringUTFChars), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            const std::uint32_t handle = call.Arg(1);
            const jstring text = static_cast<jstring>(Resolve(handle));
            if (text == nullptr) {
                // The guest asked for the characters of a null string. JNI marks
                // that as undefined, and on a device it takes the thread down:
                // here it did exactly that, in the middle of a level load. Mono
                // kept the dead thread in its list, the next collection stopped
                // the world for a thread that could never answer, and the whole
                // guest parked in futex with nothing left to restart it.
                //
                // A pointer is only null on failure, so the empty string is the
                // honest answer to a caller that lost its reference: the copy it
                // makes is empty, and it stays alive to finish its work.
                ARMBAT_LOG_WARN("Armbat",
                                    "jni: GetStringUTFChars on a null string (handle=%u, at 0x%08x)",
                                    handle, current_env_lr);
                const std::uint32_t empty = imports.AllocateData(1);
                if (empty != 0) {
                    *reinterpret_cast<char*>(arena.Ptr(empty)) = '\0';
                }
                call.Return(empty);
                return;
            }
            const char* chars = env->GetStringUTFChars(text, nullptr);
            if (chars == nullptr) {
                call.Return(0);
                return;
            }
            const std::size_t length = std::strlen(chars) + 1;
            const std::uint32_t cell = imports.AllocateData(static_cast<std::uint32_t>(length));
            // Every string the guest reads is this call, so the line is capped
            // rather than printed per call.
            if (LogAllowed("jni-string", 40)) {
                ARMBAT_LOG_INFO("Armbat",
                                    "jni: GetStringUTFChars \"%s\" len=%zu -> 0x%08x", chars,
                                    length, cell);
            }
            if (cell == 0) {
                ARMBAT_LOG_ERROR("Armbat",
                                    "jni: no room for a %zu byte string", length);
            } else {
                std::memcpy(arena.Ptr(cell), chars, length);
            }
            env->ReleaseStringUTFChars(static_cast<jstring>(Resolve(call.Arg(1))), chars);
            call.Return(cell);
        });
        add(JNI_IDX(ReleaseStringUTFChars), [this](HleCall& call) {
            if (LogAllowed("jni-release-string", 8)) {
                ARMBAT_LOG_INFO("Armbat",
                                    "jni: ReleaseStringUTFChars 0x%08x", call.Arg(2));
            }
            // The pointer GetStringUTFChars handed out is a block of the HLE
            // data area; not returning it leaks the area one string at a time.
            imports.ReleaseData(call.Arg(2));
        });

        // UTF-16 strings work like the UTF-8 ones: the guest gets a block of the
        // data area, NUL terminated, and gives it back.
        add(JNI_IDX(NewString), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            const jsize length = static_cast<jsize>(call.Arg(2));
            const std::uint64_t bytes = static_cast<std::uint64_t>(length) * sizeof(jchar);
            if (length < 0 || (length > 0 && !arena.IsMapped(call.Arg(1), bytes))) {
                call.Return(0);
                return;
            }
            call.Return(Handle(env, env->NewString(
                                       reinterpret_cast<const jchar*>(arena.Ptr(call.Arg(1))),
                                       length)));
        });
        add(JNI_IDX(GetStringChars), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            const jstring text = static_cast<jstring>(Resolve(call.Arg(1)));
            if (text == nullptr) {
                call.Return(0);
                return;
            }
            const jsize length = env->GetStringLength(text);
            const std::uint32_t block =
                imports.AllocateData(static_cast<std::uint32_t>((length + 1) * sizeof(jchar)));
            if (block == 0) {
                call.Return(0);
                return;
            }
            auto* chars = reinterpret_cast<jchar*>(arena.Ptr(block));
            if (length > 0) {
                env->GetStringRegion(text, 0, length, chars);
            }
            chars[length] = 0;
            if (call.Arg(2) != 0) {
                const std::uint8_t is_copy = 1;
                call.CopyOut(call.Arg(2), &is_copy, 1);
            }
            call.Return(block);
        });
        add(JNI_IDX(ReleaseStringChars), [this](HleCall& call) {
            imports.ReleaseData(call.Arg(2));
        });
        add(JNI_IDX(GetStringRegion), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            const jstring text = static_cast<jstring>(Resolve(call.Arg(1)));
            const jsize start = static_cast<jsize>(call.Arg(2));
            const jsize length = static_cast<jsize>(call.Arg(3));
            const std::uint64_t bytes = static_cast<std::uint64_t>(length) * sizeof(jchar);
            if (text == nullptr || start < 0 || length < 0 ||
                (length > 0 && !arena.IsMapped(call.Arg(4), bytes))) {
                return;
            }
            env->GetStringRegion(text, start, length,
                                 reinterpret_cast<jchar*>(arena.Ptr(call.Arg(4))));
        });
        add(JNI_IDX(GetStringUTFRegion), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            const jstring text = static_cast<jstring>(Resolve(call.Arg(1)));
            const jsize start = static_cast<jsize>(call.Arg(2));
            const jsize length = static_cast<jsize>(call.Arg(3));
            if (text == nullptr || start < 0 || length < 0 ||
                (length > 0 && !arena.IsMapped(call.Arg(4), static_cast<std::uint64_t>(length)))) {
                return;
            }
            env->GetStringUTFRegion(text, start, length,
                                    reinterpret_cast<char*>(arena.Ptr(call.Arg(4))));
        });

        add(JNI_IDX(FromReflectedMethod), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            jobject reflected = static_cast<jobject>(Resolve(call.Arg(1)));
            if (reflected == nullptr) {
                call.Return(0);
                return;
            }
            const jmethodID method = env->FromReflectedMethod(reflected);
            if (method == nullptr) {
                env->ExceptionClear();
                call.Return(0);
                return;
            }
            std::string name, signature;
            bool is_static = false;
            if (!DescribeMethod(env, reflected, &name, &signature, &is_static)) {
                call.Return(0);
                return;
            }
            call.Return(AddMethod(env, method, name, signature, is_static));
        });

        add(JNI_IDX(ToReflectedMethod), [this](HleCall& call) {
            // Unity's proxy bridge turns a method id back into a java.lang.reflect
            // Method and invokes the target through it; without this it gets null
            // and its proxy answer is null too.
            JNIEnv* env = HostEnv();
            const jclass type = static_cast<jclass>(Resolve(call.Arg(1)));
            const std::uint32_t method_id = call.Arg(2);
            if (type == nullptr || method_id == 0 || method_id > method_refs.size()) {
                call.Return(0);
                return;
            }
            const MethodRef& ref = method_refs[method_id - 1];
            if (ref.method == nullptr) {
                call.Return(0);
                return;
            }
            jobject reflected =
                env->ToReflectedMethod(type, ref.method, ref.is_static ? JNI_TRUE : JNI_FALSE);
            if (reflected == nullptr) {
                env->ExceptionClear();
                call.Return(0);
                return;
            }
            const std::uint32_t handle = Handle(env, reflected);
            env->DeleteLocalRef(reflected);
            call.Return(handle);
        });

        add(JNI_IDX(FromReflectedField), [this](HleCall& call) {
            // Unity's managed AndroidJavaObject finds a field through
            // java.lang.reflect (the signature it would pass is the generic
            // object one) and converts the Field here. Answering null leaves
            // every managed read with a zero field id, whose value is null,
            // so the wrapper's constructor throws instead of returning data.
            JNIEnv* env = HostEnv();
            jobject reflected = static_cast<jobject>(Resolve(call.Arg(1)));
            if (reflected == nullptr) {
                call.Return(0);
                return;
            }
            const jfieldID field = env->FromReflectedField(reflected);
            if (field == nullptr) {
                env->ExceptionClear();
                call.Return(0);
                return;
            }
            std::string name, signature;
            bool is_static = false;
            if (!DescribeField(env, reflected, &name, &signature, &is_static)) {
                call.Return(0);
                return;
            }
            const std::lock_guard lock(refs_mutex);
            field_refs.push_back(FieldRef{field, signature, is_static});
            call.Return(static_cast<std::uint32_t>(field_refs.size()));
        });

        add(JNI_IDX(ToReflectedField), [this](HleCall& call) {
            // The mirror of FromReflectedField: a field id from this table back
            // as the java.lang.reflect.Field object.
            JNIEnv* env = HostEnv();
            const jclass type = static_cast<jclass>(Resolve(call.Arg(1)));
            const std::uint32_t field_id = call.Arg(2);
            if (type == nullptr || field_id == 0 || field_id > field_refs.size()) {
                call.Return(0);
                return;
            }
            const FieldRef& ref = field_refs[field_id - 1];
            if (ref.field == nullptr) {
                call.Return(0);
                return;
            }
            jobject reflected =
                env->ToReflectedField(type, ref.field, ref.is_static ? JNI_TRUE : JNI_FALSE);
            if (reflected == nullptr) {
                env->ExceptionClear();
                call.Return(0);
                return;
            }
            const std::uint32_t handle = Handle(env, reflected);
            env->DeleteLocalRef(reflected);
            call.Return(handle);
        });

        add(JNI_IDX(GetMethodID), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            const jclass type = static_cast<jclass>(Resolve(call.Arg(1)));
            if (type == nullptr) {
                call.Return(0);
                return;
            }
            const std::string name = ArgString(call, call.Arg(2));
            const std::string signature = ArgString(call, call.Arg(3));
            jmethodID method = env->GetMethodID(type, name.c_str(), signature.c_str());
            if (method == nullptr) {
                env->ExceptionClear();
                call.Return(0);
                return;
            }
            call.Return(AddMethod(env, method, name, signature, false));
        });

        add(JNI_IDX(GetStaticMethodID), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            const jclass type = static_cast<jclass>(Resolve(call.Arg(1)));
            if (type == nullptr) {
                call.Return(0);
                return;
            }
            const std::string name = ArgString(call, call.Arg(2));
            const std::string signature = ArgString(call, call.Arg(3));
            jmethodID method = env->GetStaticMethodID(type, name.c_str(), signature.c_str());
            if (method == nullptr) {
                env->ExceptionClear();
                call.Return(0);
                return;
            }
            call.Return(AddMethod(env, method, name, signature, true));
        });

        add(JNI_IDX(GetFieldID), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            const jclass type = static_cast<jclass>(Resolve(call.Arg(1)));
            const std::string name = ArgString(call, call.Arg(2));
            const std::string signature = ArgString(call, call.Arg(3));
            if (type == nullptr) {
                MissingClass("GetFieldID", name, signature);
                call.Return(0);
                return;
            }
            jfieldID field = env->GetFieldID(type, name.c_str(), signature.c_str());
            if (field == nullptr) {
                env->ExceptionClear();
                call.Return(0);
                return;
            }
            const std::lock_guard lock(refs_mutex);
            field_refs.push_back(FieldRef{field, signature, false});
            call.Return(static_cast<std::uint32_t>(field_refs.size()));
        });

        add(JNI_IDX(GetStaticFieldID), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            const jclass type = static_cast<jclass>(Resolve(call.Arg(1)));
            const std::string name = ArgString(call, call.Arg(2));
            const std::string signature = ArgString(call, call.Arg(3));
            if (type == nullptr) {
                MissingClass("GetStaticFieldID", name, signature);
                call.Return(0);
                return;
            }
            jfieldID field = env->GetStaticFieldID(type, name.c_str(), signature.c_str());
            if (field == nullptr) {
                env->ExceptionClear();
                call.Return(0);
                return;
            }
            const std::lock_guard lock(refs_mutex);
            field_refs.push_back(FieldRef{field, signature, true});
            call.Return(static_cast<std::uint32_t>(field_refs.size()));
        });

        add(JNI_IDX(ExceptionCheck), [](HleCall& call) {
            call.Return(HostEnv()->ExceptionCheck() ? 1 : 0);
        });
        add(JNI_IDX(ExceptionOccurred), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            // The one reference that exists while an exception is pending is the
            // exception itself, so Handle() cannot make it the usual way: it
            // refuses a pending state on purpose (CheckJNI aborts when a global
            // reference is created with an exception thrown). Taking the
            // exception out, referencing it, and putting it back gives the guest
            // the reference it asked for and leaves the thread exactly as the
            // device would: still throwing.
            jobject thrown = env->ExceptionOccurred();
            env->ExceptionClear();
            const std::uint32_t handle = Handle(env, thrown);
            if (thrown != nullptr) {
                env->Throw(static_cast<jthrowable>(thrown));
                env->DeleteLocalRef(thrown);
            }
            call.Return(handle);
        });
        add(JNI_IDX(ExceptionDescribe), [](HleCall&) { HostEnv()->ExceptionDescribe(); });
        add(JNI_IDX(ExceptionClear), [](HleCall&) { HostEnv()->ExceptionClear(); });
        add(JNI_IDX(Throw), [this](HleCall& call) {
            jobject exception = Resolve(call.Arg(1));
            if (exception == nullptr) {
                // "Throw nothing" is not a throw. ART treats a null throwable as
                // a fatal JNI misuse and aborts the process, which for a
                // container means the whole app dies with the guest's mistake.
                ARMBAT_LOG_WARN("Armbat",
                                    "jni: Throw of a null throwable ignored at 0x%08x",
                                    current_env_lr);
                call.Return(0);
                return;
            }
            call.Return(static_cast<std::uint32_t>(
                HostEnv()->Throw(static_cast<jthrowable>(exception))));
        });
        add(JNI_IDX(ThrowNew), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            jclass type = static_cast<jclass>(Resolve(call.Arg(1)));
            if (type == nullptr) {
                ARMBAT_LOG_WARN("Armbat",
                                    "jni: ThrowNew of a null class ignored at 0x%08x",
                                    current_env_lr);
                call.Return(0);
                return;
            }
            call.Return(static_cast<std::uint32_t>(
                env->ThrowNew(type, ArgString(call, call.Arg(2)).c_str())));
        });

        add(JNI_IDX(GetJavaVM), [this](HleCall& call) {
            const std::uint32_t value = vm_pointer;
            call.CopyOut(call.Arg(1), &value, sizeof(value));
            call.Return(0);
        });

        add(JNI_IDX(MonitorEnter), [this](HleCall& call) {
            call.Return(static_cast<std::uint32_t>(HostEnv()->MonitorEnter(Resolve(call.Arg(1)))));
        });
        add(JNI_IDX(MonitorExit), [this](HleCall& call) {
            call.Return(static_cast<std::uint32_t>(HostEnv()->MonitorExit(Resolve(call.Arg(1)))));
        });

        add(JNI_IDX(GetArrayLength), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            call.Return(static_cast<std::uint32_t>(
                env->GetArrayLength(static_cast<jarray>(Resolve(call.Arg(1))))));
        });

        AddArrayType<jboolean>(add, JNI_IDX(GetBooleanArrayElements),
                               JNI_IDX(ReleaseBooleanArrayElements),
                               JNI_IDX(GetBooleanArrayRegion), JNI_IDX(SetBooleanArrayRegion),
                               JNI_IDX(NewBooleanArray));
        AddArrayType<jbyte>(add, JNI_IDX(GetByteArrayElements), JNI_IDX(ReleaseByteArrayElements),
                            JNI_IDX(GetByteArrayRegion), JNI_IDX(SetByteArrayRegion),
                            JNI_IDX(NewByteArray));
        AddArrayType<jchar>(add, JNI_IDX(GetCharArrayElements), JNI_IDX(ReleaseCharArrayElements),
                            JNI_IDX(GetCharArrayRegion), JNI_IDX(SetCharArrayRegion),
                            JNI_IDX(NewCharArray));
        AddArrayType<jshort>(add, JNI_IDX(GetShortArrayElements), JNI_IDX(ReleaseShortArrayElements),
                             JNI_IDX(GetShortArrayRegion), JNI_IDX(SetShortArrayRegion),
                             JNI_IDX(NewShortArray));
        AddArrayType<jint>(add, JNI_IDX(GetIntArrayElements), JNI_IDX(ReleaseIntArrayElements),
                           JNI_IDX(GetIntArrayRegion), JNI_IDX(SetIntArrayRegion),
                           JNI_IDX(NewIntArray));
        AddArrayType<jlong>(add, JNI_IDX(GetLongArrayElements), JNI_IDX(ReleaseLongArrayElements),
                            JNI_IDX(GetLongArrayRegion), JNI_IDX(SetLongArrayRegion),
                            JNI_IDX(NewLongArray));
        AddArrayType<jfloat>(add, JNI_IDX(GetFloatArrayElements), JNI_IDX(ReleaseFloatArrayElements),
                             JNI_IDX(GetFloatArrayRegion), JNI_IDX(SetFloatArrayRegion),
                             JNI_IDX(NewFloatArray));
        AddArrayType<jdouble>(add, JNI_IDX(GetDoubleArrayElements),
                              JNI_IDX(ReleaseDoubleArrayElements), JNI_IDX(GetDoubleArrayRegion),
                              JNI_IDX(SetDoubleArrayRegion), JNI_IDX(NewDoubleArray));

        // GetPrimitiveArrayCritical is the path native code takes for per-frame
        // data: Unity copies the pointer-id and coordinate arrays through it on
        // every touch, and writing into the null it was answered with is a
        // fault. A guest block cannot alias a host array, so this is the
        // copying form, which the specification allows as long as isCopy says
        // so; the block goes back into the array on release.
        add(JNI_IDX(GetPrimitiveArrayCritical), [this](HleCall& call) {
            switch (ArrayKindOf(HostEnv(), Resolve(call.Arg(1)))) {
                case ArrayKind::kBoolean: ArrayElements<jboolean>(call); return;
                case ArrayKind::kByte: ArrayElements<jbyte>(call); return;
                case ArrayKind::kChar: ArrayElements<jchar>(call); return;
                case ArrayKind::kShort: ArrayElements<jshort>(call); return;
                case ArrayKind::kInt: ArrayElements<jint>(call); return;
                case ArrayKind::kLong: ArrayElements<jlong>(call); return;
                case ArrayKind::kFloat: ArrayElements<jfloat>(call); return;
                case ArrayKind::kDouble: ArrayElements<jdouble>(call); return;
                case ArrayKind::kNone: call.Return(0); return;
            }
            call.Return(0);
        });
        add(JNI_IDX(ReleasePrimitiveArrayCritical), [this](HleCall& call) {
            switch (ArrayKindOf(HostEnv(), Resolve(call.Arg(1)))) {
                case ArrayKind::kBoolean: ReleaseArrayElements<jboolean>(call); return;
                case ArrayKind::kByte: ReleaseArrayElements<jbyte>(call); return;
                case ArrayKind::kChar: ReleaseArrayElements<jchar>(call); return;
                case ArrayKind::kShort: ReleaseArrayElements<jshort>(call); return;
                case ArrayKind::kInt: ReleaseArrayElements<jint>(call); return;
                case ArrayKind::kLong: ReleaseArrayElements<jlong>(call); return;
                case ArrayKind::kFloat: ReleaseArrayElements<jfloat>(call); return;
                case ArrayKind::kDouble: ReleaseArrayElements<jdouble>(call); return;
                case ArrayKind::kNone: return;
            }
        });

        add(JNI_IDX(NewObjectArray), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            call.Return(Handle(env, env->NewObjectArray(static_cast<jsize>(call.Arg(1)),
                                                        static_cast<jclass>(Resolve(call.Arg(2))),
                                                        Resolve(call.Arg(3)))));
        });

        add(JNI_IDX(AllocObject), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            const jclass type = static_cast<jclass>(Resolve(call.Arg(1)));
            if (type == nullptr) {
                call.Return(0);
                return;
            }
            call.Return(Handle(env, env->AllocObject(type)));
        });

        // Direct buffers are the guest's own memory seen through a Java object,
        // so the address handed to the guest is its arena offset, not the host
        // pointer the JVM stores.
        add(JNI_IDX(NewDirectByteBuffer), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            const std::uint32_t address = call.Arg(1);
            const std::uint64_t capacity = static_cast<std::uint64_t>(call.Arg(2));
            if (capacity != 0 && !arena.IsMapped(address, capacity)) {
                call.Return(0);
                return;
            }
            call.Return(Handle(env, env->NewDirectByteBuffer(arena.Ptr(address),
                                                             static_cast<jlong>(capacity))));
        });
        add(JNI_IDX(GetDirectBufferAddress), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            const jobject buffer = Resolve(call.Arg(1));
            call.Return(buffer == nullptr ? 0 : DirectBufferAddress(env, buffer));
        });
        add(JNI_IDX(GetDirectBufferCapacity), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            const jobject buffer = Resolve(call.Arg(1));
            const jlong capacity =
                buffer == nullptr ? -1 : env->GetDirectBufferCapacity(buffer);
            call.ReturnPair(static_cast<std::uint32_t>(static_cast<std::uint64_t>(capacity)),
                            static_cast<std::uint32_t>(static_cast<std::uint64_t>(capacity) >> 32));
        });
        add(JNI_IDX(GetObjectArrayElement), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            call.Return(Handle(env, env->GetObjectArrayElement(
                                       static_cast<jobjectArray>(Resolve(call.Arg(1))),
                                       static_cast<jsize>(call.Arg(2)))));
        });
        add(JNI_IDX(SetObjectArrayElement), [this](HleCall& call) {
            JNIEnv* env = HostEnv();
            env->SetObjectArrayElement(static_cast<jobjectArray>(Resolve(call.Arg(1))),
                                       static_cast<jsize>(call.Arg(2)), Resolve(call.Arg(3)));
        });

        add(JNI_IDX(RegisterNatives), [this](HleCall& call) {
            call.Return(RegisterGuestNatives(call));
        });
        add(JNI_IDX(UnregisterNatives), [this](HleCall& call) {
            jclass type = static_cast<jclass>(Resolve(call.Arg(1)));
            if (type == nullptr) {
                call.Return(static_cast<std::uint32_t>(JNI_ERR));
                return;
            }
            call.Return(static_cast<std::uint32_t>(HostEnv()->UnregisterNatives(type)));
        });

        add(JNI_IDX(NewObject), [this](HleCall& call) { NewObjectThrough(call, 0); });
        add(JNI_IDX(NewObjectV), [this](HleCall& call) { NewObjectThrough(call, 1); });
        add(JNI_IDX(NewObjectA), [this](HleCall& call) { NewObjectThrough(call, 2); });

        for (int kind = 0; kind < 9; ++kind) {
            add(95 + kind, [this](HleCall& call) { FieldThrough(call, false, false); });
            add(104 + kind, [this](HleCall& call) { FieldThrough(call, false, true); });
            add(145 + kind, [this](HleCall& call) { FieldThrough(call, true, false); });
            add(154 + kind, [this](HleCall& call) { FieldThrough(call, true, true); });
        }

        // Every Call<Type>Method entry point, in the three shapes JNI defines
        // for it: plain, V (va_list) and A (jvalue array).
        constexpr char kResultTypes[10] = {'L', 'Z', 'B', 'C', 'S', 'I', 'J', 'F', 'D', 'V'};
        for (int type = 0; type < 10; ++type) {
            for (int form = 0; form < 3; ++form) {
                const char result = kResultTypes[type];
                add(34 + 3 * type + form, [this, result, form](HleCall& call) {
                    CallThrough(call, result, form, 0);
                });
                add(64 + 3 * type + form, [this, result, form](HleCall& call) {
                    CallThrough(call, result, form, 1);
                });
                add(114 + 3 * type + form, [this, result, form](HleCall& call) {
                    CallThrough(call, result, form, 2);
                });
            }
        }

        add_vm(JNI_VM_IDX(AttachCurrentThread), [this](HleCall& call) {
            const std::uint32_t value = env_pointer;
            call.CopyOut(call.Arg(1), &value, sizeof(value));
            call.Return(0);  // JNI_OK
        });
        add_vm(JNI_VM_IDX(AttachCurrentThreadAsDaemon), [this](HleCall& call) {
            const std::uint32_t value = env_pointer;
            call.CopyOut(call.Arg(1), &value, sizeof(value));
            call.Return(0);
        });
        add_vm(JNI_VM_IDX(DetachCurrentThread), [](HleCall& call) { call.Return(0); });
        add_vm(JNI_VM_IDX(GetEnv), [this](HleCall& call) {
            const std::uint32_t value = env_pointer;
            call.CopyOut(call.Arg(1), &value, sizeof(value));
            call.Return(0);
        });
        add_vm(JNI_VM_IDX(DestroyJavaVM), [](HleCall& call) { call.Return(0); });
    }

    std::uint32_t AddMethod(JNIEnv* env, jmethodID method, const std::string& name,
                            const std::string& signature, bool is_static) {
        // The host method id is not usable from the guest, so the table position
        // is what the guest gets back.
        const std::lock_guard lock(refs_mutex);
        if (const auto found = method_ids.find(method); found != method_ids.end()) {
            return found->second;
        }
        method_refs.push_back(MethodRef{method, name, signature, is_static});
        const std::uint32_t id = static_cast<std::uint32_t>(method_refs.size());
        method_ids.emplace(method, id);
        return id;
    }

    // JNI descriptor of a Class object. The reflected name of a primitive is its
    // own keyword, an array keeps the '[' prefix it already carries, and every
    // other name is an object type with dots turned into slashes.
    std::string ClassDescriptor(JNIEnv* env, jclass type) {
        if (type == nullptr) {
            return std::string();
        }
        jclass class_class = env->FindClass("java/lang/Class");
        const jmethodID get_name = env->GetMethodID(class_class, "getName", "()Ljava/lang/String;");
        std::string name;
        auto text = static_cast<jstring>(env->CallObjectMethod(type, get_name));
        if (text != nullptr) {
            const char* chars = env->GetStringUTFChars(text, nullptr);
            if (chars != nullptr) {
                name = chars;
                env->ReleaseStringUTFChars(text, chars);
            }
            env->DeleteLocalRef(text);
        }
        env->DeleteLocalRef(class_class);
        static const std::pair<const char*, char> primitives[] = {
            {"void", 'V'},   {"boolean", 'Z'}, {"byte", 'B'},  {"char", 'C'},
            {"short", 'S'},  {"int", 'I'},     {"long", 'J'},  {"float", 'F'},
            {"double", 'D'},
        };
        for (const auto& [keyword, letter] : primitives) {
            if (name == keyword) {
                return std::string(1, letter);
            }
        }
        for (char& character : name) {
            if (character == '.') {
                character = '/';
            }
        }
        if (!name.empty() && name[0] == '[') {
            return name;
        }
        return "L" + name + ";";
    }

    // Name, JNI descriptor and kind of a reflected java.lang.reflect.Field.
    // FromReflectedField has to answer with the same table entry GetFieldID
    // would have produced: the field read handlers pick the marshalling by the
    // descriptor, and a field found through reflection carries only its type.
    bool DescribeField(JNIEnv* env, jobject field, std::string* name, std::string* signature,
                       bool* is_static) {
        jclass field_class = env->FindClass("java/lang/reflect/Field");
        bool done = false;
        if (field_class != nullptr) {
            const jmethodID get_name =
                env->GetMethodID(field_class, "getName", "()Ljava/lang/String;");
            const jmethodID get_type =
                env->GetMethodID(field_class, "getType", "()Ljava/lang/Class;");
            const jmethodID get_modifiers = env->GetMethodID(field_class, "getModifiers", "()I");
            if (get_name != nullptr && get_type != nullptr && get_modifiers != nullptr) {
                auto text = static_cast<jstring>(env->CallObjectMethod(field, get_name));
                if (text != nullptr) {
                    const char* chars = env->GetStringUTFChars(text, nullptr);
                    if (chars != nullptr) {
                        *name = chars;
                        env->ReleaseStringUTFChars(text, chars);
                    }
                    env->DeleteLocalRef(text);
                }
                auto type = static_cast<jclass>(env->CallObjectMethod(field, get_type));
                *signature = ClassDescriptor(env, type);
                if (type != nullptr) {
                    env->DeleteLocalRef(type);
                }
                const jint modifiers = env->CallIntMethod(field, get_modifiers);
                // java.lang.reflect.Modifier.STATIC
                *is_static = (modifiers & 0x0008) != 0;
                done = !name->empty() && !signature->empty();
            }
            env->DeleteLocalRef(field_class);
        }
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            return false;
        }
        return done;
    }

    // Name, JNI descriptor and kind of a reflected java.lang.reflect.Method.
    // FromReflectedMethod has to answer with the same table entry GetMethodID
    // would have produced, so the whole signature is rebuilt from the types the
    // Method carries: the call handlers marshal arguments by it.
    //
    // The reflected object may also be a Constructor -- FromReflectedMethod
    // accepts both, and asking a Constructor for getName aborts the process
    // under CheckJNI. A constructor is described the way GetMethodID("<init>")
    // would describe it, so NewObjectA finds it in the same table.
    bool DescribeMethod(JNIEnv* env, jobject method, std::string* name, std::string* signature,
                        bool* is_static) {
        jclass method_class = env->FindClass("java/lang/reflect/Method");
        jclass constructor_class = env->FindClass("java/lang/reflect/Constructor");
        bool done = false;
        if (method_class != nullptr && constructor_class != nullptr) {
            const bool is_constructor = env->IsInstanceOf(method, constructor_class) == JNI_TRUE;
            const jclass type_class =
                env->FindClass(is_constructor ? "java/lang/reflect/Constructor"
                                              : "java/lang/reflect/Method");
            const jmethodID get_name =
                is_constructor
                    ? nullptr
                    : env->GetMethodID(type_class, "getName", "()Ljava/lang/String;");
            const jmethodID get_return =
                is_constructor
                    ? nullptr
                    : env->GetMethodID(type_class, "getReturnType", "()Ljava/lang/Class;");
            const jmethodID get_params =
                env->GetMethodID(type_class, "getParameterTypes", "()[Ljava/lang/Class;");
            const jmethodID get_modifiers =
                env->GetMethodID(type_class, "getModifiers", "()I");
            if (get_params != nullptr && get_modifiers != nullptr &&
                (is_constructor || (get_name != nullptr && get_return != nullptr))) {
                if (is_constructor) {
                    *name = "<init>";
                } else {
                    auto text = static_cast<jstring>(env->CallObjectMethod(method, get_name));
                    if (text != nullptr) {
                        const char* chars = env->GetStringUTFChars(text, nullptr);
                        if (chars != nullptr) {
                            *name = chars;
                            env->ReleaseStringUTFChars(text, chars);
                        }
                        env->DeleteLocalRef(text);
                    }
                }
                std::string descriptor = "(";
                auto params = static_cast<jobjectArray>(env->CallObjectMethod(method, get_params));
                if (params != nullptr) {
                    const jsize count = env->GetArrayLength(params);
                    for (jsize i = 0; i < count; ++i) {
                        auto parameter = static_cast<jclass>(env->GetObjectArrayElement(params, i));
                        descriptor += ClassDescriptor(env, parameter);
                        if (parameter != nullptr) {
                            env->DeleteLocalRef(parameter);
                        }
                    }
                    env->DeleteLocalRef(params);
                }
                descriptor += ')';
                if (is_constructor) {
                    descriptor += 'V';
                } else {
                    auto returned = static_cast<jclass>(env->CallObjectMethod(method, get_return));
                    descriptor += ClassDescriptor(env, returned);
                    if (returned != nullptr) {
                        env->DeleteLocalRef(returned);
                    }
                }
                *signature = std::move(descriptor);
                const jint modifiers = env->CallIntMethod(method, get_modifiers);
                // java.lang.reflect.Modifier.STATIC
                *is_static = !is_constructor && (modifiers & 0x0008) != 0;
                done = !name->empty();
            }
            if (type_class != nullptr) {
                env->DeleteLocalRef(type_class);
            }
        }
        if (method_class != nullptr) {
            env->DeleteLocalRef(method_class);
        }
        if (constructor_class != nullptr) {
            env->DeleteLocalRef(constructor_class);
        }
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            return false;
        }
        return done;
    }

    // ---- arrays -----------------------------------------------------------

    enum class ArrayKind {
        kBoolean,
        kByte,
        kChar,
        kShort,
        kInt,
        kLong,
        kFloat,
        kDouble,
        kNone
    };

    // The element type of a primitive array. A critical section may not call
    // back into the framework, and the guest may hand back any array, so the type
    // is read from the array's own class name ("[I", "[F", ...). Resolving the
    // array classes by FindClass would depend on which class loader the calling
    // thread happens to see, and a wrong answer here is a null block the guest
    // writes into.
    ArrayKind ArrayKindOf(JNIEnv* env, jobject object) {
        if (object == nullptr) {
            if (LogAllowed("jni-array-null", 4)) {
                ARMBAT_LOG_ERROR("Armbat", "jni: %s got a null array",
                                    EnvName(current_env_index));
            }
            return ArrayKind::kNone;
        }
        jclass array_class = env->GetObjectClass(object);
        if (array_class == nullptr) {
            env->ExceptionClear();
            return ArrayKind::kNone;
        }
        jclass class_class = env->GetObjectClass(array_class);
        jmethodID get_name =
            class_class != nullptr ? env->GetMethodID(class_class, "getName", "()Ljava/lang/String;")
                                   : nullptr;
        std::string name;
        if (get_name != nullptr) {
            auto text = static_cast<jstring>(env->CallObjectMethod(array_class, get_name));
            if (text != nullptr) {
                const char* utf = env->GetStringUTFChars(text, nullptr);
                if (utf != nullptr) {
                    name = utf;
                    env->ReleaseStringUTFChars(text, utf);
                }
                env->DeleteLocalRef(text);
            }
        }
        env->DeleteLocalRef(array_class);
        if (class_class != nullptr) {
            env->DeleteLocalRef(class_class);
        }
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            return ArrayKind::kNone;
        }
        if (name.size() == 2 && name[0] == '[') {
            switch (name[1]) {
                case 'Z': return ArrayKind::kBoolean;
                case 'B': return ArrayKind::kByte;
                case 'C': return ArrayKind::kChar;
                case 'S': return ArrayKind::kShort;
                case 'I': return ArrayKind::kInt;
                case 'J': return ArrayKind::kLong;
                case 'F': return ArrayKind::kFloat;
                case 'D': return ArrayKind::kDouble;
                default: break;
            }
        }
        if (LogAllowed("jni-array-kind", 4)) {
            ARMBAT_LOG_ERROR("Armbat", "jni: cannot tell the element type of %s",
                                name.empty() ? "an object that is not an array" : name.c_str());
        }
        return ArrayKind::kNone;
    }

    // A guest sees an array as a block of the HLE data area. Elements are
    // copied in on the Get and back on Release unless the guest asks for
    // JNI_ABORT, which is what makes a byte array read from Java usable.
    template <typename T>
    void ArrayElements(HleCall& call) {
        JNIEnv* env = HostEnv();
        jarray array = static_cast<jarray>(Resolve(call.Arg(1)));
        if (array == nullptr) {
            call.Return(0);
            return;
        }
        const jsize length = env->GetArrayLength(array);
        if (length < 0) {
            call.Return(0);
            return;
        }
        const std::uint64_t bytes = static_cast<std::uint64_t>(length) * sizeof(T);
        if (bytes > 0x3FFFFFFFu) {
            call.Return(0);
            return;
        }
        const std::uint32_t block =
            imports.AllocateData(bytes == 0 ? 1 : static_cast<std::uint32_t>(bytes));
        if (block == 0) {
            if (LogAllowed("jni-noarray-block", 4)) {
                ARMBAT_LOG_ERROR("Armbat", "jni: no guest block for %d array elements", length);
            }
            call.Return(0);
            return;
        }
        if (length > 0) {
            HostGetArrayRegion<T>(env, array, 0, length,
                                  reinterpret_cast<T*>(arena.Ptr(block)));
        }
        if (call.Arg(2) != 0) {
            const std::uint8_t is_copy = 1;
            call.CopyOut(call.Arg(2), &is_copy, 1);
        }
        call.Return(block);
    }

    template <typename T>
    void ReleaseArrayElements(HleCall& call) {
        JNIEnv* env = HostEnv();
        jarray array = static_cast<jarray>(Resolve(call.Arg(1)));
        const std::uint32_t block = call.Arg(2);
        if (block == 0) {
            return;
        }
        if (call.Arg(3) != JNI_ABORT && array != nullptr) {
            const jsize length = env->GetArrayLength(array);
            const std::uint64_t bytes = static_cast<std::uint64_t>(length) * sizeof(T);
            if (length > 0 && arena.IsMapped(block, bytes)) {
                HostSetArrayRegion<T>(env, array, 0, length,
                                      reinterpret_cast<const T*>(arena.Ptr(block)));
            }
        }
        imports.ReleaseData(block);
    }

    template <typename T>
    void GetArrayRegion(HleCall& call) {
        JNIEnv* env = HostEnv();
        jarray array = static_cast<jarray>(Resolve(call.Arg(1)));
        const jsize start = static_cast<jsize>(call.Arg(2));
        const jsize length = static_cast<jsize>(call.Arg(3));
        const std::uint32_t block = call.Arg(4);
        const std::uint64_t bytes = static_cast<std::uint64_t>(length) * sizeof(T);
        if (array == nullptr || start < 0 || length < 0 || block == 0 || length == 0 ||
            !arena.IsMapped(block, bytes)) {
            return;
        }
        HostGetArrayRegion<T>(env, array, start, length,
                              reinterpret_cast<T*>(arena.Ptr(block)));
    }

    template <typename T>
    void SetArrayRegion(HleCall& call) {
        JNIEnv* env = HostEnv();
        jarray array = static_cast<jarray>(Resolve(call.Arg(1)));
        const jsize start = static_cast<jsize>(call.Arg(2));
        const jsize length = static_cast<jsize>(call.Arg(3));
        const std::uint32_t block = call.Arg(4);
        const std::uint64_t bytes = static_cast<std::uint64_t>(length) * sizeof(T);
        if (array == nullptr || start < 0 || length < 0 || block == 0 || length == 0 ||
            !arena.IsMapped(block, bytes)) {
            return;
        }
        HostSetArrayRegion<T>(env, array, start, length,
                              reinterpret_cast<const T*>(arena.Ptr(block)));
    }

    template <typename T>
    void NewArray(HleCall& call) {
        JNIEnv* env = HostEnv();
        call.Return(Handle(env, HostNewArray<T>(env, static_cast<jsize>(call.Arg(1)))));
    }

    template <typename T, typename Add>
    void AddArrayType(Add& add, std::size_t elements, std::size_t release, std::size_t get_region,
                      std::size_t set_region, std::size_t create) {
        add(elements, [this](HleCall& call) { ArrayElements<T>(call); });
        add(release, [this](HleCall& call) { ReleaseArrayElements<T>(call); });
        add(get_region, [this](HleCall& call) { GetArrayRegion<T>(call); });
        add(set_region, [this](HleCall& call) { SetArrayRegion<T>(call); });
        add(create, [this](HleCall& call) { NewArray<T>(call); });
    }

    // A null class reaches the id lookups when the guest stores the answer of a
    // FindClass that failed, or keeps a local reference past the call that made
    // it. CheckJNI aborts the whole process for one, and a lookup that cannot
    // name its class answers zero on the platform as well, so it is answered
    // here and named once -- the alternative is a crash report that says
    // nothing about which class the archive lost.
    void MissingClass(const char* entry, const std::string& name,
                      const std::string& signature) {
        static std::mutex mutex;
        static std::set<std::string> logged;
        const std::lock_guard lock(mutex);
        if (logged.size() >= 100 ||
            !logged.insert(std::string(entry) + " " + name + signature).second) {
            return;
        }
        ARMBAT_LOG_INFO("Armbat", "jni: %s on a null class: %s%s", entry, name.c_str(),
                            signature.c_str());
    }

    jclass GuestClass(const std::string& name) {
        JNIEnv* env = HostEnv();
        if (env == nullptr || loader == nullptr) {
            return nullptr;
        }
        // ClassLoader.loadClass cannot resolve array descriptors ([B,
        // [Ljava/lang/String;), while Class.forName can, and it takes the
        // dotted binary name that BinaryName produces. initialize=false keeps
        // class initializers from running at lookup time, like FindClass.
        jclass class_class = env->FindClass("java/lang/Class");
        if (class_class == nullptr) {
            env->ExceptionClear();
            return nullptr;
        }
        const jmethodID for_name = env->GetStaticMethodID(
            class_class, "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
        if (for_name == nullptr) {
            env->ExceptionClear();
            env->DeleteLocalRef(class_class);
            return nullptr;
        }
        jstring text = env->NewStringUTF(BinaryName(name).c_str());
        jobject type = env->CallStaticObjectMethod(class_class, for_name, text, JNI_FALSE, loader);
        env->DeleteLocalRef(text);
        env->DeleteLocalRef(class_class);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            return nullptr;
        }
        return static_cast<jclass>(type);
    }

    // A native method registered by the library itself (JNI_OnLoad path): the
    // array is a guest JNINativeMethod triple of guest pointers.
    std::uint32_t RegisterGuestNatives(HleCall& call) {
        JNIEnv* env = HostEnv();
        // Registration happens from the guest's JNI_OnLoad, i.e. after the
        // startup sweep sealed the trampolines, and several guest threads may
        // do it at once: hold the bridge and keep the region writable.
        const std::lock_guard lock(mutex);
        UnsealTrampolines();
        const jclass type = static_cast<jclass>(Resolve(call.Arg(1)));
        const std::uint32_t count = call.Arg(3);
        std::vector<JNINativeMethod> table;
        std::vector<std::string> names;
        std::vector<std::string> signatures;
        table.reserve(count);
        names.reserve(count);
        signatures.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t entry[3] = {};
            if (!call.CopyIn(call.Arg(2) + i * 12, entry, sizeof(entry))) {
                SealTrampolines();
                return JNI_ERR;
            }
            names.push_back(ArgString(call, entry[0]));
            signatures.push_back(ArgString(call, entry[1]));
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t entry[3] = {};
            call.CopyIn(call.Arg(2) + i * 12, entry, sizeof(entry));
            natives.push_back(NativeMethod{entry[2], signatures[i]});
            const std::uint32_t id = static_cast<std::uint32_t>(natives.size() - 1);
            table.push_back(JNINativeMethod{const_cast<char*>(names[i].c_str()),
                                            const_cast<char*>(signatures[i].c_str()),
                                            MakeTrampoline(id)});
        }
        const jint result = env->RegisterNatives(type, table.data(), static_cast<jint>(table.size()));
        SealTrampolines();
        return static_cast<std::uint32_t>(result);
    }

    // ---- trampolines -------------------------------------------------------

    static std::uint32_t StpX(int rt, int rt2, int offset) {
        return 0xA9000000u | (static_cast<std::uint32_t>(offset / 8) << 15) |
               (static_cast<std::uint32_t>(rt2) << 10) | (31u << 5) | static_cast<std::uint32_t>(rt);
    }

    static std::uint32_t StpD(int rt, int rt2, int offset) {
        return 0x6D000000u | (static_cast<std::uint32_t>(offset / 8) << 15) |
               (static_cast<std::uint32_t>(rt2) << 10) | (31u << 5) | static_cast<std::uint32_t>(rt);
    }

    // Writes a dispatcher stub into the next free slot. The region has to be
    // writable: see UnsealTrampolines. Returns nullptr when the slots run out.
    void* MakeTrampoline(std::uint32_t id) {
        if (trampolines == nullptr || trampolines_used >= kTrampolineCount) {
            return nullptr;
        }
        auto* code = reinterpret_cast<std::uint32_t*>(trampolines + trampolines_used * kTrampolineStride);
        ++trampolines_used;

        code[0] = 0xD10243FFu;  // sub sp, sp, #0x90
        code[1] = StpX(0, 1, 0);
        code[2] = StpX(2, 3, 16);
        code[3] = StpX(4, 5, 32);
        code[4] = StpX(6, 7, 48);
        code[5] = StpD(0, 1, 64);
        code[6] = StpD(2, 3, 80);
        code[7] = StpD(4, 5, 96);
        code[8] = StpD(6, 7, 112);
        code[9] = 0x910003E0u;                       // mov x0, sp
        code[10] = 0x910243E1u;                      // add x1, sp, #0x90
        code[11] = 0x52800000u | (id << 5) | 2u;     // movz w2, #id

        // The caller's link register lives in the frame: `blr` overwrites lr with
        // the address of the instructions that follow it, so without saving it
        // first the `ret` at the end would jump back into this same trampoline
        // and spin there forever.
        code[12] = 0xF90043FEu;                      // str x30, [sp, #128]

        // The dispatcher address is materialised, not loaded from a literal: a
        // literal after `blr` would sit on the return address and the trampoline
        // would resume in the middle of it (SIGILL).
        const std::uint64_t dispatcher = reinterpret_cast<std::uint64_t>(&ArmBatNativeDispatch);
        code[13] = 0xD2800000u | (static_cast<std::uint32_t>(dispatcher & 0xFFFFu) << 5) | 16u;
        code[14] = 0xF2800000u | (1u << 21) |
                   (static_cast<std::uint32_t>((dispatcher >> 16) & 0xFFFFu) << 5) | 16u;
        code[15] = 0xF2800000u | (2u << 21) |
                   (static_cast<std::uint32_t>((dispatcher >> 32) & 0xFFFFu) << 5) | 16u;
        code[16] = 0xF2800000u | (3u << 21) |
                   (static_cast<std::uint32_t>((dispatcher >> 48) & 0xFFFFu) << 5) | 16u;
        code[17] = 0xD63F0200u;                      // blr x16
        // The dispatcher answers in x0. JNI returns a float or double in d0, so
        // copy the (possibly 64-bit) result there as well: for ints ART ignores
        // it, for floats the low half is already the softfp bit pattern.
        code[18] = 0x9E670000u;                      // fmov d0, x0
        code[19] = 0xF94043FEu;                      // ldr x30, [sp, #128]
        code[20] = 0x910243FFu;                      // add sp, sp, #0x90
        code[21] = 0xD65F03C0u;                      // ret
        return code;
    }

    static std::uint64_t ArmBatNativeDispatch(std::uint64_t* frame, std::uint64_t* stack_args,
                                              std::uint32_t id);
    std::uint64_t Dispatch(std::uint64_t* frame, std::uint64_t* stack_args, std::uint32_t id);

    void UnsealTrampolines() {
        if (trampolines == nullptr || !trampolines_sealed) {
            return;
        }
        // The guest's own JNI_OnLoad registers natives long after the startup
        // sweep walked Java_* symbols and sealed the region executable, so a new
        // trampoline has to be writable again before MakeTrampoline touches it.
        mprotect(trampolines, kTrampolineStride * kTrampolineCount, PROT_READ | PROT_WRITE);
        trampolines_sealed = false;
    }

    void SealTrampolines() {
        if (trampolines == nullptr) {
            return;
        }
        mprotect(trampolines, kTrampolineStride * kTrampolineCount, PROT_READ | PROT_EXEC);
        trampolines_sealed = true;
    }

    // ---- guest calls into the framework ------------------------------------

    // ---- arguments and calls ----------------------------------------------

    // Where the arguments of a Call*Method live: after the fixed arguments, in
    // registers and on the stack (the plain form), or in a va_list / jvalue
    // area in guest memory (the V and A forms). Both are walked by signature,
    // because only the signature says where the 64-bit arguments start.
    struct GuestArguments {
        HleCall& call;
        bool from_memory = false;
        // A jvalue array stores every element in an 8-byte slot; a va_list packs
        // the same values in 4-byte words with 8-byte alignment for pairs.
        bool jvalue = false;
        std::uint32_t address = 0;
        std::size_t word = 3;
        bool ok = true;

        std::uint32_t Word() {
            if (!from_memory) {
                return call.Arg(word++);
            }
            std::uint32_t value = 0;
            if (!call.CopyIn(address, &value, sizeof(value))) {
                ok = false;
                return 0;
            }
            address += jvalue ? 8 : 4;
            return value;
        }

        std::uint64_t Wide() {
            if (from_memory && jvalue) {
                std::uint64_t value = 0;
                if (!call.CopyIn(address, &value, sizeof(value))) {
                    ok = false;
                    return 0;
                }
                address += 8;
                return value;
            }
            if (from_memory) {
                address = (address + 7u) & ~std::uint32_t{7};
            } else if ((word & 1) != 0) {
                ++word;
            }
            const std::uint32_t low = Word();
            const std::uint32_t high = Word();
            return (static_cast<std::uint64_t>(high) << 32) | low;
        }
    };

    void ReadArguments(const std::string& signature, GuestArguments& in,
                       std::vector<jvalue>& out) {
        std::size_t at = signature.find('(');
        if (at == std::string::npos) {
            in.ok = false;
            return;
        }
        ++at;
        while (at < signature.size() && signature[at] != ')') {
            const std::size_t start = at;
            while (at < signature.size() && signature[at] == '[') {
                ++at;
            }
            if (at >= signature.size()) {
                in.ok = false;
                return;
            }
            const char kind = signature[at];
            if (kind == 'L') {
                while (at < signature.size() && signature[at] != ';') {
                    ++at;
                }
            }
            ++at;
            const bool object = signature[start] == 'L' || signature[start] == '[';
            jvalue value{};
            if (object) {
                value.l = Resolve(in.Word());
            } else {
                switch (kind) {
                    case 'Z': value.z = static_cast<jboolean>(in.Word()); break;
                    case 'B': value.b = static_cast<jbyte>(in.Word()); break;
                    case 'C': value.c = static_cast<jchar>(in.Word()); break;
                    case 'S': value.s = static_cast<jshort>(in.Word()); break;
                    case 'I': value.i = static_cast<jint>(in.Word()); break;
                    case 'J': value.j = static_cast<jlong>(in.Wide()); break;
                    case 'F': {
                        if (in.jvalue) {
                            const std::uint32_t bits = in.Word();
                            std::memcpy(&value.f, &bits, sizeof(bits));
                        } else {
                            // Variadic float arguments are promoted to double,
                            // which is 8 bytes in r-register pairs.
                            const std::uint64_t bits = in.Wide();
                            double promoted = 0.0;
                            std::memcpy(&promoted, &bits, sizeof(promoted));
                            value.f = static_cast<jfloat>(promoted);
                        }
                        break;
                    }
                    case 'D': {
                        const std::uint64_t bits = in.Wide();
                        std::memcpy(&value.d, &bits, sizeof(bits));
                        break;
                    }
                    default: in.ok = false; return;
                }
            }
            out.push_back(value);
            if (!in.ok) {
                return;
            }
        }
    }

    // Calls one Java method and hands the result back in the guest's ABI: a
    // float or a double arrives as its bit pattern in r0/r0:r1 (softfp), an
    // object as a reference handle, a long as a pair of words.
    void InvokeCall(HleCall& call, char result, bool is_static, bool nonvirtual,
                    jobject object, jobject type, jmethodID method, jvalue* args) {
        JNIEnv* env = HostEnv();
        switch (result) {
            case 'V':
                if (is_static) {
                    env->CallStaticVoidMethodA(static_cast<jclass>(type), method, args);
                } else if (nonvirtual) {
                    env->CallNonvirtualVoidMethodA(object, static_cast<jclass>(type), method,
                                                   args);
                } else {
                    env->CallVoidMethodA(object, method, args);
                }
                call.Return(0);
                return;
            case 'Z': {
                jboolean value = 0;
                if (is_static) {
                    value = env->CallStaticBooleanMethodA(static_cast<jclass>(type), method, args);
                } else if (nonvirtual) {
                    value = env->CallNonvirtualBooleanMethodA(
                        object, static_cast<jclass>(type), method, args);
                } else {
                    value = env->CallBooleanMethodA(object, method, args);
                }
                call.Return(value != JNI_FALSE ? 1 : 0);
                return;
            }
            case 'B': {
                jbyte value = 0;
                if (is_static) {
                    value = env->CallStaticByteMethodA(static_cast<jclass>(type), method, args);
                } else if (nonvirtual) {
                    value = env->CallNonvirtualByteMethodA(object, static_cast<jclass>(type),
                                                           method, args);
                } else {
                    value = env->CallByteMethodA(object, method, args);
                }
                call.Return(static_cast<std::uint32_t>(static_cast<std::int32_t>(value)));
                return;
            }
            case 'C': {
                jchar value = 0;
                if (is_static) {
                    value = env->CallStaticCharMethodA(static_cast<jclass>(type), method, args);
                } else if (nonvirtual) {
                    value = env->CallNonvirtualCharMethodA(object, static_cast<jclass>(type),
                                                           method, args);
                } else {
                    value = env->CallCharMethodA(object, method, args);
                }
                call.Return(value);
                return;
            }
            case 'S': {
                jshort value = 0;
                if (is_static) {
                    value = env->CallStaticShortMethodA(static_cast<jclass>(type), method, args);
                } else if (nonvirtual) {
                    value = env->CallNonvirtualShortMethodA(object, static_cast<jclass>(type),
                                                            method, args);
                } else {
                    value = env->CallShortMethodA(object, method, args);
                }
                call.Return(static_cast<std::uint32_t>(static_cast<std::int32_t>(value)));
                return;
            }
            case 'I': {
                jint value = 0;
                if (is_static) {
                    value = env->CallStaticIntMethodA(static_cast<jclass>(type), method, args);
                } else if (nonvirtual) {
                    value = env->CallNonvirtualIntMethodA(object, static_cast<jclass>(type),
                                                          method, args);
                } else {
                    value = env->CallIntMethodA(object, method, args);
                }
                call.Return(static_cast<std::uint32_t>(value));
                return;
            }
            case 'J': {
                jlong value = 0;
                if (is_static) {
                    value = env->CallStaticLongMethodA(static_cast<jclass>(type), method, args);
                } else if (nonvirtual) {
                    value = env->CallNonvirtualLongMethodA(object, static_cast<jclass>(type),
                                                           method, args);
                } else {
                    value = env->CallLongMethodA(object, method, args);
                }
                call.ReturnPair(static_cast<std::uint32_t>(value),
                                static_cast<std::uint32_t>(static_cast<std::uint64_t>(value) >> 32));
                return;
            }
            case 'F': {
                jfloat value = 0;
                if (is_static) {
                    value = env->CallStaticFloatMethodA(static_cast<jclass>(type), method, args);
                } else if (nonvirtual) {
                    value = env->CallNonvirtualFloatMethodA(object, static_cast<jclass>(type),
                                                            method, args);
                } else {
                    value = env->CallFloatMethodA(object, method, args);
                }
                std::uint32_t bits = 0;
                std::memcpy(&bits, &value, sizeof(bits));
                call.Return(bits);
                return;
            }
            case 'D': {
                jdouble value = 0;
                if (is_static) {
                    value = env->CallStaticDoubleMethodA(static_cast<jclass>(type), method, args);
                } else if (nonvirtual) {
                    value = env->CallNonvirtualDoubleMethodA(object, static_cast<jclass>(type),
                                                             method, args);
                } else {
                    value = env->CallDoubleMethodA(object, method, args);
                }
                std::uint64_t bits = 0;
                std::memcpy(&bits, &value, sizeof(bits));
                call.ReturnPair(static_cast<std::uint32_t>(bits),
                                static_cast<std::uint32_t>(bits >> 32));
                return;
            }
            default: {
                jobject value = nullptr;
                if (is_static) {
                    value = env->CallStaticObjectMethodA(static_cast<jclass>(type), method, args);
                } else if (nonvirtual) {
                    value = env->CallNonvirtualObjectMethodA(object, static_cast<jclass>(type),
                                                             method, args);
                } else {
                    value = env->CallObjectMethodA(object, method, args);
                }
                call.Return(Handle(env, value));
                return;
            }
        }
    }

    // `family` is 0 for an instance call, 1 for nonvirtual, 2 for static; `form`
    // is 0 for the plain varargs entry point, 1 for V (va_list) and 2 for A
    // (jvalue array).
    void CallThrough(HleCall& call, char result, int form, int family) {
        JNIEnv* env = HostEnv();
        jobject object = nullptr;
        jobject type = nullptr;
        std::uint32_t method_id = 0;
        std::size_t first_word = 3;
        if (family == 2) {
            type = Resolve(call.Arg(1));
            method_id = call.Arg(2);
        } else if (family == 1) {
            object = Resolve(call.Arg(1));
            type = Resolve(call.Arg(2));
            method_id = call.Arg(3);
            first_word = 4;
        } else {
            object = Resolve(call.Arg(1));
            method_id = call.Arg(2);
        }
        if (method_id == 0 || method_id > method_refs.size()) {
            ARMBAT_LOG_ERROR("Armbat",
                                "jni: call with method id %u of %zu", method_id,
                                method_refs.size());
            call.Return(0);
            return;
        }
        const MethodRef& ref = method_refs[method_id - 1];
        if (ref.method == nullptr || (family == 2 ? type == nullptr : object == nullptr)) {
            ARMBAT_LOG_ERROR("Armbat",
                                "jni: call %s on null receiver (family=%d)", ref.signature.c_str(),
                                family);
            call.Return(0);
            return;
        }
        GuestArguments arguments{call};
        arguments.word = first_word;
        if (form != 0) {
            arguments.from_memory = true;
            arguments.jvalue = form == 2;
            arguments.address = call.Arg(static_cast<std::size_t>(first_word));
        }
        std::vector<jvalue> values;
        ReadArguments(ref.signature, arguments, values);
        if (!arguments.ok) {
            call.Return(0);
            return;
        }
        const std::size_t close = ref.signature.find(')');
        const char returns = close != std::string::npos && close + 1 < ref.signature.size()
                                 ? ref.signature[close + 1]
                                 : 'V';
        // The null-object diagnostic names the method it came from, and says
        // whether the framework threw something the guest then had to deal with.
        // A null answer from the guest's own file reader is silent to the
        // caller, so name the file that was asked for.
        if (ref.name == "getFileDataBytes") {
            if (values.size() == 1 && values[0].l != nullptr) {
                if (LogAllowed("getfiledata", 80)) {
                    const std::string path = StringOf(env, static_cast<jstring>(values[0].l));
                    ARMBAT_LOG_INFO("Armbat",
                                        "jni: getFileDataBytes \"%s\"", path.c_str());
                }
            } else if (LogAllowed("getfiledata-null", 8)) {
                ARMBAT_LOG_INFO("Armbat",
                                    "jni: getFileDataBytes with a null name: lr=0x%08x r0=0x%08x "
                                    "r1=0x%08x r2=0x%08x r3=0x%08x",
                                    current_env_lr, call.Machine().Reg(0), call.Machine().Reg(1),
                                    call.Machine().Reg(2), call.Machine().Reg(3));
            }
        }
        const std::string previous = current_call;
        current_call = ref.name + ref.signature;
        InvokeCall(call, returns, family == 2, family == 1, object, type, ref.method,
                   values.empty() ? nullptr : values.data());
        current_call = previous;
    }

    // Reads or writes one field. The field id carries the signature, so all
    // four families (instance/static, get/set) go through this one path.
    // NewObject has the same three shapes as the call family: the plain entry
    // point takes its arguments in registers, V a va_list and A a jvalue array.
    // All three end in the host's NewObjectA, because a guest va_list cannot be
    // forwarded to a host variadic function.
    void NewObjectThrough(HleCall& call, int form) {
        JNIEnv* env = HostEnv();
        const jclass type = static_cast<jclass>(Resolve(call.Arg(1)));
        const std::uint32_t method_id = call.Arg(2);
        if (type == nullptr || method_id == 0 || method_id > method_refs.size()) {
            ARMBAT_LOG_ERROR("Armbat",
                                "jni: NewObject with constructor id %u of %zu", method_id,
                                method_refs.size());
            call.Return(0);
            return;
        }
        const MethodRef& ref = method_refs[method_id - 1];
        if (ref.method == nullptr) {
            call.Return(0);
            return;
        }
        GuestArguments arguments{call};
        arguments.word = 3;
        if (form != 0) {
            arguments.from_memory = true;
            arguments.jvalue = form == 2;
            arguments.address = call.Arg(3);
        }
        std::vector<jvalue> values;
        ReadArguments(ref.signature, arguments, values);
        if (!arguments.ok) {
            call.Return(0);
            return;
        }
        const std::string previous = current_call;
        current_call = ref.name + ref.signature;
        const jobject created =
            env->NewObjectA(type, ref.method, values.empty() ? nullptr : values.data());
        current_call = previous;
        call.Return(Handle(env, created));
    }

    void FieldThrough(HleCall& call, bool is_static, bool is_set) {
        JNIEnv* env = HostEnv();
        jobject object = nullptr;
        jobject type = nullptr;
        std::uint32_t id = 0;
        if (is_static) {
            type = Resolve(call.Arg(1));
            id = call.Arg(2);
        } else {
            object = Resolve(call.Arg(1));
            id = call.Arg(2);
        }
        if (id == 0 || id > field_refs.size()) {
            call.Return(0);
            return;
        }
        const FieldRef& ref = field_refs[id - 1];
        // A null receiver is what a failed call upstream leaves behind; the
        // host would dereference it, so answer with zero instead.
        if (ref.field == nullptr || (is_static ? type == nullptr : object == nullptr)) {
            ARMBAT_LOG_ERROR("Armbat",
                                "jni: field %s on null receiver (static=%d)", ref.signature.c_str(),
                                is_static ? 1 : 0);
            call.Return(0);
            return;
        }
        const jclass cls = static_cast<jclass>(type);
        const char kind = ref.signature.empty() ? 'I' : ref.signature[0];

        if (is_set) {
            switch (kind) {
                case 'Z': {
                    const jboolean v = call.Arg(3) != 0 ? JNI_TRUE : JNI_FALSE;
                    if (is_static) env->SetStaticBooleanField(cls, ref.field, v);
                    else env->SetBooleanField(object, ref.field, v);
                    break;
                }
                case 'B': {
                    const jbyte v = static_cast<jbyte>(call.Arg(3));
                    if (is_static) env->SetStaticByteField(cls, ref.field, v);
                    else env->SetByteField(object, ref.field, v);
                    break;
                }
                case 'C': {
                    const jchar v = static_cast<jchar>(call.Arg(3));
                    if (is_static) env->SetStaticCharField(cls, ref.field, v);
                    else env->SetCharField(object, ref.field, v);
                    break;
                }
                case 'S': {
                    const jshort v = static_cast<jshort>(call.Arg(3));
                    if (is_static) env->SetStaticShortField(cls, ref.field, v);
                    else env->SetShortField(object, ref.field, v);
                    break;
                }
                case 'I': {
                    const jint v = static_cast<jint>(call.Arg(3));
                    if (is_static) env->SetStaticIntField(cls, ref.field, v);
                    else env->SetIntField(object, ref.field, v);
                    break;
                }
                case 'J': {
                    const jlong v = static_cast<jlong>(
                        (static_cast<std::uint64_t>(call.Arg(4)) << 32) | call.Arg(3));
                    if (is_static) env->SetStaticLongField(cls, ref.field, v);
                    else env->SetLongField(object, ref.field, v);
                    break;
                }
                case 'F': {
                    jfloat v = 0;
                    const std::uint32_t bits = call.Arg(3);
                    std::memcpy(&v, &bits, sizeof(bits));
                    if (is_static) env->SetStaticFloatField(cls, ref.field, v);
                    else env->SetFloatField(object, ref.field, v);
                    break;
                }
                case 'D': {
                    jdouble v = 0;
                    const std::uint64_t bits = (static_cast<std::uint64_t>(call.Arg(4)) << 32) |
                                               call.Arg(3);
                    std::memcpy(&v, &bits, sizeof(bits));
                    if (is_static) env->SetStaticDoubleField(cls, ref.field, v);
                    else env->SetDoubleField(object, ref.field, v);
                    break;
                }
                default: {
                    jobject v = Resolve(call.Arg(3));
                    if (is_static) env->SetStaticObjectField(cls, ref.field, v);
                    else env->SetObjectField(object, ref.field, v);
                    break;
                }
            }
            call.Return(0);
            return;
        }

        switch (kind) {
            case 'Z': {
                const jboolean v = is_static ? env->GetStaticBooleanField(cls, ref.field)
                                             : env->GetBooleanField(object, ref.field);
                call.Return(v != JNI_FALSE ? 1 : 0);
                break;
            }
            case 'B': {
                const jbyte v = is_static ? env->GetStaticByteField(cls, ref.field)
                                          : env->GetByteField(object, ref.field);
                call.Return(static_cast<std::uint32_t>(static_cast<std::int32_t>(v)));
                break;
            }
            case 'C': {
                const jchar v = is_static ? env->GetStaticCharField(cls, ref.field)
                                          : env->GetCharField(object, ref.field);
                call.Return(v);
                break;
            }
            case 'S': {
                const jshort v = is_static ? env->GetStaticShortField(cls, ref.field)
                                           : env->GetShortField(object, ref.field);
                call.Return(static_cast<std::uint32_t>(static_cast<std::int32_t>(v)));
                break;
            }
            case 'I': {
                const jint v = is_static ? env->GetStaticIntField(cls, ref.field)
                                         : env->GetIntField(object, ref.field);
                call.Return(static_cast<std::uint32_t>(v));
                break;
            }
            case 'J': {
                const jlong v = is_static ? env->GetStaticLongField(cls, ref.field)
                                          : env->GetLongField(object, ref.field);
                call.ReturnPair(static_cast<std::uint32_t>(v),
                                static_cast<std::uint32_t>(static_cast<std::uint64_t>(v) >> 32));
                break;
            }
            case 'F': {
                const jfloat v = is_static ? env->GetStaticFloatField(cls, ref.field)
                                           : env->GetFloatField(object, ref.field);
                std::uint32_t bits = 0;
                std::memcpy(&bits, &v, sizeof(bits));
                call.Return(bits);
                break;
            }
            case 'D': {
                const jdouble v = is_static ? env->GetStaticDoubleField(cls, ref.field)
                                            : env->GetDoubleField(object, ref.field);
                std::uint64_t bits = 0;
                std::memcpy(&bits, &v, sizeof(bits));
                call.ReturnPair(static_cast<std::uint32_t>(bits),
                                static_cast<std::uint32_t>(bits >> 32));
                break;
            }
            default: {
                jobject v = is_static ? env->GetStaticObjectField(cls, ref.field)
                                      : env->GetObjectField(object, ref.field);
                call.Return(Handle(env, v));
                break;
            }
        }
    }

    // Name of a JNIEnv entry by the number the guest calls it with. A slot with
    // no handler answers null for every request, and the bare number leaves the
    // reader guessing which platform call just disappeared.
    static const char* EnvName(std::size_t index) {
        static const std::array<const char*, kJniTableSize> names = [] {
            std::array<const char*, kJniTableSize> table{};
#include "jni_env_names.inc"
            return table;
        }();
        return index < names.size() ? names[index] : nullptr;
    }

    bool HandleSvc(std::uint32_t svc, A32Machine& machine_ref) {
        if (svc >= kJniVmBase && svc < kJniVmBase + vm_calls.size()) {
            HleCall call(machine_ref, arena);
            const std::size_t index = svc - kJniVmBase;
            if (vm_calls[index]) {
                vm_calls[index](call);
            } else {
                call.Return(0);
            }
            return true;
        }
        if (svc < kJniEnvBase || svc >= kJniEnvBase + kJniTableSize) {
            return false;
        }
        const std::size_t index = svc - kJniEnvBase;
        // A throwable left pending by the previous call cannot cross into this
        // one: every entry point except the four that inspect the exception
        // aborts under CheckJNI. See DropPendingException.
        if (index != JNI_IDX(ExceptionCheck) && index != JNI_IDX(ExceptionOccurred) &&
            index != JNI_IDX(ExceptionDescribe) && index != JNI_IDX(ExceptionClear)) {
            DropPendingException(HostEnv(), index);
        }
        current_env_index = index;
        current_env_lr = machine_ref.Reg(14);
        if (local_ref_count.load(std::memory_order_relaxed) > kLocalRefBudget) {
            TrimLocalRefs();
        }
        HleCall call(machine_ref, arena);
        if (index < jni_calls.size() && !seen[index]) {
            seen[index] = true;
            ARMBAT_LOG_INFO("Armbat",
                                "env call #%zu tid=%d r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x", index,
                                ::gettid(), machine_ref.Reg(0), machine_ref.Reg(1),
                                machine_ref.Reg(2), machine_ref.Reg(3));
        }
        {
            // The ring keeps the call's table index and the pc; the name is
            // rendered only if a hang report asks for it. Formatting a string
            // here ran on every JNI call the guest made, and asset loading makes
            // tens of thousands of them.
            imports.RecordJniCall(static_cast<std::uint32_t>(index), machine_ref.Reg(14));
        }
        if (index < jni_calls.size() && jni_calls[index]) {
            // The guest is about to hand control to Java: what it wrote into a
            // mirrored direct buffer has to be there before Java reads it.
            FlushDirectBuffers();
            jni_calls[index](call);
        } else {
            if (!warned[index]) {
                warned[index] = true;
                // The entry answers 0 to everything the guest asks, which looks
                // like missing platform data rather than a gap in the bridge;
                // name it so a missing entry is readable straight from a log.
                const char* entry = EnvName(index);
                Log("jni: unimplemented JNIEnv entry #" + std::to_string(index) +
                    (entry != nullptr ? std::string(" (") + entry + ")" : std::string(" (?)")) +
                    " answers 0");
            }
            call.Return(0);
        }
        return true;
    }

    // ---- registration ------------------------------------------------------

    std::string RegisterExports(JNIEnv* env, jobject class_loader) {
        this->loader = env->NewGlobalRef(class_loader);
        env->GetJavaVM(&g_vm);

        jclass class_class = env->FindClass("java/lang/Class");
        jclass method_class = env->FindClass("java/lang/reflect/Method");
        jmethodID get_methods = env->GetMethodID(class_class, "getDeclaredMethods",
                                                 "()[Ljava/lang/reflect/Method;");
        jmethodID method_get_name =
            env->GetMethodID(method_class, "getName", "()Ljava/lang/String;");
        jmethodID method_modifiers = env->GetMethodID(method_class, "getModifiers", "()I");
        jmethodID method_return = env->GetMethodID(method_class, "getReturnType", "()Ljava/lang/Class;");
        jmethodID method_params =
            env->GetMethodID(method_class, "getParameterTypes", "()[Ljava/lang/Class;");
        jmethodID class_name = env->GetMethodID(class_class, "getName", "()Ljava/lang/String;");

        std::string report;
        std::size_t registered = 0;
        std::size_t skipped = 0;

        // A library's JNI_OnLoad may already have registered natives and sealed
        // the region again before this sweep runs, so it has to be unsealed for
        // the whole walk and sealed once at the end.
        UnsealTrampolines();

        std::unordered_map<std::string, jclass> classes;
        // One Java_* entry point. It can come from a library the HLE loader
        // mapped or - on the bionic path - from a library the guest's own
        // linker loaded, which is why the source is a plain (symbol, address).
        auto consider = [&](const std::string& symbol, std::uint32_t address) {
            if (symbol.rfind("Java_", 0) != 0) {
                return;
            }
            std::vector<std::string> segments;
            std::string wanted_signature;
            if (!SplitJavaSymbol(symbol, &segments, &wanted_signature)) {
                return;
            }
            if (segments.size() < 2) {
                return;
            }

            // The class part may contain underscores, so try the longest split
            // first and keep the first class the loader knows.
            jclass type = nullptr;
            std::string type_name;
            std::string native_name;
            for (std::size_t split = segments.size() - 1; split >= 1; --split) {
                std::string candidate;
                for (std::size_t i = 0; i < split; ++i) {
                    if (i != 0) {
                        candidate += '/';
                    }
                    candidate += segments[i];
                }
                const std::string descriptor = "L" + candidate + ";";
                const auto known = classes.find(descriptor);
                if (known != classes.end()) {
                    type = known->second;
                    type_name = descriptor;
                } else {
                    jclass found = GuestClass(descriptor);
                    classes.emplace(descriptor, found);
                    if (found != nullptr) {
                        type = found;
                        type_name = descriptor;
                    }
                }
                if (type != nullptr) {
                    native_name = segments[split];
                    break;
                }
                if (split == 1) {
                    break;
                }
            }
            if (type == nullptr) {
                ++skipped;
                return;
            }

            // A name can cover several methods: one plain Java method and one
            // native, or several native overloads. RegisterNatives only accepts
            // native methods, so the candidates are filtered by ACC_NATIVE; a
            // mangled argument signature in the symbol then selects the exact
            // overload. A short name that still matches more than one native
            // method binds all of them, which is what dlsym does per method.
            std::vector<std::string> signatures;
            jobjectArray methods =
                static_cast<jobjectArray>(env->CallObjectMethod(type, get_methods));
            const jsize count = methods != nullptr ? env->GetArrayLength(methods) : 0;
            for (jsize i = 0; i < count; ++i) {
                jobject item = env->GetObjectArrayElement(methods, i);
                jstring name = static_cast<jstring>(env->CallObjectMethod(item, method_get_name));
                const bool named = StringOf(env, name) == native_name;
                env->DeleteLocalRef(name);
                if (!named) {
                    env->DeleteLocalRef(item);
                    continue;
                }
                if ((env->CallIntMethod(item, method_modifiers) & 0x0100) == 0) {
                    env->DeleteLocalRef(item);
                    continue;
                }
                std::string found =
                    SignatureOf(env, item, method_return, method_params, class_name);
                env->DeleteLocalRef(item);
                const std::size_t close = found.find(')');
                if (!wanted_signature.empty() &&
                    found.substr(1, close == std::string::npos ? 0 : close - 1) !=
                        wanted_signature) {
                    continue;
                }
                signatures.push_back(std::move(found));
            }
            if (methods != nullptr) {
                env->DeleteLocalRef(methods);
            }
            if (signatures.empty()) {
                ++skipped;
                return;
            }

            for (const std::string& signature : signatures) {
                natives.push_back(NativeMethod{address, signature});
                const std::uint32_t id = static_cast<std::uint32_t>(natives.size() - 1);
                void* trampoline = MakeTrampoline(id);
                if (trampoline == nullptr) {
                    report += "no trampoline for " + symbol + "\n";
                    natives.pop_back();
                    continue;
                }
                JNINativeMethod entry{const_cast<char*>(native_name.c_str()),
                                      const_cast<char*>(signature.c_str()), trampoline};
                if (env->RegisterNatives(type, &entry, 1) != JNI_OK) {
                    env->ExceptionClear();
                    ++skipped;
                    continue;
                }
                ++registered;
            }
            (void)type_name;
        };

        for (const LoadedLibrary& library : container.Modules().Libraries()) {
            for (const auto& [symbol, address] : container.Loader().Exports(library)) {
                consider(symbol, address);
            }
        }
        // The bionic path maps libraries with the guest's own linker, so they
        // never appear in the HLE module list. The container resolved their
        // Java_* entry points with dlsym and hands them over here.
        for (const GuestContainer::GuestExport& entry : container.BionicExports()) {
            consider(entry.symbol, entry.address);
        }

        SealTrampolines();
        report += "natives: " + std::to_string(registered) + " registered, " +
                  std::to_string(skipped) + " skipped\n";
        return report;
    }

    static std::string SignatureOf(JNIEnv* env, jobject method, jmethodID method_return,
                                   jmethodID method_params, jmethodID class_name) {
        std::string signature = "(";
        jobjectArray params =
            static_cast<jobjectArray>(env->CallObjectMethod(method, method_params));
        const jsize count = params != nullptr ? env->GetArrayLength(params) : 0;
        for (jsize i = 0; i < count; ++i) {
            jobject type = env->GetObjectArrayElement(params, i);
            signature += DescriptorOf(env, type, class_name);
            env->DeleteLocalRef(type);
        }
        if (params != nullptr) {
            env->DeleteLocalRef(params);
        }
        signature += ")";
        jobject result = env->CallObjectMethod(method, method_return);
        signature += DescriptorOf(env, result, class_name);
        if (result != nullptr) {
            env->DeleteLocalRef(result);
        }
        return signature;
    }

    static std::string DescriptorOf(JNIEnv* env, jobject type, jmethodID class_name) {
        jstring name = static_cast<jstring>(env->CallObjectMethod(type, class_name));
        const std::string text = StringOf(env, name);
        env->DeleteLocalRef(name);
        if (text == "int") return "I";
        if (text == "long") return "J";
        if (text == "boolean") return "Z";
        if (text == "byte") return "B";
        if (text == "char") return "C";
        if (text == "short") return "S";
        if (text == "float") return "F";
        if (text == "double") return "D";
        if (text == "void") return "V";
        if (!text.empty() && text[0] == '[') {
            std::string array = text;
            std::replace(array.begin(), array.end(), '.', '/');
            return array;
        }
        std::string object = text;
        std::replace(object.begin(), object.end(), '.', '/');
        return "L" + object + ";";
    }

};

JniBridge::Impl* JniBridge::Impl::current = nullptr;

std::uint64_t JniBridge::Impl::ArmBatNativeDispatch(std::uint64_t* frame,
                                                    std::uint64_t* stack_args, std::uint32_t id) {
    return current->Dispatch(frame, stack_args, id);
}

std::uint64_t JniBridge::Impl::Dispatch(std::uint64_t* frame, std::uint64_t* stack_args,
                                        std::uint32_t id) {
    if (id >= natives.size()) {
        return 0;
    }
    NativeMethod& method = natives[id];
    JNIEnv* env = HostEnv();
    if (env == nullptr) {
        return 0;
    }

    // ART reads a reference the native returns only after this function is gone,
    // so that one handle is released by the next native call on this thread.
    ReleaseRef(g_deferred_return.handle, g_deferred_return.guest_owned,
               g_deferred_return.generation);
    g_deferred_return = PendingRef{};

    // Everything this call hands the guest lives for exactly this call.
    const std::size_t frame_base = g_local_frames.size();
    g_local_frames.emplace_back();
    struct LocalFrame {
        Impl* self;
        std::size_t base;
        ~LocalFrame() { self->ReleaseLocalFrame(base); }
    } local_frame{this, frame_base};

    std::vector<std::uint32_t> args;
    args.push_back(env_pointer);
    args.push_back(Handle(env, reinterpret_cast<jobject>(frame[1])));

    std::size_t x = 2;      // x0/x1 are env and this
    std::size_t d = 0;
    std::size_t stack = 0;

    const auto next_int = [&]() -> std::uint32_t {
        if (x < 8) {
            return static_cast<std::uint32_t>(frame[x++]);
        }
        return static_cast<std::uint32_t>(stack_args[stack++]);
    };
    const auto next_pointer = [&]() -> std::uintptr_t {
        if (x < 8) {
            return static_cast<std::uintptr_t>(frame[x++]);
        }
        return static_cast<std::uintptr_t>(stack_args[stack++]);
    };
    const auto next_double_bits = [&]() -> std::uint64_t {
        if (d < 8) {
            return frame[8 + d++];
        }
        return stack_args[stack++];
    };

    const std::string& signature = method.signature;
    // Everything read after the call has to be copied out first: the guest may
    // add natives of its own while this one runs (see the deque above).
    const std::uint32_t entry = method.entry;
    const std::size_t close = signature.find(')');
    const char returns = close != std::string::npos && close + 1 < signature.size()
                             ? signature[close + 1]
                             : 'V';
    std::size_t i = 1;  // skip '('
    while (i < signature.size() && signature[i] != ')') {
        const char type = signature[i];
        switch (type) {
        case 'L':
        case '[': {
            // ART hands a native method its real references, not guest handles:
            // an object argument has to be registered with Handle() so the
            // 32-bit guest can name it. Resolve() here looked up the host pointer
            // as if it were one of our handles and produced null, which is how
            // setUpBreakpad(String) kept receiving a null path.
            if (type == 'L') {
                while (i < signature.size() && signature[i] != ';') {
                    ++i;
                }
                ++i;
            } else {
                ++i;
                while (i < signature.size() && signature[i] == '[') {
                    ++i;
                }
                if (i < signature.size() && signature[i] == 'L') {
                    while (i < signature.size() && signature[i] != ';') {
                        ++i;
                    }
                    ++i;
                } else if (i < signature.size()) {
                    ++i;  // primitive element type of an array
                }
            }
            args.push_back(Handle(env, reinterpret_cast<jobject>(next_pointer())));
            break;
        }
        case 'F':
            args.push_back(static_cast<std::uint32_t>(next_double_bits()));
            ++i;
            break;
        case 'D': {
            const std::uint64_t bits = next_double_bits();
            if (args.size() % 2 != 0) {
                args.push_back(0);
            }
            args.push_back(static_cast<std::uint32_t>(bits));
            args.push_back(static_cast<std::uint32_t>(bits >> 32));
            ++i;
            break;
        }
        case 'J': {
            // On arm64 a long is one 64-bit x register, not two 32-bit words.
            const std::uint64_t value = next_pointer();
            if (args.size() % 2 != 0) {
                args.push_back(0);  // AAPCS wants 64-bit values on an even slot
            }
            args.push_back(static_cast<std::uint32_t>(value));
            args.push_back(static_cast<std::uint32_t>(value >> 32));
            ++i;
            break;
        }
        case 'Z':
            args.push_back(next_int() & 1u);
            ++i;
            break;
        case 'B':
            args.push_back(static_cast<std::uint32_t>(static_cast<std::int8_t>(next_int())));
            ++i;
            break;
        case 'C':
            args.push_back(next_int() & 0xFFFFu);
            ++i;
            break;
        case 'S':
            args.push_back(static_cast<std::uint32_t>(static_cast<std::int16_t>(next_int())));
            ++i;
            break;
        case 'I':
            args.push_back(next_int());
            ++i;
            break;
        default:
            ++i;
            break;
        }
    }

    // A platform thread -- every ART thread, the engine's own included -- holds
    // host registers, a host stack and host TLS, and none of them can carry an
    // ARM frame. Such a call gets a guest context on that same thread, so the
    // Java methods the native calls back into still run where Android expects
    // them. A call that arrives from a guest thread (a Java method the guest
    // called, coming back in) nests on that thread's machine: it is executing
    // there and cannot be handed to anybody else.
    std::uint32_t r0 = 0;
    std::uint32_t r1 = 0;
    A32Machine* const current = CurrentGuestMachine();
    if (current != nullptr || !container.RunOnGuestThread(entry, args, &r0, &r1)) {
        A32Machine& target = current != nullptr ? *current : machine;
        if (target.Executing()) {
            target.CallNested(entry, args);
        } else {
            target.CallFunction(entry, args);
        }
        r0 = target.Reg(0);
        r1 = target.Reg(1);
    }

    // Java reads a direct buffer as soon as this method returns, so anything the
    // guest wrote into the arena copy has to be in Java's own memory by then.
    FlushDirectBuffers();

    // The guest answered in its own ABI: int in r0, long/double in r0:r1, an
    // object as one of our handles. ART, on the other side, wants a host jobject
    // in x0 and floating point in d0. Returning the raw r0 handed ART a small
    // handle where it expected a pointer.
    switch (returns) {
        case 'V':
            return 0;
        case 'L':
        case '[': {
            jobject result = Resolve(r0);
            if (result == nullptr) {
                static std::atomic<int> reported{0};
                if (r0 != 0 || reported.fetch_add(1) < 8) {
                    ARMBAT_LOG_WARN("Armbat",
                                        "jni: native 0x%08x %s returned r0=%u (null object)",
                                        entry, signature.c_str(), r0);
                }
            } else {
                DeferReturn(r0);
            }
            return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(result));
        }
        case 'Z':
            return r0 & 1u;
        case 'J':
        case 'D':
            return (static_cast<std::uint64_t>(r1) << 32) | r0;
        case 'F':
            return r0;  // the trampoline moves these bits into s0
        default:
            return r0;
    }
}

JniBridge::JniBridge(GuestContainer& container) : impl_(std::make_unique<Impl>(container)) {
    g_bridge.store(this, std::memory_order_release);
}

JniBridge::~JniBridge() {
    if (g_bridge.load(std::memory_order_acquire) == this) {
        g_bridge.store(nullptr, std::memory_order_release);
    }
    if (impl_->trampolines != nullptr) {
        munmap(impl_->trampolines, kTrampolineStride * kTrampolineCount);
    }
}

JNIEnv* JniBridge::HostEnvironment() const { return HostEnv(); }

jobject JniBridge::ResolveGuestObject(std::uint32_t handle) const { return impl_->Resolve(handle); }

std::uint32_t JniBridge::KeepObject(JNIEnv* env, jobject object) {
    return impl_->PersistentHandle(env, object);
}

JniBridge* CurrentJniBridge() { return g_bridge.load(std::memory_order_acquire); }

bool JniBridge::Prepare(std::string* error) {
    if (!impl_->Prepare(error)) {
        return false;
    }
    const std::uint32_t env = impl_->env_pointer;
    A32Machine& machine = impl_->machine;
    Impl* self = impl_.get();
    machine.AddSvcHandler(
        [self](std::uint32_t svc, A32Machine& target) { return self->HandleSvc(svc, target); });
    (void)env;
    Impl::current = impl_.get();
    return true;
}

std::string JniBridge::RegisterExports(JNIEnv* env, jobject loader) {
    if (env == nullptr) {
        return "natives: no JNIEnv\n";
    }
    const std::lock_guard lock(impl_->mutex);
    return impl_->RegisterExports(env, loader);
}

std::uint32_t JniBridge::GuestEnv() const { return impl_->env_pointer; }

std::uint32_t JniBridge::GuestVm() const { return impl_->vm_pointer; }

}  // namespace armbat
