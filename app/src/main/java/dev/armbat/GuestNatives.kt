package dev.armbat

import android.app.Activity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Surface

/**
 * The container's native world: the JIT, the guest heap and the archive's ARMv7
 * libraries.
 *
 * The Java part of a guest keeps running on the framework, but everything the
 * archive compiled for armeabi-v7a runs here. Both sides meet at the JNI bridge,
 * which is why the entry points live in one object.
 */
internal object GuestNatives {
    init {
        System.loadLibrary("armbat_jni")
    }

    /**
     * Maps every ARMv7 library of the archive and returns the loader's report.
     * Called before the guest's classes are instantiated: their static
     * initializers expect the libraries to be there. The target SDK the archive
     * was built for goes with it: bionic's linker keeps its legacy behaviour
     * (text relocations, DT_NEEDED repair) for applications targeting API 22 or
     * older, and nothing else tells it which application it is loading for.
     *
     * dataRoot and obbRoot are the guest's folders on the shared storage, the
     * stand-ins for /data/data/<package> and /sdcard/Android/obb/<package>;
     * packageName is the name the archive was built with, and sharedRoot is the
     * folder the guest's own /sdcard names.
     */
    external fun loadArchive(
        apkPath: String,
        cacheDir: String,
        targetSdk: Int,
        dataRoot: String,
        obbRoot: String,
        packageName: String,
        sharedRoot: String,
    ): String

    /**
     * Binds the archive's Java_* entry points in its ARMv7 libraries to the
     * classes of its Java part. The class loader that defines those classes is
     * the only one that can resolve their names, so it is passed along.
     */
    external fun registerNatives(loader: ClassLoader): String

    /**
     * Starts the archive's native activity. Mirrors what android.app.NativeActivity
     * does once it has loaded the library: the archive's ANativeActivity_onCreate
     * runs with the container's activity as its Java side.
     */
    external fun create(surface: Surface?)

    /** The surface exists: the archive's ANativeActivity gets its window. */
    external fun windowCreated(surface: Surface?)

    /** The surface is gone; the archive gets onNativeWindowDestroyed. */
    external fun windowDestroyed()

    /**
     * The window is active: the archive gets onInputQueueCreated, the way the
     * platform hands a native activity the queue it reads its touches from.
     * Delivered once, whatever view the archive installs.
     */
    external fun inputQueue()

    /**
     * One touch in the archive's own window pixels. A native activity reads
     * input through AInputQueue, not through the view it drew on, so the event
     * is queued there for the archive's own loop to pick up.
     *
     * Answers true when the archive is reading that queue: from then on the
     * touch belongs to it, exactly as on a device, and the Java view tree must
     * not also receive it.
     */
    external fun touch(event: MotionEvent): Boolean

    /**
     * One key in the archive's own window. A native activity reads its keys
     * through AInputQueue, never through the Java activity: the platform's
     * NativeActivity takes its window's input queue, so the view tree and the
     * activity's own dispatchKeyEvent never see a key that the archive's
     * native loop is waiting for.
     *
     * Answers true when the archive is reading that queue, exactly as [touch]
     * does. An archive without a native input loop is an ordinary Java window,
     * and the caller dispatches the key to the activity itself.
     */
    external fun key(event: KeyEvent): Boolean

    /**
     * The archive's own activity object, handed over before onCreate so the
     * native side reaches it through ANativeActivity.clazz, the way the
     * platform does it.
     */
    external fun activity(activity: Any)

    /** Forwards one android.app.Activity lifecycle step to the archive. */
    external fun lifecycle(event: Int)

    /**
     * Sets the guest Activity's mUiThread, which Activity.attach() normally does.
     * The platform denies this application's classes the reflection that field
     * needs, so native code writes it from a thread without Java frames there.
     */
    external fun setActivityUiThread(activity: Activity, thread: Thread): Boolean

    /**
     * Sets the guest Activity's mParent to the container's activity, the same
     * way and for the same reason as [setActivityUiThread]. The platform hands
     * a child's finish() and startActivityForResult() to its parent, which is
     * how the container keeps both inside the guest.
     */
    external fun setActivityParent(activity: Activity, parent: Activity): Boolean
}
