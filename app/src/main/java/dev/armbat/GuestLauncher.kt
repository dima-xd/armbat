package dev.armbat

import android.app.Activity
import android.app.Application
import android.app.Instrumentation
import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.ContextWrapper
import android.content.Intent
import android.content.IntentFilter
import android.content.ServiceConnection
import android.content.SharedPreferences
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.content.res.Resources
import android.graphics.SurfaceTexture
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.os.Build
import android.os.IBinder
import android.os.Looper
import android.view.Display
import android.view.InputDevice
import android.view.KeyCharacterMap
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Surface
import android.view.View
import android.view.ViewGroup
import android.view.Window
import android.view.WindowManager
import android.widget.FrameLayout
import dalvik.system.DexClassLoader
import java.io.File
import java.io.FileOutputStream
import java.net.URL
import java.util.zip.ZipFile
import kotlin.math.min

/**
 * Runs an APK's Java part inside this process.
 *
 * The dex is loaded with DexClassLoader; the launchable Activity is instantiated
 * and given the pieces Activity.attach() would normally give it: a base context
 * whose resources come from the archive, a PhoneWindow, the application, the
 * main thread, the host's window token. The guest's decor view is then placed
 * into the host Activity's own content, so both windows share one real
 * ViewRootImpl and the framework handles drawing and touch.
 *
 * Activity.attach itself is not used: it is hidden and its parameter list changes
 * between releases. Everything here is either public API or a field that a
 * container has to set anyway.
 *
 * The guest sees a virtual display sized after the application's own era (see
 * [virtualSize]), not the physical one. Old games read the display size to lay
 * out their interface and then compare raw touch coordinates against it, so both
 * have to agree, and the physical screen of a current phone is far too tall for
 * them. The guest's picture is scaled to fit and the leftover space is left
 * black; touches are translated back into the virtual space.
 *
 * The app must be debuggable: fields like Activity.mWindow and the PhoneWindow
 * class are non-SDK interfaces, and Android only lets a debuggable process reach
 * them.
 */
class GuestLauncher(private val host: Activity) {

    /**
     * DexClassLoader over the archive's dex files, plus the archive itself as a
     * source of class-path resources.
     *
     * Old games (Doodle Jump included) read their game data with
     * Class.getResourceAsStream("/assets/data/..."), which asks the defining
     * class loader, not the package's AssetManager. A plain DexClassLoader only
     * looks at its parent and at the extracted dex directory, so those lookups
     * have to be answered from the APK's zip.
     */
    private class GuestClassLoader(
        dexPath: String,
        optimizedDirectory: String,
        libraryPath: String?,
        parent: ClassLoader,
        private val apkPath: String,
    ) : DexClassLoader(dexPath, optimizedDirectory, libraryPath, parent) {
        private val archive: String = File(apkPath).toURI().toURL().toString()

        /**
         * An archive whose activity derives from NativeActivity expects the
         * platform class to own the native side. The platform class only knows
         * how to dlopen an arm64 library, which an armeabi-v7a archive does not
         * have, so the container ships its own: a dex next to the archive's that
         * defines android.app.NativeActivity. The runtime insists that the
         * loader resolves the name to the very class it is defining, so the
         * parent has to be skipped here and the class defined by this loader.
         */
        override fun loadClass(name: String, resolve: Boolean): Class<*> {
            // Both names have to come from this dex rather than from the
            // platform: the platform's NativeActivity cannot host an arm32
            // library, and the platform's TelephonyManager refuses to answer a
            // process that targets a current SDK (see the class itself).
            if (name == NATIVE_ACTIVITY || name == TELEPHONY_MANAGER) {
                return findClass(name)
            }
            return super.loadClass(name, resolve)
        }

        override fun findResource(name: String): URL? {
            if (name.startsWith("assets/")) {
                val present = runCatching {
                    ZipFile(apkPath).use { it.getEntry(name) != null }
                }.getOrDefault(false)
                if (present) {
                    return URL("jar:$archive!/$name")
                }
            }
            return super.findResource(name)
        }
    }

