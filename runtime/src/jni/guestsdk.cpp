// A stand-in for the 64-bit library the platform insists on loading.
//
// An archive that ships only armeabi-v7a still calls System.loadLibrary in its
// static initializers, and the runtime resolves that name against the loader's
// native directories. There is no arm64 file to find, so the container puts this
// one there under the archive's own names: it loads, answers the JNI version and
// owns nothing. The archive's real ARMv7 libraries are mapped into the guest
// world by libarmbat_jni, where they actually run.
#include <jni.h>

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* /*vm*/, void* /*reserved*/) {
    return JNI_VERSION_1_6;
}
