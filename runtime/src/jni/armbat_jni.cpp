#include <jni.h>

#include <android/log.h>

#include <mutex>
#include <string>
#include <string_view>

#include "armbat/guest_stdio.h"
#include "armbat/log.h"
#if ARMBAT_SELFTEST_IN_APK
#include "armbat/selftest.h"
#endif

namespace {

// A zygote child's stdout goes to /dev/null, so guest printf output is only
// visible in logcat if we copy it there ourselves.
void InstallLogSink() {
    static std::once_flag installed;
    std::call_once(installed, [] {
        armbat::SetStdioSink([](int fd, std::string_view text) {
            armbat::LogText(fd == 2 ? armbat::LogLevel::Error : armbat::LogLevel::Info, "Guest",
                            text);
        });
    });
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

}  // namespace

#if ARMBAT_SELFTEST_IN_APK
extern "C" JNIEXPORT jstring JNICALL
Java_dev_armbat_Native_runSelfTest(JNIEnv* env, jobject /*self*/) {
    InstallLogSink();

    const armbat::SuiteReport jit = armbat::RunJitSelftest();
    const armbat::SuiteReport loader = armbat::RunLoaderSelftest();

    std::string text = jit.text;
    text += "\n";
    text += loader.text;
    text += jit.Ok() && loader.Ok() ? "\nALL OK\n" : "\nFAILURES\n";

    // The shell shows this text, and logcat keeps a copy: a test that only lives
    // on screen cannot be read from a headless run.
    armbat::LogText(armbat::LogLevel::Info, "Armbat", text);

    return env->NewStringUTF(text.c_str());
}
#endif  // ARMBAT_SELFTEST_IN_APK

// The Java side logs through the same pipeline as the runtime, so one file
// holds the launch, the guest's output and the fault that followed. A logger
// per side would interleave two clocks and two formats in one session.
extern "C" JNIEXPORT jboolean JNICALL
Java_dev_armbat_ArmbatLog_nativeStart(JNIEnv* env, jobject /*self*/, jstring directory) {
    const std::string path = StringOf(env, directory);
    InstallLogSink();
    return armbat::LogStart(path) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_dev_armbat_ArmbatLog_nativeWrite(JNIEnv* env, jobject /*self*/, jint level, jstring tag,
                                      jstring message) {
    const std::string text = StringOf(env, message);
    if (text.empty()) {
        return;
    }
    const std::string name = StringOf(env, tag);
    armbat::LogText(static_cast<armbat::LogLevel>(level), name.empty() ? "Java" : name.c_str(),
                    text);
}

extern "C" JNIEXPORT void JNICALL
Java_dev_armbat_ArmbatLog_nativeFlush(JNIEnv* /*env*/, jobject /*self*/) {
    armbat::LogFlush();
}

// One level for the file and the mirror: the settings screen offers one choice,
// and a reader does not expect the two to disagree after flipping it.
extern "C" JNIEXPORT void JNICALL
Java_dev_armbat_ArmbatLog_nativeSetLevel(JNIEnv* /*env*/, jobject /*self*/, jint level) {
    const auto value = static_cast<armbat::LogLevel>(level);
    armbat::LogSetLevels(value, value);
}

extern "C" JNIEXPORT jint JNICALL
Java_dev_armbat_ArmbatLog_nativeLevel(JNIEnv* /*env*/, jobject /*self*/) {
    return static_cast<jint>(armbat::LogFileLevel());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_armbat_ArmbatLog_nativePath(JNIEnv* env, jobject /*self*/) {
    return env->NewStringUTF(armbat::LogFilePath().c_str());
}