    private class GuestContext(
        base: Context,
        private val guestResources: Resources,
        private val guestLoader: ClassLoader,
        private val guestApkPath: String,
        private val guestArchive: android.content.pm.ApplicationInfo?,
        private val guestLibraryDir: String?,
        private val guestDisplay: Display,
        private val guestPackage: String,
        private val startGuest: (Intent) -> Boolean,
    ) : ContextWrapper(base) {
        private var guestTheme: Resources.Theme? = null

        /**
         * Package queries from a guest describe the archive, not the container:
         * an SDK reads its own <meta-data> and its own version through this, and
         * the platform would answer both from the container's manifest.
         */
        private val guestPackageManager: android.content.pm.PackageManager by lazy {
            GuestPackageManager(
                baseContext.packageManager,
                guestArchive,
                guestApkPath,
                guestLibraryDir,
                baseContext.packageName,
            )
        }

        override fun getPackageManager(): android.content.pm.PackageManager = guestPackageManager

        /**
         * A package the device does not have answers the way the platform
         * answers for one that is not installed: with NameNotFoundException.
         *
         * The context may otherwise be built by the host, and the guest would
         * then read the phone's own copy of that package -- its resources, its
         * data, its code. An old game's bundled Google+ button asks Play
         * services for the context, resolves its own icon there, and calls
         * Resources.getDrawable(0) when only the modern package is present;
         * that exception is on the main thread and ends the process.
         */
        override fun createPackageContext(packageName: String, flags: Int): Context {
            if (GuestPackageManager.isAbsentPackage(packageName)) {
                throw android.content.pm.PackageManager.NameNotFoundException(
                    "$packageName is not installed",
                )
            }
            return super.createPackageContext(packageName, flags)
        }

        /**
         * The storage permissions the platform stopped granting.
         *
         * An archive built for the Android 4 era checks READ and
         * WRITE_EXTERNAL_STORAGE before it starts its engine. Both are capped by
         * target SDK on a current release: the platform answers "denied" to
         * every application, whatever the user chose, and the archive stops at
         * its own permission screen. The container cannot run at all without
         * all files access, which is strictly more than those permissions ever
         * gave, so the guest is answered from that grant.
         */
        override fun checkPermission(permission: String, pid: Int, uid: Int): Int {
            if (permission in LEGACY_STORAGE && GuestStorage.isAvailable()) {
                return android.content.pm.PackageManager.PERMISSION_GRANTED
            }
            return super.checkPermission(permission, pid, uid)
        }

        /**
         * The application info the guest sees is the archive's, with the paths
         * the container answers for it. nativeLibraryDir matters most: a Unity
         * application hands it to its native loader, which dlopens libunity.so
         * from it, and the container's own directory holds 64-bit stand-ins
         * instead of the guest's libraries.
         */
        private val guestApplicationInfo: android.content.pm.ApplicationInfo by lazy {
            val info = android.content.pm.ApplicationInfo(baseContext.applicationInfo)
            val archive = guestArchive
            if (archive != null) {
                if (archive.metaData != null) {
                    info.metaData = android.os.Bundle(archive.metaData)
                }
                info.className = archive.className
            }
            info.sourceDir = guestApkPath
            info.publicSourceDir = guestApkPath
            if (guestLibraryDir != null) {
                info.nativeLibraryDir = guestLibraryDir
            }
            info
        }

        override fun getApplicationInfo(): android.content.pm.ApplicationInfo = guestApplicationInfo

        /**
         * The package the guest believes it is.
         *
         * Library code resolves its own resources by name -- Nimble asks the
         * framework for "components.xml" of the package it thinks it is -- and
         * the framework would look for the container's package instead, find
         * nothing and hand back a null map. Paths the guest builds from this
         * name also have to be the archive's, or they point at the container's
         * (nonexistent) external directory.
         */
        override fun getPackageName(): String = guestPackage

        /** The virtual screen the application was written for, not the panel. */
        override fun getDisplay(): Display = guestDisplay

        override fun getResources(): Resources = guestResources

        /**
         * The application context of a guest has to resolve the archive's
         * resources: SDKs inflate their own layouts and read their own strings
         * through it, and the container's resources do not contain those ids.
         */
        override fun getApplicationContext(): Context = this

        /**
         * A cloned inflater, so layouts referenced by the archive's ids are
         * inflated with the archive's resources instead of the container's.
         */
        private val guestInflater: android.view.LayoutInflater by lazy {
            android.view.LayoutInflater.from(baseContext).cloneInContext(this)
        }

        /**
         * The phone the archive sees. It has to be the guest's own class, not
         * the platform's: the archive's code casts what it gets to the
         * TelephonyManager its class loader resolves, and on a current platform
         * the real one throws a SecurityException at the identifiers that this
         * era of application reads before it does anything else.
         */
        private val guestTelephony: Any? by lazy {
            runCatching {
                guestLoader.loadClass(TELEPHONY_MANAGER)
                    .getDeclaredConstructor()
                    .newInstance()
            }.getOrNull()
        }

        override fun getSystemService(name: String): Any? = when (name) {
            LAYOUT_INFLATER_SERVICE -> guestInflater
            android.content.Context.TELEPHONY_SERVICE -> guestTelephony ?: super.getSystemService(name)
            else -> super.getSystemService(name)
        }

        override fun getAssets() = guestResources.assets

        override fun getClassLoader(): ClassLoader = guestLoader

        /**
         * Old games locate their raw data inside the package by seeking in the
         * package file: they take the asset's offset from the real asset manager
         * and read it from getPackageResourcePath(). Answering with the container
         * would point those reads into the container's own APK  Doodle Jump
         * reads its sound bank that way and turns into white noise without this.
         */
        /**
         * The framework always answers with an existing private directory and
         * creates it on demand; a portable runtime such as HockeyApp's setup
         * code stores the path at startup and dies later if it is null.
         *
         * The directory is the guest's own folder on the shared storage rather
         * than the container's private one: the guest's Java code runs in this
         * process and touches the host filesystem directly, so a path the
         * native side maps would never be reached from here. Both sides answer
         * the same tree instead -- armbat/android/data/<package>.
         */
        override fun getFilesDir(): File {
            val dir = File(GuestStorage.dataDir(guestPackage), "files")
            if (!dir.exists()) {
                GuestStorage.ensure(guestPackage)
            }
            return dir
        }

        override fun getCacheDir(): File {
            val dir = File(GuestStorage.dataDir(guestPackage), "cache")
            if (!dir.exists()) {
                GuestStorage.ensure(guestPackage)
            }
            return dir
        }

        /**
         * The framework keeps the internal and the external files directory
         * apart, and the difference is not cosmetic: an application is free to
         * wipe its external tree, because everything in it is data the user
         * could have put there, and old builds of several games delete it on
         * first run to drop what an earlier download left behind. They delete
         * the directory through the path this call hands them.
         */
        override fun getExternalFilesDir(type: String?): File {
            val root = GuestStorage.externalFilesDir(guestPackage)
            val dir = if (type == null) root else File(root, type)
            if (!dir.exists()) {
                GuestStorage.ensure(guestPackage)
                dir.mkdirs()
            }
            return dir
        }

        override fun getExternalFilesDirs(type: String?): Array<File?> =
            arrayOf(getExternalFilesDir(type))

        override fun getExternalCacheDir(): File {
            val dir = GuestStorage.externalCacheDir(guestPackage)
            if (!dir.exists()) {
                GuestStorage.ensure(guestPackage)
                dir.mkdirs()
            }
            return dir
        }

        override fun getExternalCacheDirs(): Array<File?> = arrayOf(getExternalCacheDir())

        /**
         * Where an application looks for its expansion files. Answered with the
         * shared tree so both a game that asks the framework and one that builds
         * /sdcard/Android/obb/<package> itself (the kernel maps that path) find
         * the files the user put there.
         */
        override fun getObbDir(): File = GuestStorage.obbDir(guestPackage).also {
            if (!it.exists()) {
                GuestStorage.ensure(guestPackage)
            }
        }

        override fun getPackageResourcePath(): String = guestApkPath

        override fun getPackageCodePath(): String = guestApkPath

        /**
         * Old applications still pass MODE_WORLD_READABLE/MODE_WORLD_WRITEABLE,
         * which the framework has rejected since Android 7. The container has its
         * own private directory, so dropping those bits keeps the files private
         * and lets the application run.
         */
        private fun legacyMode(mode: Int): Int =
            mode and (MODE_WORLD_READABLE or MODE_WORLD_WRITEABLE).inv()

        override fun openFileOutput(name: String, mode: Int): FileOutputStream =
            super.openFileOutput(name, legacyMode(mode))

        override fun getSharedPreferences(name: String, mode: Int): SharedPreferences =
            super.getSharedPreferences(name, legacyMode(mode))

        /**
         * The container presents a device without Google services: nothing in it
         * ships the Play Store, Play services or the licensing service, and the
         * guest has to take the same "store unavailable" path it takes on such a
         * device. On a phone that happens to have them installed the guest would
         * otherwise connect to the real billing service and run a code path no
         * offline container can support.
         */
        private fun isUnavailableService(service: Intent): Boolean {
            // The billing helper binds with an implicit intent (no package), so
            // the action has to be checked as well as an explicit package name.
            val packageName = service.`package`
            if (packageName != null) {
                if (packageName == "com.android.vending" ||
                    packageName == "com.google.android.gms" ||
                    packageName == "com.google.android.gsf" ||
                    packageName == "com.google.android.gms.phenotype"
                ) {
                    return true
                }
            }
            val action = service.action ?: return false
            return action.startsWith("com.android.vending.") ||
                action.startsWith("com.google.android.gms.") ||
                action.startsWith("com.google.android.finsky.")
        }

        override fun bindService(service: Intent, conn: ServiceConnection, flags: Int): Boolean {
            if (isUnavailableService(service)) {
                ArmbatLog.i(TAG, "bindService refused: ${service.`package`} is not part of this device")
                return false
            }
            return super.bindService(service, conn, flags)
        }

        override fun startService(service: Intent): ComponentName? {
            if (isUnavailableService(service)) {
                ArmbatLog.i(TAG, "startService refused: ${service.`package`} is not part of this device")
                return null
            }
            return super.startService(service)
        }

        /**
         * An archive can also open one of its own screens through a Context a
         * helper only holds; Activity.startActivity goes through the guest
         * Instrumentation, this is the same rule for the other door.
         */
        override fun startActivity(intent: Intent) {
            if (startGuest(intent)) return
            super.startActivity(intent)
        }

        override fun startActivity(intent: Intent, options: android.os.Bundle?) {
            if (startGuest(intent)) return
            super.startActivity(intent, options)
        }

        /**
         * A receiver registered for a non-system broadcast must name EXPORTED or
         * NOT_EXPORTED since Android 14, and that rule is checked against the
         * process's own target SDK - which is the container's, not the archive's.
         * An archive written for an older platform therefore throws
         * SecurityException on a call that was legal when it was built, so the
         * container answers as the platform did then.
         */
        override fun registerReceiver(
            receiver: BroadcastReceiver?,
            filter: IntentFilter?,
        ): Intent? {
            if (Build.VERSION.SDK_INT < 33) {
                return super.registerReceiver(receiver, filter)
            }
            return try {
                super.registerReceiver(receiver, filter, RECEIVER_EXPORTED)
            } catch (error: SecurityException) {
                super.registerReceiver(receiver, filter, RECEIVER_NOT_EXPORTED)
            }
        }

        override fun registerReceiver(
            receiver: BroadcastReceiver?,
            filter: IntentFilter?,
            flags: Int,
        ): Intent? {
            if (Build.VERSION.SDK_INT < 33) {
                return super.registerReceiver(receiver, filter, flags)
            }
            if ((flags and (RECEIVER_EXPORTED or RECEIVER_NOT_EXPORTED)) != 0) {
                return super.registerReceiver(receiver, filter, flags)
            }
            return super.registerReceiver(receiver, filter, flags or RECEIVER_EXPORTED)
        }

        override fun getTheme(): Resources.Theme {
            var current = guestTheme
            if (current == null) {
                current = guestResources.newTheme()
                guestTheme = current
            }
            return current
        }
    }

    /**
     * The window manager a guest Activity sees.
     *
     * The platform answers getDefaultDisplay() from the manager's own context,
     * so a manager that belongs to the container's activity answers for the
     * panel -- 2772x1272 -- while the guest draws into a virtual screen of
     * 1280x720. Engines of the 2.x era read the display to map input: N.O.V.A 3
     * scales every touch by 1920/screenHeight before it hit-tests its own
     * canvas, so with the panel's size no touch reached the button it landed on.
     *
     * The manager for the virtual display is the platform's own: a display
     * context answers getDisplayNoVerify() with that display, which is exactly
     * what the guest has to see. It used to be a WindowManagerImpl subclass with
     * getDefaultDisplay() overridden, but the constructor such a subclass needs
     * is gone from the runtime on API 36 (NoSuchMethodError), so the guest
     * silently kept the container's manager.
     */
    private fun guestWindowManager(screen: Display): WindowManager {
        val manager = runCatching {
            host.createDisplayContext(screen).getSystemService(WindowManager::class.java)
        }.getOrNull()
        if (manager == null) {
            // A platform that refuses a display context still has to run the
            // guest: the panel's answers are then wrong, but only for a guest
            // that asks the window manager how big its screen is.
            ArmbatLog.w(TAG, "guest keeps the container's window manager")
            return host.windowManager
        }
        return manager
    }

    /**
     * Hosts the guest's decor view at the virtual aspect ratio and maps touches
     * into its coordinate space.
     *
     * The guest's own decor is laid out at the virtual size and then scaled, so
     * the framework maps window coordinates into the guest's pixels for us, per
     * pointer, keeping the action index, pressure and event time a native
     * activity reads. A gesture that starts in the letterbox belongs to no guest
     * window and never reaches the guest; a gesture the view tree did not take is
     * offered to the activity itself (see dispatchTouchEvent).
     */
    private class GuestFrame(
        context: Context,
        private var virtualWidth: Int,
        private var virtualHeight: Int,
        private val guest: Activity,
    ) : FrameLayout(context) {
        private var factor = 1f
        private var shiftX = 0f
        private var shiftY = 0f

        /**
         * Gives the guest the canvas of a new shape, after the window rotated.
         *
         * The decor was laid out at the old size, so it is measured again at
         * the new one: the guest's own views then re-layout for the screen they
         * now describe, and the picture fills the window instead of sitting in
         * a letterbox.
         */
        fun resize(width: Int, height: Int) {
            virtualWidth = width
            virtualHeight = height
            val child = getChildAt(0) ?: return
            child.layoutParams = FrameLayout.LayoutParams(width, height)
            requestLayout()
            fit()
        }

        fun fit() {
            if (width == 0 || height == 0) {
                return
            }
            factor = min(width / virtualWidth.toFloat(), height / virtualHeight.toFloat())
            shiftX = (width - virtualWidth * factor) / 2f
            shiftY = (height - virtualHeight * factor) / 2f
            val child = getChildAt(0) ?: return
            child.pivotX = 0f
            child.pivotY = 0f
            child.scaleX = factor
            child.scaleY = factor
            child.translationX = shiftX
            child.translationY = shiftY
            ArmbatLog.i(TAG, "guest surface ${virtualWidth}x$virtualHeight at ${factor}x, shift $shiftX,$shiftY")
        }

        override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
            super.onSizeChanged(w, h, oldw, oldh)
            fit()
        }

        private var forwarding = false

        override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
            // The guest's decor is laid out at the virtual size and scaled up, so
            // the framework converts window coordinates into the guest's own
            // pixels on the way down -- for every pointer at once, and without
            // touching the pointer ids, the action index, the pressure or the
            // event time that a native activity reads. The event is therefore
            // dispatched exactly as it arrived.
            if (ev.actionMasked == MotionEvent.ACTION_DOWN) {
                forwarding = false
                val x = (ev.x - shiftX) / factor
                val y = (ev.y - shiftY) / factor
                if (x < 0f || y < 0f || x >= virtualWidth || y >= virtualHeight) {
                    // The letterbox belongs to no window of the guest; a gesture
                    // begun there must not reach it.
                    return false
                }
            }
            if (super.dispatchTouchEvent(ev)) {
                return true
            }
            // Only a gesture this frame started belongs to the activity: a view
            // tree that consumed the down is not offered the rest of it.
            if (ev.actionMasked != MotionEvent.ACTION_DOWN && !forwarding) {
                return false
            }
            // Nothing in the guest's view tree wanted the event. An activity is
            // also allowed to read input in dispatchTouchEvent itself (DroidHen's
            // games route every touch to the engine there), and that override is
            // never reached by a decor view transplanted into another window. The
            // unconsumed event is offered to the activity in its own pixels; the
            // activity dispatches to the same decor again, which has just
            // declined it, so this cannot double-handle a gesture.
            val scaled = MotionEvent.obtain(ev)
            val matrix = android.graphics.Matrix()
            matrix.setScale(1f / factor, 1f / factor)
            matrix.postTranslate(-shiftX / factor, -shiftY / factor)
            scaled.transform(matrix)
            val handled = try {
                guest.dispatchTouchEvent(scaled)
            } finally {
                scaled.recycle()
            }
            // The activity's own dispatchTouchEvent may report false after it has
            // already fed the engine. The host stops sending a gesture to a view
            // that returned false on the down, so the frame has to keep it: an up
            // that never arrives leaves the guest's button stuck pressed.
            forwarding = ev.actionMasked != MotionEvent.ACTION_UP &&
                ev.actionMasked != MotionEvent.ACTION_CANCEL
            return handled || forwarding
        }
    }

    /**
     * One archive activity being hosted, with the window and the frame its
     * decor is placed in.
     *
     * An activity an archive started with startActivityForResult stays alive
     * until it is finished, exactly as a real back stack keeps the caller: the
     * result it waits for arrives from the activity above it.
     */
    private class Hosted(
        val info: ActivityInfo,
        val activity: Activity,
        val window: Window,
        val frame: GuestFrame,
        /** The code the caller asked for this activity's result with. */
        val requestCode: Int,
    ) {
        var started = false
        var resumed = false

        /** The archive called finish(); the result still has to be handed back. */
        var finished = false
        var destroyed = false
    }

    /** One request the archive made about its own activities. */
    private sealed interface Action {
        class Start(
            val className: String,
            val intent: Intent,
            val requestCode: Int,
        ) : Action

        class Finish(val hosted: Hosted) : Action
    }

    /**
     * One launch of an archive: everything that outlives a single activity.
     *
     * An archive may hand its startup from a permission screen to its engine's
     * own activity, and a game may open another one of its own later. All of
     * them share the guest userland, the Application object and the virtual
     * display; only the activity, its window and the frame in the host change.
     */
    private class Session(
        val prepared: Prepared,
        val context: GuestContext,
        val application: Application,
        val screen: VirtualScreen,
        var virtualWidth: Int,
        var virtualHeight: Int,
    ) {
        /** The archive's activities, bottom first; the last one is on screen. */
        val stack = ArrayDeque<Hosted>()
    }

    private class VirtualScreen(
        val display: VirtualDisplay,
        val surface: Surface,
        val texture: SurfaceTexture,
    )

    private var session: Session? = null
    private var hosted: Hosted? = null

    /**
     * Whether this process has already been claimed by a guest world.
     *
     * The native side keeps exactly one container per process: a second
     * loadArchive returns the first world instead of building a new one. The
     * Java side is bound to that world through JNI ids and one class loader, and
     * ART refuses to map the same library into a second loader. There is no way
     * back, so the process ends with its guest (see [endWorld]).
     */
    private var worldLoaded = false


    /**
     * Activity requests the archive made, in the order it made them.
     *
     * startActivity and finish run inside a guest callback, so the activity
     * that asked cannot be replaced in the middle of its own onCreate: the
     * request is queued and [pump] applies it between lifecycle steps. A
     * request that arrives from an event handler is drained from the looper
     * instead, once that handler has unwound.
     */
    private val pending = java.util.concurrent.ConcurrentLinkedQueue<Action>()
    private var pumping = false

    /**
     * Called once the archive has no activity left: the guest is done and the
     * host should show its own screen again.
     */
    var onFinished: (() -> Unit)? = null

    /** Called on the UI thread for every step of the boot sequence. */
    var onStage: ((LaunchStage) -> Unit)? = null

    /**
     * The guest's Instrumentation.
     *
     * Activity.startActivityForResult always hands the intent to
     * Instrumentation.execStartActivity. The platform marks that method hidden,
     * so it is absent from the SDK, but it is public and virtual in the runtime:
     * a method of the same name and descriptor below overrides it, and the
     * framework's call lands here instead of in the platform's implementation.
     * An archive is not installed, so the platform has no record of its
     * activities and answers an explicit intent naming one with
     * ActivityNotFoundException; the container hosts the requested activity
     * itself and never asks the platform for it.
     *
     * Anything the archive does not own still leaves the process through the
     * container's own activity, exactly as it did when the guest borrowed the
     * container's Instrumentation.
     */
    private inner class GuestInstrumentation : Instrumentation() {
        @Suppress("UNUSED_PARAMETER")
        fun execStartActivity(
            who: Context,
            contextThread: IBinder?,
            token: IBinder?,
            target: Activity?,
            intent: Intent,
            requestCode: Int,
            options: android.os.Bundle?,
        ): Instrumentation.ActivityResult? {
            if (startGuestActivity(intent, requestCode)) {
                return null
            }
            ArmbatLog.i(TAG, "guest startActivity leaves the container: $intent")
            host.startActivity(intent, options)
            return null
        }
    }

    private val instrumentation by lazy { GuestInstrumentation() }

    /**
     * Everything a launch needs that does not touch the view hierarchy, done on
     * a worker thread.
     *
     * Booting the guest userland, mapping the archive's ARMv7 libraries and
     * running their JNI_OnLoad take seconds, and the platform kills an activity
     * whose main thread is unavailable for 5. The framework's input dispatcher
     * is what enforces that, so the work has to happen off the main thread.
     */
    private class Prepared(
        val name: String,
        val activityActivity: android.content.pm.ActivityInfo,
        val loader: ClassLoader,
        val resources: Resources,
        val archiveInfo: android.content.pm.ApplicationInfo?,
        val targetSdk: Int,
        val apkPath: String,
        val screenOrientation: Int,
        val packageName: String,
        val virtualWidth: Int,
        val virtualHeight: Int,
        val densityDpi: Int,
    )

    private fun prepare(apkPath: String): Prepared {
        val activity = launchableActivity(apkPath)
            ?: throw IllegalStateException(host.getString(R.string.error_no_activity))
        val name = activity.name
        // The archive's package names its data and obb folder on the shared
        // storage. The guest itself answers as the container's package, so this
        // is read from the manifest rather than from the guest's context.
        @Suppress("DEPRECATION")
        val archivePackage = host.packageManager.getPackageArchiveInfo(apkPath, 0)
            ?.packageName.orEmpty()
        GuestStorage.ensure(archivePackage)
        ArmbatLog.i(TAG, "guest package: $archivePackage")
        ArmbatLog.i(TAG, "guest activity: $name theme=0x${archiveTheme(activity).toString(16)}")
        // The window is rotated while the archive is being prepared, not once
        // it starts: mapping its libraries takes seconds and the platform
        // rotates asynchronously, so asking here means the guest opens on a
        // window that already has the shape its manifest asked for.
        if (activity.screenOrientation != ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED) {
            ArmbatLog.i(TAG, "guest requests orientation ${activity.screenOrientation}")
            host.requestedOrientation = activity.screenOrientation
        }

        val dexFiles = GuestDex.prepare(
            host,
            apkPath,
            archivePackage,
            GuestStorage.dataDir(archivePackage),
            GuestStorage.obbDir(archivePackage),
            ::stage,
        )
        if (dexFiles.isEmpty()) {
            throw IllegalStateException(host.getString(R.string.error_no_dex))
        }
        // The archive's own target SDK, read before anything native happens: the
        // guest's linker needs it to keep the behaviour this build was tested
        // against.
        val targetSdk = activity.applicationInfo?.targetSdkVersion ?: 0
        stage(LaunchStage.NATIVE)
        val nativeDir = nativeLibraries(apkPath)
        val loader = GuestClassLoader(
            dexFiles.joinToString(File.pathSeparator) { it.absolutePath },
            host.codeCacheDir.absolutePath,
            nativeDir,
            host.classLoader,
            apkPath,
        )
        // The archive's own libraries have to be mapped before its static
        // initializers run: they call System.loadLibrary and then straight
        // into native code. The two data roots are the guest's folders on the
        // shared storage; the runtime answers /data/data/<package> and
        // /sdcard/Android/{data,obb}/<package> from them.
        GuestNatives.loadArchive(
            apkPath,
            host.codeCacheDir.absolutePath,
            targetSdk,
            GuestStorage.dataDir(archivePackage).absolutePath,
            GuestStorage.obbDir(archivePackage).absolutePath,
            archivePackage,
        )
        // From here the process belongs to this guest world; a second launch
        // cannot build another one (see worldLoaded).
        worldLoaded = true
        // The archive's native methods have to be bound before its Java part
        // calls any of them, which starts with onCreate.
        GuestNatives.registerNatives(loader)
        val type = loader.loadClass(name)
        if (!Activity::class.java.isAssignableFrom(type)) {
            throw IllegalStateException(host.getString(R.string.error_not_activity, name))
        }
        // The screen the guest is going to draw on is fixed here, before its
        // resources exist: an archive layouts its interface from the screen the
        // resources describe, and those have to be the pixels of the surface it
        // renders into, not the panel's.
        val landscape = wantsLandscape(activity.screenOrientation)
        val (virtualWidth, virtualHeight) = virtualSize(targetSdk, landscape)
        val densityDpi = virtualDensity(minOf(virtualWidth, virtualHeight))
        val screen = ApkResources.Screen(virtualWidth, virtualHeight, densityDpi)
        stage(LaunchStage.RESOURCES)
        val resources = ApkResources.of(host, apkPath, screen) ?: host.resources
        val archiveInfo = runCatching {
            host.packageManager.getPackageArchiveInfo(
                apkPath,
                PackageManager.GET_META_DATA,
            )?.applicationInfo
        }.getOrNull()
        return Prepared(
            name = name,
            activityActivity = activity,
            loader = loader,
            resources = resources,
            archiveInfo = archiveInfo,
            targetSdk = targetSdk,
            apkPath = apkPath,
            screenOrientation = activity.screenOrientation,
            packageName = archivePackage,
            virtualWidth = virtualWidth,
            virtualHeight = virtualHeight,
            densityDpi = densityDpi,
        )
    }

    /**
     * Builds the guest's world and hosts the archive's first activity. Runs on
     * the main thread: everything it creates belongs to the view hierarchy.
     */
    private fun start(prepared: Prepared) {
        // From here on the process carries an application: it has to keep the
        // importance a running one has, or being covered by another window is
        // enough for the platform to end it.
        GuestService.start(host)
        stage(LaunchStage.SURFACE)
        val targetSdk = prepared.targetSdk
        // An application that declares an orientation gets the window it asked
        // for. The platform rotates the whole activity for an application
        // installed on the device, and a guest that asked for a landscape screen
        // but was handed a portrait surface draws a rotated picture -- or a
        // letterboxed strip -- however correct its own layout is. The virtual
        // screen takes the same aspect, so the picture fills the window at the
        // resolution the application was written for.
        val landscape = wantsLandscape(prepared.screenOrientation)
        ArmbatLog.i(TAG, "guest orientation ${prepared.screenOrientation} landscape=$landscape")
        if (prepared.screenOrientation != ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED) {
            host.requestedOrientation = prepared.screenOrientation
        }
        // The window belongs to a current application, so a 120 Hz phone gives
        // it 120 frames a second, and a guest that steps its world once per
        // frame runs at double speed. Ask the display for the rate the guest
        // was written for; a zero rate means "no preference".
        val panel = host.window.attributes
        panel.preferredRefreshRate = GUEST_FRAME_RATE
        host.window.attributes = panel
        val (virtualWidth, virtualHeight) = prepared.virtualWidth to prepared.virtualHeight
        ArmbatLog.i(
            TAG,
            "guest screen ${virtualWidth}x$virtualHeight at ${prepared.densityDpi}dpi " +
                "for targetSdk $targetSdk",
        )
        val screen = createVirtualDisplay(virtualWidth, virtualHeight, prepared.densityDpi)
        val context = GuestContext(
            host,
            prepared.resources,
            prepared.loader,
            prepared.apkPath,
            prepared.archiveInfo,
            GUEST_LIBRARY_DIR,
            screen.display.display,
            prepared.packageName,
            startGuest = { startGuestActivity(it, -1) },
        )
        // The platform creates the archive's Application before its first
        // activity and answers getApplication() with it. An archive keeps
        // process-wide state there (Fruit Ninja's Application caches its own
        // instance, and every helper that needs a ContentResolver reads it), so
        // the container's Application is not a substitute: that state stays
        // null and the guest's first framework call throws.
        val application = guestApplication(prepared, context)
        val session = Session(prepared, context, application, screen, virtualWidth, virtualHeight)
        this.session = session
        stage(LaunchStage.ACTIVITY)
        request(
            Action.Start(
                prepared.name,
                // The intent names the archive, so an application that reads
                // the component it was started with sees the package its
                // PackageManager knows, not the container's.
                Intent().setClassName(prepared.packageName, prepared.name),
                -1,
            ),
        )
    }

    /**
     * Applies every queued activity request until the archive makes no more.
     *
     * A request runs the lifecycle steps the platform would run and asks after
     * each of them whether the archive has already moved on: an activity that
     * hands over to another one from its own onCreate is never started, and one
     * that finishes itself is never resumed. The queue is applied here, after
     * the guest callback that filled it has returned, never in the middle of
     * that callback.
     */
    private fun pump() {
        if (pumping) {
            return
        }
        pumping = true
        try {
            while (true) {
                val session = session ?: break
                val action = pending.poll() ?: break
                when (action) {
                    is Action.Start -> open(session, action)
                    is Action.Finish -> close(session, action.hosted)
                }
            }
        } catch (error: Throwable) {
            ArmbatLog.e(TAG, "guest activity failed", error)
        } finally {
            pumping = false
        }
    }

    /** Queues a request, and applies it as soon as the caller has unwound. */
    private fun request(action: Action) {
        pending.add(action)
        if (pumping) {
            return
        }
        if (Looper.myLooper() == Looper.getMainLooper()) {
            pump()
        } else {
            host.window.decorView.post { pump() }
        }
    }

    /** Builds an activity the archive asked for and drives it to the screen. */
    private fun open(session: Session, start: Action.Start) {
        val caller = session.stack.lastOrNull()
        if (caller != null) {
            suspendHosted(caller)
        }
        val created = createHosted(session, start)
        if (pending.isNotEmpty()) {
            return
        }
        startHosted(created)
        if (pending.isNotEmpty()) {
            return
        }
        resumeHosted(created)
        if (pending.isNotEmpty()) {
            return
        }
        settle(created)
    }

    /**
     * Ends an activity the archive finished and hands its result to the
     * activity that asked for it, which is then resumed.
     */
    private fun close(session: Session, hosted: Hosted) {
        if (hosted.destroyed) {
            return
        }
        if (session.stack.lastOrNull() !== hosted) {
            // It finished while another activity was on top of it: it is
            // already suspended, and nothing is waiting for its result yet.
            session.stack.remove(hosted)
            destroyHosted(hosted)
            // The activity above it may have been created while this finish was
            // still queued (see open): the archive started the next screen and
            // ended itself from the same onCreate, so the open that built the
            // new one stopped before any of its own lifecycle steps. Nothing
            // else would drive it, and the archive would sit on a created but
            // never shown activity.
            val top = session.stack.lastOrNull() ?: return
            if (top.finished || top.destroyed) {
                return
            }
            if (!top.started) {
                startHosted(top)
                if (pending.isNotEmpty()) {
                    return
                }
            }
            if (!top.resumed) {
                resumeHosted(top)
                if (pending.isNotEmpty()) {
                    return
                }
            }
            if (this.hosted !== top) {
                settle(top)
            }
            return
        }
        session.stack.removeLast()
        destroyHosted(hosted)
        val caller = session.stack.lastOrNull()
        if (caller == null) {
            guestFinished()
            return
        }
        deliverResult(caller, hosted)
        if (!caller.started) {
            startHosted(caller)
            if (pending.isNotEmpty()) {
                return
            }
        }
        if (!caller.resumed) {
            resumeHosted(caller)
            if (pending.isNotEmpty()) {
                return
            }
        }
        settle(caller)
    }

    /**
     * The archive finished every activity it had: its session is over, and the
     * host is told so it can show its own screen again.
     */
    private fun guestFinished() {
        ArmbatLog.i(TAG, "guest finished: the archive has no activity left")
        destroy()
        onFinished?.invoke()
    }

    /**
     * The framework's ActivityInfo for an activity of the archive.
     *
     * The archive is not installed, so its manifest entry is read out of the
     * package file: an activity that asked for a theme of its own, or for
     * orientation and configuration changes, has to be given them exactly as a
     * device would.
     */
    private fun archiveActivity(prepared: Prepared, className: String): ActivityInfo? {
        @Suppress("DEPRECATION")
        val archive = host.packageManager.getPackageArchiveInfo(
            prepared.apkPath,
            PackageManager.GET_ACTIVITIES or PackageManager.GET_META_DATA,
        ) ?: return null
        return archive.activities?.firstOrNull { it.name == className }
    }

    /**
     * The theme an activity of the archive runs with.
     *
     * The manifest can declare the theme on <application> instead of on the
     * activity, and the platform then answers ActivityInfo.getThemeResource
     * with the application's. An archive that declares it only there was being
     * started with the platform's default theme instead: a Holo title/action
     * bar the game never accounts for, taking 48dp off a full-screen surface
     * and shifting the picture down.
     */
    private fun archiveTheme(archive: ActivityInfo): Int =
        if (archive.theme != 0) archive.theme else archive.applicationInfo?.theme ?: 0

    /**
     * The ActivityInfo the guest sees. attach() publishes it, and Activity
     * reads the parent activity, theme and orientation from it.
     *
     * The identity fields are the archive's, like getPackageName() and the
     * component: an activity that reports the container's package here while
     * its context answers the archive's name sends every PackageManager round
     * trip -- AppCompat resolves the parent activity that way -- to a package
     * the container's manifest does not declare.
     */
    private fun hostedInfo(archive: ActivityInfo): ActivityInfo {
        val info = ActivityInfo()
        info.packageName = archive.packageName
        info.name = archive.name
        info.applicationInfo = archive.applicationInfo ?: host.applicationInfo
        info.theme = archiveTheme(archive)
        // NativeActivity takes the library it has to start from the activity's
        // own meta-data, so the archive's entry has to survive.
        info.metaData = archive.metaData
        info.screenOrientation = archive.screenOrientation
        info.configChanges = archive.configChanges
        return info
    }

    /**
     * Instantiates one archive activity and gives it everything attach() would
     * normally provide. onCreate is already run; the caller drives the rest.
     */
    private fun createHosted(session: Session, request: Action.Start): Hosted {
        val prepared = session.prepared
        val archive = archiveActivity(prepared, request.className)
            ?: throw IllegalStateException(
                host.getString(R.string.error_activity_not_declared, request.className),
            )
        val type = prepared.loader.loadClass(request.className)
        val activity = type.getDeclaredConstructor().newInstance() as Activity
        val context = session.context
        val theme = archiveTheme(archive)
        if (theme != 0) {
            // PhoneWindow takes its theme from the context as well as from the
            // Activity, so the archive's theme has to land here: otherwise the
            // decor grows a title/action bar that the guest does not know about.
            context.theme.applyStyle(theme, true)
        }
        attachBaseContext(activity, context)
        attachFragments(activity)
        field(activity, "mResources", prepared.resources)
        field(activity, "mApplication", session.application)
        // Activity.runOnUiThread compares against mUiThread, which attach() sets
        // and the non-SDK policy denies this application's classes (max-target-o).
        // Without it every runOnUiThread only posts: work an archive queues from
        // onCreate is still pending when onResume runs, and Jetpack Joyride's
        // Facebook client dereferences the singleton its posted runnable has not
        // created yet. The field is written from native code instead.
        if (!GuestNatives.setActivityUiThread(activity, Looper.getMainLooper().thread)) {
            ArmbatLog.w(TAG, "guest ui thread not set")
        }
        optional(activity, "mMainThread", activityThread())
        // The guest activity is a child of the container's activity, which is
        // how an embedded activity is declared to the platform: the questions
        // it asks about the task it lives in (isTaskRoot, the task id, the
        // orientation) are answered for the task the container runs in, and
        // finish() comes back to the host instead of ending the container.
        optional(activity, "mToken", hostToken)
        // Written from native: the non-SDK policy denies this application's
        // classes the reflection for it (see GuestNatives.setActivityParent).
        if (!GuestNatives.setActivityParent(activity, host)) {
            ArmbatLog.w(TAG, "guest parent not set")
        }
        optional(activity, "mInstrumentation", instrumentation)

        optional(activity, "mActivityInfo", hostedInfo(archive))
        // attach() publishes the application info too, and Activity.getTheme()
        // reads the archive's target SDK from it to choose the platform default
        // theme. Without it the theme starts empty and the first View the
        // archive inflates fails on an android:* attribute.
        optional(activity, "mApplicationInfo", archive.applicationInfo ?: host.applicationInfo)
        // The activity names the archive's package, not the container's: the
        // guest context answers getPackageName() with the archive's name, and
        // framework code that turns the component back into a PackageManager
        // query -- AppCompat asks NavUtils for the parent activity -- has to
        // land in GuestPackageManager, where only the archive's name is known.
        // With the container's name the query misses the archive's manifest and
        // surfaces as IllegalArgumentException from AppCompatDelegate onCreate.
        optional(
            activity, "mComponent",
            ComponentName(prepared.packageName, request.className),
        )

        // The guest's own window: its decor is what gets transplanted into the
        // host, but the window still behaves normally for the guest's
        // setContentView, findViewById and theme calls. Layout-in-screen keeps
        // the decor from insetting itself for system bars, which would shift
        // the picture away from the coordinates the guest hit-tests.
        //
        // The platform builds the window against the activity, not against the
        // activity's base context: the activity is the ContextThemeWrapper that
        // owns the theme, and the window's inflater and every view it creates
        // read the theme from there.
        val window = newWindow(activity)
        // A guest dialog is a window of its own, and the platform inherits the
        // dialog's token from the window manager's parent window. Point the
        // activity at a local manager whose parent is this window and give the
        // window the host's token; without both, Activity.showDialog throws
        // BadTokenException from an archive's background thread. The package
        // stays the container's here: this window is registered with the
        // platform, which knows no package but the installed one.
        window.setWindowManager(
            guestWindowManager(session.screen.display.display),
            hostToken,
            host.packageName,
        )
        field(activity, "mWindowManager", window.windowManager)
        field(activity, "mWindow", window)
        window.callback = activity
        val attributes = window.attributes
        attributes.flags = attributes.flags or
            WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN or
            WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
        window.attributes = attributes

        // The archive's theme is what removes the title bar and the action
        // bar; without it the guest gets the platform default and its content
        // is laid out below a strip that the guest does not account for.
        if (theme != 0) {
            activity.setTheme(theme)
        }
        activity.setIntent(request.intent)

        // The activity joins the stack before onCreate runs. An archive may
        // finish an activity from inside its own onCreate -- Marmalade's
        // permission screen starts the loader and then ends itself -- and
        // finish() arrives here as finishFromChild on the host. An activity the
        // session does not know yet is mistaken for a foreign child, and the
        // host ends instead: the window goes away while the guest keeps running
        // behind it. Registered first, the same finish() becomes the ordinary
        // "finish while creating" the queue already supports.
        val frame = GuestFrame(host, session.virtualWidth, session.virtualHeight, activity)
        val hosted = Hosted(hostedInfo(archive), activity, window, frame, request.requestCode)
        session.stack.addLast(hosted)
        try {
            lifecycle(activity, "onCreate", null)
        } catch (error: Throwable) {
            session.stack.remove(hosted)
            throw error
        }
        ArmbatLog.i(TAG, "lifecycle: onCreate returned")
        // The window's decor exists only once onCreate has set its content, so
        // it is transplanted into the frame after the callback, not before it.
        frame.addView(
            window.decorView,
            FrameLayout.LayoutParams(session.virtualWidth, session.virtualHeight),
        )
        return hosted
    }

    private fun startHosted(hosted: Hosted) {        lifecycle(hosted.activity, "onStart")
        hosted.started = true
        ArmbatLog.i(TAG, "lifecycle: onStart returned")
    }

    private fun resumeHosted(hosted: Hosted) {
        lifecycle(hosted.activity, "onResume")
        hosted.resumed = true
        ArmbatLog.i(TAG, "lifecycle: onResume returned")
        // The platform reports the focus of the window to the activity right
        // after the resume, and an archive may drive its whole engine from
        // there: Unity's player only wakes up in onWindowFocusChanged. The
        // guest's decor never sees the host's focus, so the call is made here.
        windowFocus(hosted.activity, true)
    }

    /** Puts the activity's decor into the host and makes it the visible one. */
    private fun settle(hosted: Hosted) {
        val frame = hosted.frame
        // The guest decor is not a window of its own, so the host's system bar
        // insets must not reach it: they would shift its content down inside
        // the virtual area while the guest still hit-tests from 0.
        frame.setOnApplyWindowInsetsListener { _, _ -> android.view.WindowInsets.CONSUMED }
        stage(LaunchStage.READY)
        host.setContentView(frame)
        androidx.core.view.WindowCompat.setDecorFitsSystemWindows(host.window, false)
        val bars = androidx.core.view.WindowCompat.getInsetsController(host.window, frame)
        bars.hide(androidx.core.view.WindowInsetsCompat.Type.systemBars())
        bars.systemBarsBehavior =
            androidx.core.view.WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        this.hosted = hosted
        ArmbatLog.i(TAG, "guest activity shown: ${hosted.info.name}")
    }

    /**
     * Takes an activity off the screen without ending it.
     *
     * Another activity of the archive is above it now, so it goes through the
     * two steps the platform runs for a covered activity, and keeps everything
     * it built: the frame, the views and its state stay for the moment it is
     * resumed again.
     */
    private fun suspendHosted(hosted: Hosted) {
        if (hosted.resumed) {
            windowFocus(hosted.activity, false)
            lifecycle(hosted.activity, "onPause")
            hosted.resumed = false
        }
        if (hosted.started) {
            lifecycle(hosted.activity, "onStop")
            hosted.started = false
        }
        if (this.hosted === hosted) {
            this.hosted = null
        }
    }

    /**
     * Ends an activity for good.
     *
     * The archive's finish() arrives here through the host (see createHosted)
     * instead of through the activity service, so the container ends the
     * activity itself, in the order the platform would.
     */
    private fun destroyHosted(hosted: Hosted) {
        if (hosted.destroyed) {
            return
        }
        hosted.destroyed = true
        if (this.hosted === hosted) {
            this.hosted = null
        }
        runCatching {
            if (hosted.resumed) {
                windowFocus(hosted.activity, false)
                lifecycle(hosted.activity, "onPause")
                hosted.resumed = false
            }
            if (hosted.started) {
                lifecycle(hosted.activity, "onStop")
                hosted.started = false
            }
            lifecycle(hosted.activity, "onDestroy")
        }.onFailure { ArmbatLog.w(TAG, "guest activity ${hosted.info.name}: $it") }
    }

    /**
     * An archive activity called finish().
     *
     * The guest activity is a child of the host activity (see createHosted), so
     * the platform hands that request here instead of ending the container's own
     * activity. It is queued: a finish() from onCreate must not run inside the
     * callback that made it.
     *
     * Returns false when the activity is not one of the archive's, so the host
     * can let the platform handle its own children.
     */
    fun finishGuestActivity(activity: Activity): Boolean {
        val hosted = session?.stack?.firstOrNull { it.activity === activity } ?: return false
        if (hosted.finished) {
            return true
        }
        hosted.finished = true
        // Activity.isFinishing reads this field, and the platform would have set
        // it: an archive that checks it after finish() has to see the truth.
        optional(hosted.activity, "mFinished", true)
        request(Action.Finish(hosted))
        return true
    }

    /**
     * Hosts an intent that names one of the archive's own activities.
     *
     * The archive is not installed, so the platform has no record of its
     * activities and an explicit intent naming one comes back as
     * ActivityNotFoundException. The class itself is already loaded from the
     * archive's dex in this process, so the activity is built here instead.
     *
     * Returns false for anything the guest does not own, including a class its
     * class loader did not define: that intent has to leave the container.
     */
    fun startGuestActivity(intent: Intent, requestCode: Int): Boolean {
        val session = session ?: return false
        val component = intent.component ?: return false
        val className = resolveGuestActivity(session, component.className) ?: return false
        ArmbatLog.i(TAG, "guest activity requested: $className")
        // The copy carries the resolved class and the archive's package: the
        // intent is handed to the guest through setIntent, so both halves have
        // to describe the package GuestPackageManager answers for.
        val forwarded = Intent(intent)
        forwarded.component = ComponentName(session.prepared.packageName, className)
        request(Action.Start(className, forwarded, requestCode))
        return true
    }

    /**
     * The class an intent names when that intent belongs to the archive.
     *
     * A guest normally names the archive's own package, but the container's
     * name still reaches an intent built from a path or an identity the
     * platform reported, so a class name in the container's namespace is
     * resolved to the archive's part after it as well.
     */
    private fun resolveGuestActivity(session: Session, name: String): String? {
        val archive = session.prepared.packageName
        val container = host.packageName
        val candidates = buildList {
            add(name)
            if (name.startsWith("$container.")) {
                add(archive + name.removePrefix(container))
            }
            if (name.startsWith(".")) {
                add(archive + name)
            }
        }
        for (candidate in candidates) {
            val type = runCatching {
                Class.forName(candidate, false, session.prepared.loader)
            }.getOrNull() ?: continue
            if (type.classLoader === session.prepared.loader &&
                Activity::class.java.isAssignableFrom(type)
            ) {
                return candidate
            }
        }
        return null
    }

    /**
     * Hands the result of a finished activity to the one that asked for it,
     * which is the step ActivityThread takes before it resumes the caller.
     */
    private fun deliverResult(caller: Hosted, finished: Hosted) {
        val requestCode = finished.requestCode
        if (requestCode < 0) {
            return
        }
        val code = resultCode(finished.activity)
        val data = resultData(finished.activity)
        runCatching {
            val method = Activity::class.java.getDeclaredMethod(
                "onActivityResult",
                Integer.TYPE,
                Integer.TYPE,
                Intent::class.java,
            )
            method.isAccessible = true
            method.invoke(caller.activity, requestCode, code, data)
        }.onFailure { ArmbatLog.w(TAG, "guest activity result: $it") }
    }

    /** The result an archive set before finishing, as the platform reads it. */
    private fun resultCode(activity: Activity): Int = runCatching {
        Activity::class.java.getDeclaredField("mResultCode").let {
            it.isAccessible = true
            it.getInt(activity)
        }
    }.getOrDefault(Activity.RESULT_CANCELED)

    private fun resultData(activity: Activity): Intent? = runCatching {
        Activity::class.java.getDeclaredField("mResultData").let {
            it.isAccessible = true
            it.get(activity) as Intent?
        }
    }.getOrNull()

    /**
     * The token of the platform activity the guest is drawn in.
     *
     * The guest activity is a child of that activity, so this is the token the
     * platform knows: it is what makes isTaskRoot() answer about the task the
     * container lives in instead of about a record that does not exist.
     */
    private val hostToken: IBinder? by lazy {
        runCatching {
            val method = Activity::class.java.getDeclaredMethod("getActivityToken")
            method.isAccessible = true
            method.invoke(host) as IBinder?
        }.getOrNull()
    }

    /**
     * Starts an archive. The native world is built on a worker thread and the
     * guest's window and lifecycle are then created on the main thread; the
     * callback reports null on success.
     */
    fun launch(apkPath: String, onResult: (String?) -> Unit) {
        stage(LaunchStage.PREPARE)
        Thread({
            var prepared: Prepared? = null
            var failure: String? = null
            try {
                prepared = prepare(apkPath)
            } catch (error: Throwable) {
                ArmbatLog.e(TAG, "guest launch failed", error)
                failure = "${error.javaClass.simpleName}: ${error.message ?: host.getString(R.string.error_no_message)}"
            }
            val ready = prepared
            host.runOnUiThread {
                // The activity may have gone away while the guest booted; putting
                // its view back at that point would resurrect a destroyed window.
                if (host.isDestroyed || host.isFinishing) {
                    return@runOnUiThread
                }
                if (ready == null) {
                    onResult(failure)
                    return@runOnUiThread
                }
                onResult(try {
                    start(ready)
                    null
                } catch (error: Throwable) {
                    ArmbatLog.e(TAG, "guest launch failed", error)
                    "${error.javaClass.simpleName}: ${error.message ?: host.getString(R.string.error_no_message)}"
                })
            }
        }, "armbat-launch").start()
    }

    private fun stage(value: LaunchStage) {
        ArmbatLog.i(TAG, "launch stage: $value")
        host.runOnUiThread {
            if (!host.isDestroyed && !host.isFinishing) {
                onStage?.invoke(value)
            }
        }
    }

    fun pause() {
        val hosted = hosted ?: return
        runCatching {
            windowFocus(hosted.activity, false)
            lifecycle(hosted.activity, "onPause")
        }
        hosted.resumed = false
    }

    /** Whether an archive is loaded and holding this process's window. */
    fun isRunning(): Boolean = session != null

    /**
     * The platform's back action, delivered to the archive's own activity.
     *
     * The guest has only ever seen the input the container forwards, and back
     * is no different -- except that it is not a view event at all. This
     * application targets a current SDK, so the platform turns the back
     * gesture into an invocation on the container's activity instead of a
     * [KeyEvent]; with nothing to answer it, the container ended, and the game
     * ended with it. The invocation is therefore turned back into the event a
     * build of this era was written against, a [KeyEvent.KEYCODE_BACK] that
     * goes down and up, and handed to the archive exactly as the platform
     * handed it to an activity of that era:
     *
     * - a game that reads the key itself answers from its own dispatchKeyEvent
     *   (Minecraft opens its pause screen there, on the key up);
     * - an activity overriding onKeyDown, or the platform's own Activity
     *   answering through onKeyUp into onBackPressed, is served by the key
     *   chain itself: the default ends that activity, which is the archive
     *   leaving the screen -- not the container closing under it.
     *
     * Returns false when no archive is on screen, so the container keeps its
     * own back behaviour.
     */
    fun pressBack(): Boolean {
        val hosted = hosted ?: return false
        val time = android.os.SystemClock.uptimeMillis()
        val handled = runCatching {
            // Both halves are always delivered: a game that acts on the key up
            // (Minecraft) never sees it otherwise.
            val down = hosted.activity.dispatchKeyEvent(backKey(time, KeyEvent.ACTION_DOWN))
            val up = hosted.activity.dispatchKeyEvent(backKey(time, KeyEvent.ACTION_UP))
            down || up
        }.onFailure { ArmbatLog.w(TAG, "back: ${hosted.info.name}: $it") }.getOrDefault(false)
        ArmbatLog.i(TAG, "back: ${hosted.info.name} handled=$handled")
        return true
    }

    /**
     * One half of a back press, shaped the way a device reports its own back
     * key: a keyboard source and the virtual device, no characters, no repeat.
     * An archive that inspects the event sees what it would see on a phone.
     */
    private fun backKey(time: Long, action: Int): KeyEvent = KeyEvent(
        time,
        time,
        action,
        KeyEvent.KEYCODE_BACK,
        0,
        0,
        KeyCharacterMap.VIRTUAL_KEYBOARD,
        0,
        0,
        InputDevice.SOURCE_KEYBOARD,
    )

    fun resume() {
        val hosted = hosted ?: return
        runCatching {
            lifecycle(hosted.activity, "onResume")
            windowFocus(hosted.activity, true)
        }
        hosted.resumed = true
    }

    /**
     * The window the guest draws into changed its shape.
     *
     * An archive that does not pin an orientation follows the phone -- osu!droid
     * asks for landscape from the onCreate of its menu activity -- and the
     * platform turns this activity with it. The guest's canvas was built for the
     * old shape, so the picture would be a small rectangle in the middle of the
     * window: the canvas is swapped for the new one and the archive is told
     * about the change, as it is told on the device.
     */
    fun onConfigurationChanged(config: Configuration) {
        val session = session ?: return
        val landscape = config.orientation == Configuration.ORIENTATION_LANDSCAPE
        if (landscape == session.virtualWidth > session.virtualHeight) {
            return
        }
        // Queued: the platform is still walking its own configuration change,
        // and the activities below are torn down and built again.
        val copy = Configuration(config)
        host.window.decorView.post { rotate(copy) }
    }

    /** Gives the guest the canvas of the window's new shape (see above). */
    private fun rotate(config: Configuration) {
        val session = session ?: return
        val landscape = config.orientation == Configuration.ORIENTATION_LANDSCAPE
        if (landscape == session.virtualWidth > session.virtualHeight) {
            return
        }
        val (width, height) = virtualSize(session.prepared.targetSdk, landscape)
        ArmbatLog.i(TAG, "guest screen rotated: ${width}x$height")
        val densityDpi = session.prepared.densityDpi
        val metrics = android.util.DisplayMetrics()
        metrics.setTo(session.prepared.resources.displayMetrics)
        metrics.widthPixels = width
        metrics.heightPixels = height
        // The configuration the archive resolves its resources against has to
        // describe the canvas, not the panel: an archive with a landscape
        // layout folder has to see the folder its screen now matches.
        val updated = Configuration(config)
        updated.densityDpi = densityDpi
        updated.screenWidthDp = width * 160 / densityDpi
        updated.screenHeightDp = height * 160 / densityDpi
        updated.smallestScreenWidthDp = min(updated.screenWidthDp, updated.screenHeightDp)
        runCatching { session.prepared.resources.updateConfiguration(updated, metrics) }
            .onFailure { ArmbatLog.w(TAG, "guest configuration: $it") }
        session.prepared.resources.displayMetrics.widthPixels = width
        session.prepared.resources.displayMetrics.heightPixels = height
        session.virtualWidth = width
        session.virtualHeight = height
        runCatching {
            session.screen.display.resize(width, height, densityDpi)
            session.screen.texture.setDefaultBufferSize(width, height)
        }
        // A renderer usually reads the surface once and follows it afterwards,
        // and an activity is told about the new configuration the way the
        // platform tells one that declares it handles the change: the window
        // hands it to the decor, and the view tree lays itself out again on the
        // canvas it now has. Tearing the activities down instead would rebuild
        // an engine in the middle of a session, which is the platform's answer
        // only for an activity that never asked to be kept.
        for (entry in session.stack) {
            entry.frame.resize(width, height)
            runCatching { entry.activity.onConfigurationChanged(config) }
                .onFailure { ArmbatLog.w(TAG, "guest configuration ${entry.info.name}: $it") }
        }
    }

    fun destroy() {
        // No guest, no requests: an activity an archive asks for while its
        // session is being torn down would otherwise be built on a released
        // display.
        pending.clear()
        session?.stack?.let { stack ->
            while (stack.isNotEmpty()) {
                destroyHosted(stack.removeLast())
            }
        }
        hosted = null
        val session = session
        runCatching { session?.screen?.display?.release() }
        runCatching { session?.screen?.surface?.release() }
        runCatching { session?.screen?.texture?.release() }
        this.session = null
        // The library is a list, and a list belongs to whatever way the user
        // holds the phone; the archive's orientation only applies while it runs.
        runCatching { host.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED }
        // No guest, no guest frame rate: the window goes back to the panel's.
        runCatching {
            val panel = host.window.attributes
            panel.preferredRefreshRate = 0f
            host.window.attributes = panel
        }
        GuestService.stop(host)
        endWorld()
    }

    /**
     * Ends the process together with the guest world it hosted.
     *
     * A guest leaves its JIT, its guest heap and its ARMv7 libraries mapped in
     * this process, ART will not map a library into a second class loader
     * ("already opened by ClassLoader ..., can't open in ClassLoader ..."), and
     * the native container has one world per process by construction. Keeping
     * the process alive would hand the next launch a dead world and a class
     * loader that cannot load a single library, which is exactly what happened
     * after the task was swiped away and the same archive was opened again. The
     * next launch therefore starts from a fresh process.
     */
    private fun endWorld() {
        if (!worldLoaded) {
            return
        }
        ArmbatLog.i(TAG, "guest world ends with the process")
        ArmbatLog.flush()
        android.os.Process.killProcess(android.os.Process.myPid())
    }

    /**
     * Activity's lifecycle methods are protected, and performCreate/performStart
     * do bookkeeping that assumes a real attach(). Calling the overrides directly
     * is enough for an old application and keeps the host's own lifecycle intact.
     */
    private fun lifecycle(activity: Activity, name: String, argument: Any? = null) {
        val method = if (name == "onCreate") {
            Activity::class.java.getDeclaredMethod(name, android.os.Bundle::class.java)
        } else {
            Activity::class.java.getDeclaredMethod(name)
        }
        method.isAccessible = true
        if (argument != null || name == "onCreate") {
            method.invoke(activity, argument)
        } else {
            method.invoke(activity)
        }
    }

    /**
     * The focus of the window, as the platform reports it to the activity.
     *
     * The guest's window is the host's window, and the guest's decor is only a
     * view inside it, so nothing would ever call this on the archive. An archive
     * may depend on it completely: Unity's activity forwards it to its player,
     * which is where the engine is told to resume rendering.
     */
    private fun windowFocus(activity: Activity, hasFocus: Boolean) {
        try {
            val method = Activity::class.java.getDeclaredMethod(
                "onWindowFocusChanged",
                java.lang.Boolean.TYPE,
            )
            method.isAccessible = true
            method.invoke(activity, hasFocus)
            ArmbatLog.i(TAG, "window focus: $hasFocus delivered")
        } catch (error: Throwable) {
            ArmbatLog.i(TAG, "window focus: $hasFocus failed: ${error.cause ?: error}")
        }
    }

    /**
     * The screen the application was written for.
     *
     * Games of the Android 2.x/4.x era were laid out for a 480x800 "normal"
     * phone or a 720p panel, and draw their interface in display pixels; later
     * ones expect a modern 1080p screen. The guest is given that size and the
     * picture is then scaled to the real display.
     *
     * In widescreen the short side keeps that size and the long one follows the
     * panel: these games scale their width and their height independently and
     * only lay out along the short side, so on a 19.5:9 phone the same
     * interface simply has more room, and the picture fills the display the way
     * it does when the application is installed on the device.
     */
    private fun virtualSize(targetSdk: Int, landscape: Boolean): Pair<Int, Int> {
        val (width, height) = when {
            targetSdk <= android.os.Build.VERSION_CODES.KITKAT -> 480 to 800
            targetSdk <= android.os.Build.VERSION_CODES.LOLLIPOP_MR1 -> 720 to 1280
            else -> 1080 to 1920
        }
        if (!GuestScreen.widescreen(host)) {
            return if (landscape) height to width else width to height
        }
        val panel = host.resources.displayMetrics
        val ratio = maxOf(panel.widthPixels, panel.heightPixels).toFloat() /
            minOf(panel.widthPixels, panel.heightPixels)
        val short = minOf(width, height)
        val long = (short * ratio).toInt()
        return if (landscape) long to short else short to long
    }

    /**
     * The density of a virtual screen.
     *
     * The guest lays its interface out in dp and the picture is scaled to the
     * panel afterwards, so the density belongs to the virtual screen, not to the
     * phone: 720p at xhdpi is the phone this era was written for, and the panel's
     * density on a 720p screen would draw every menu and dialog three times its
     * intended size. The bucket is the one nearest to that reference.
     */
    private fun virtualDensity(shortSide: Int): Int {
        val wanted = shortSide * 320 / 720
        return DENSITY_BUCKETS.minByOrNull { kotlin.math.abs(it - wanted) } ?: 320
    }

    /**
     * Whether the archive asked for a landscape window.
     *
     * The manifest's screenOrientation uses the same numbers as the platform's
     * requestedOrientation, so the container passes the value through unchanged;
     * this only answers the question the virtual screen has to know. An archive
     * that follows the device (unspecified, sensor, full sensor) gets the way
     * the phone is held: the display's rotation, not this activity's
     * configuration, which still describes the window the previous archive left
     * behind while the platform turns it back.
     */
    private fun wantsLandscape(screenOrientation: Int): Boolean = when (screenOrientation) {
        ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE,
        ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE,
        ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE,
        ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE,
        -> true

        ActivityInfo.SCREEN_ORIENTATION_PORTRAIT,
        ActivityInfo.SCREEN_ORIENTATION_REVERSE_PORTRAIT,
        ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT,
        ActivityInfo.SCREEN_ORIENTATION_USER_PORTRAIT,
        -> false

        else -> heldLandscape()
    }

    /** Whether the phone itself is held sideways, whatever the window still says. */
    private fun heldLandscape(): Boolean = runCatching {
        when (host.display?.rotation) {
            Surface.ROTATION_90, Surface.ROTATION_270 -> true
            Surface.ROTATION_0, Surface.ROTATION_180 -> false
            else -> null
        }
    }.getOrNull()
        ?: (host.resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE)

    private fun createVirtualDisplay(width: Int, height: Int, densityDpi: Int): VirtualScreen {
        val manager = host.getSystemService(DisplayManager::class.java)
        val texture = SurfaceTexture(0)
        texture.setDefaultBufferSize(width, height)
        val surface = Surface(texture)
        val display = manager.createVirtualDisplay(
            "armbat-guest",
            width,
            height,
            densityDpi,
            surface,
            0,
        )
        return VirtualScreen(display, surface, texture)
    }

    /**
     * The permissions a guest may need before it starts.
     *
     * The archive's manifest is the declaration, and the container's own is
     * added to it: the guest asks the platform about its own package by the
     * container's name (the archive is not installed), so a permission only the
     * container declares would look like one of the guest's that it does not
     * hold.
     */
    fun declaredPermissions(apkPath: String): Set<String> {
        @Suppress("DEPRECATION")
        val archive = runCatching {
            host.packageManager.getPackageArchiveInfo(apkPath, PackageManager.GET_PERMISSIONS)
                ?.requestedPermissions
        }.getOrNull()
        @Suppress("DEPRECATION")
        val own = runCatching {
            host.packageManager.getPackageInfo(host.packageName, PackageManager.GET_PERMISSIONS)
                ?.requestedPermissions
        }.getOrNull()
        return buildSet {
            archive?.let { addAll(it) }
            own?.let { addAll(it) }
        }
    }

    /**
     * The archive's permissions that this package can actually be granted.
     *
     * The platform only grants a permission a package declares, and the archive
     * is not installed: the container's own manifest is what carries its
     * permissions. A request that mixes names the container declares with names
     * it does not comes back all-denied in one shot, because the framework
     * refuses the unknown ones before the dialog is drawn -- so an archive that
     * declares, say, camera access the container never asked for would have its
     * phone-state request silently refused as well, and the user would see a
     * dialog that closes by itself. Only the intersection is requested; the rest
     * stays denied, exactly as it would for a package that never declared it,
     * and the guest's own checks then see the platform's real answer.
     */
    fun requestablePermissions(apkPath: String): Set<String> {
        @Suppress("DEPRECATION")
        val archive = runCatching {
            host.packageManager.getPackageArchiveInfo(apkPath, PackageManager.GET_PERMISSIONS)
                ?.requestedPermissions?.toSet().orEmpty()
        }.getOrDefault(emptySet())
        @Suppress("DEPRECATION")
        val carried = runCatching {
            host.packageManager.getPackageInfo(host.packageName, PackageManager.GET_PERMISSIONS)
                ?.requestedPermissions?.toSet().orEmpty()
        }.getOrDefault(emptySet())
        val requestable = (archive intersect carried) - satisfiedWithoutAsking()
        val notCarried = archive - carried
        if (notCarried.isNotEmpty()) {
            // The archive may rely on a permission the container does not
            // declare. The refusal is otherwise silent, so say it once.
            ArmbatLog.i(TAG, "permissions not declared by the container: $notCarried")
        }
        return requestable
    }

    /**
     * The permissions that cannot be granted any more but that the guest already
     * sees as held.
     *
     * A current platform refuses READ and WRITE_EXTERNAL_STORAGE by target SDK,
     * so asking for them opens a dialog that closes itself with "denied" and the
     * launch then shows an error for a permission the guest never lacked. The
     * guest is answered from all files access instead (see GuestContext), so the
     * request is only noise and is skipped while that access is held.
     */
    private fun satisfiedWithoutAsking(): Set<String> =
        if (GuestStorage.isAvailable()) LEGACY_STORAGE else emptySet()

    private fun launchableActivity(apkPath: String): android.content.pm.ActivityInfo? {
        @Suppress("DEPRECATION")
        val archive = host.packageManager.getPackageArchiveInfo(
            apkPath,
            PackageManager.GET_ACTIVITIES or PackageManager.GET_META_DATA,
        ) ?: return null
        val activities = archive.activities ?: return null
        for (activity in activities) {
            ArmbatLog.i(TAG, "activity in manifest: ${activity.name}")
        }
        // The entry point an icon starts, not the first component the manifest
        // lists: an archive may declare a helper before it (a store sign-in
        // wrapper, a licence check), and that helper then asks the platform for
        // a service the guest does not have -- a dialog where the application
        // should be. PackageManager drops intent filters, so the manifest is
        // read from the archive itself.
        val entry = runCatching {
            ZipFile(apkPath).use { zip ->
                val manifest = zip.getEntry(ApkManifest.FILE)
                if (manifest == null) {
                    null
                } else {
                    zip.getInputStream(manifest).use { ApkManifest.launcherActivity(it.readBytes()) }
                }
            }
        }.getOrNull()
        return activities.firstOrNull { it.name == entry } ?: activities.firstOrNull()
    }

    /**
     * The directory the guest's class loader resolves native library names in.
     *
     * An archive that only ships armeabi-v7a has no arm64 file for
     * System.loadLibrary to find, and the runtime refuses to continue when the
     * name is missing. The container puts its empty stand-in there under every
     * name the archive uses; the code that name refers to runs in the guest
     * world instead.
     *
     * The directory lives in the container's own cache, never in the data tree
     * the guest reads: an archive that keeps its own copies of the libraries
     * under files/ would otherwise find 64-bit stand-ins in place of the armv7
     * files it wrote, and the guest's loader cannot map those.
     */
    private fun nativeLibraries(apkPath: String): String? {
        // Android picks one ABI directory per application: armeabi-v7a when the
        // archive has one, armeabi otherwise. An archive built before the v7a
        // ABI existed still runs here, and System.loadLibrary has to find a
        // stand-in for it like any other.
        val byAbi = LinkedHashMap<String, MutableList<String>>()
        ZipFile(apkPath).use { zip ->
            val entries = zip.entries()
            while (entries.hasMoreElements()) {
                val entry = entries.nextElement().name
                val abi = when {
                    entry.startsWith("lib/armeabi-v7a/") -> "lib/armeabi-v7a/"
                    entry.startsWith("lib/armeabi/") -> "lib/armeabi/"
                    else -> continue
                }
                if (entry.endsWith(".so")) {
                    byAbi.getOrPut(abi) { mutableListOf() } += entry.substringAfterLast('/')
                }
            }
        }
        val names = byAbi["lib/armeabi-v7a/"] ?: byAbi["lib/armeabi/"] ?: return null
        if (names.isEmpty()) {
            return null
        }

        // The container's own APK stores native libraries uncompressed and does
        // not extract them to nativeLibraryDir, so the stand-in is read out of
        // the archive itself.
        val stub = ZipFile(host.applicationInfo.sourceDir).use { zip ->
            val entry = zip.getEntry("lib/arm64-v8a/${STUB_LIBRARY}") ?: return null
            zip.getInputStream(entry).use { it.readBytes() }
        }

        val directory = File(
            host.codeCacheDir,
            "native-stubs/${File(apkPath).nameWithoutExtension}",
        )
        directory.deleteRecursively()
        directory.mkdirs()
        for (name in names) {
            File(directory, name).writeBytes(stub)
        }
        // Earlier builds wrote the same stand-ins under files/, where an archive
        // that keeps its own copy of the libraries finds them first. Remove that
        // directory only while it still holds nothing but the stand-ins.
        val legacy = File(host.filesDir, "native/${File(apkPath).nameWithoutExtension}")
        val leftovers = legacy.listFiles()
        if (leftovers != null && leftovers.isNotEmpty() &&
            leftovers.all { it.isFile && it.length().toInt() == stub.size &&
                it.readBytes().contentEquals(stub) }
        ) {
            legacy.deleteRecursively()
        }
        return directory.absolutePath
    }

    private fun attachBaseContext(activity: ContextWrapper, context: Context) {
        val method = ContextWrapper::class.java.getDeclaredMethod("attachBaseContext", Context::class.java)
        method.isAccessible = true
        method.invoke(activity, context)
    }

    /**
     * The Application a guest process starts with, made from the manifest's
     * <application android:name> exactly as the platform makes it.
     *
     * An archive that names none still gets one: that is what a device
     * installs, and it is the framework's own class attached to the archive's
     * base context. Falling back to the container's Application instead handed
     * the guest a context whose services are the container's -- HockeyApp asks
     * getApplication() for the phone service, receives the platform's
     * TelephonyManager, and the archive's cast to the guest class of that name
     * fails before its activity has started.
     */
    private fun guestApplication(prepared: Prepared, context: Context): Application {
        val className = prepared.archiveInfo?.className
        val application = if (className.isNullOrEmpty()) {
            Application()
        } else {
            runCatching {
                val type = Class.forName(className, true, prepared.loader)
                check(Application::class.java.isAssignableFrom(type)) {
                    "$className is not an Application"
                }
                val created = type.getDeclaredConstructor().newInstance() as Application
                ArmbatLog.i(TAG, "application: $className")
                created
            }.getOrElse {
                ArmbatLog.w(TAG, "application $className: $it")
                Application()
            }
        }
        // What Application.attach does before onCreate: the base context is
        // where getPackageName, getResources and getContentResolver answer from.
        attachBaseContext(application, context)
        application.onCreate()
        return application
    }

    /**
     * Activity.onCreate dispatches to the fragment manager, and the fragment
     * manager refuses to run until attachHost has been called: that happens in
     * Activity.attach, which is not used here.
     */
    private fun attachFragments(activity: Activity) {
        val controller = Activity::class.java.getDeclaredField("mFragments").run {
            isAccessible = true
            get(activity)
        } ?: return
        val method = controller.javaClass.methods.firstOrNull { it.name == "attachHost" } ?: return
        method.isAccessible = true
        method.invoke(controller, null)
    }

    private fun activityThread(): Any? = runCatching {
        val type = Class.forName("android.app.ActivityThread")
        val method = type.getDeclaredMethod("currentActivityThread")
        method.isAccessible = true
        method.invoke(null)
    }.getOrNull()

    private fun newWindow(context: Context): Window {
        runCatching {
            val type = Class.forName("com.android.internal.policy.PhoneWindow")
            val constructor = type.getDeclaredConstructor(Context::class.java)
            constructor.isAccessible = true
            return constructor.newInstance(context) as Window
        }
        val policy = Class.forName("com.android.internal.policy.PolicyManager")
        val method = policy.getDeclaredMethod("makeNewWindow", Context::class.java)
        method.isAccessible = true
        return method.invoke(null, context) as Window
    }

    private fun field(target: Any, name: String, value: Any?) {
        var type: Class<*>? = target.javaClass
        while (type != null) {
            try {
                val field = type.getDeclaredField(name)
                field.isAccessible = true
                field.set(target, value)
                return
            } catch (missing: NoSuchFieldException) {
                type = type.superclass
            }
        }
        throw NoSuchFieldException("${target.javaClass.simpleName}.$name")
    }

    private fun optional(target: Any, name: String, value: Any?) {
        runCatching { field(target, name, value) }
    }

    private companion object {
        const val TAG = "Armbat"
        const val NATIVE_ACTIVITY = "android.app.NativeActivity"
        const val TELEPHONY_MANAGER = "android.telephony.TelephonyManager"
        const val STUB_LIBRARY = "libguestsdk.so"
        // Where the guest's own linker found the archive's libraries. An
        // application that dlopens from ApplicationInfo.nativeLibraryDir has to
        // land on the copies the container already loaded, not on the 64-bit
        // stand-ins the host class loader needs.
        const val GUEST_LIBRARY_DIR = "/armbat/app-libs"
        // The frame rate the guest's era assumes. Every archive the container
        // can run is 32-bit, so none of them was written for a faster panel: the
        // engine advances its simulation once per presented frame and counts on
        // sixty of them a second.
        const val GUEST_FRAME_RATE = 60f
        // The storage permissions of the Android 4 era. The platform no longer
        // grants them to any application, whatever the user answers, while the
        // container itself runs from all files access.
        val LEGACY_STORAGE = setOf(
            "android.permission.READ_EXTERNAL_STORAGE",
            "android.permission.WRITE_EXTERNAL_STORAGE",
        )
        // The densities the platform recognises. A virtual screen takes the one
        // nearest its reference, so a guest sees a real device's configuration.
        val DENSITY_BUCKETS = intArrayOf(120, 160, 240, 320, 480, 640)
    }
}
