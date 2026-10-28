package dev.armbat

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.res.Configuration
import android.net.Uri
import android.os.Bundle
import android.view.KeyEvent
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.activity.SystemBarStyle
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBars
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * The launcher: imported archives on a page grid, a dock, and the three soft
 * keys of the platform the reference comes from.
 *
 * A tap starts an application, a long press opens its details, the dock holds
 * the container's own actions, and Settings carries the language choice.
 */
class MainActivity : ComponentActivity() {

    private data class Notice(val title: String, val text: String? = null)

    private sealed interface Screen {
        data object Home : Screen
        data class Details(val app: ApkInfo) : Screen
        data object Settings : Screen
        data object About : Screen
        data object Apps : Screen
    }

    private lateinit var guest: GuestLauncher
    private lateinit var library: Library

    /** A back key of the guest's that this activity took and must finish. */
    private var backKeyDelivered = false

    /** An archive handed over by another application, consumed by [Shell]. */
    private var pendingImport by mutableStateOf<Uri?>(null)

    /** Where the boot screen is while [GuestLauncher] prepares an archive. */
    private var launchStage by mutableStateOf(LaunchStage.PREPARE)

    /**
     * Whether the shared tree the guests live in can be read. The permission is
     * granted on a system screen outside this activity, so the answer is read
     * again on every resume rather than once at start.
     */
    private var storageReady by mutableStateOf(false)

    override fun attachBaseContext(newBase: Context) {
        super.attachBaseContext(AppLocale.wrap(newBase))
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        guest = GuestLauncher(this)
        guest.onFinished = { showLibrary() }
        guest.onStage = { stage -> launchStage = stage }
        // Back belongs to the archive while it runs: it is the game's own pause
        // screen or its own way out, not the container's. The platform turns
        // the gesture into a back invocation on this activity (the application
        // targets a current SDK, so it no longer arrives as a key event), and
        // that invocation is handed to the guest; a guest with nothing to do
        // about it ends itself, and the launcher comes back. The callback is
        // added before the shell is composed, so every screen of the shell
        // keeps priority over it.
        onBackPressedDispatcher.addCallback(
            this,
            object : OnBackPressedCallback(true) {
                override fun handleOnBackPressed() {
                    if (guest.pressBack()) {
                        return
                    }
                    // No archive on screen: behave exactly as before, with the
                    // shell's own handlers and then the platform's default.
                    isEnabled = false
                    onBackPressedDispatcher.onBackPressed()
                    isEnabled = true
                }
            },
        )
        library = Library(this)
        storageReady = GuestStorage.isAvailable()
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(android.graphics.Color.BLACK),
            navigationBarStyle = SystemBarStyle.dark(android.graphics.Color.BLACK),
        )
        // Only on a fresh start: after a recreation the view intent is still
        // attached, and importing the same archive twice is exactly what the
        // library refuses.
        if (savedInstanceState == null) {
            pendingImport = viewArchive(intent)
        }
        setContent { Shell() }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        pendingImport = viewArchive(intent)
    }

    /** The URI of an APK handed over by a file manager, or null for a normal start. */
    private fun viewArchive(intent: Intent?): Uri? {
        when (intent?.action) {
            Intent.ACTION_VIEW, Intent.ACTION_INSTALL_PACKAGE -> Unit
            else -> return null
        }
        // Most managers put the archive in the data URI, a few older ones in the
        // stream extra of the same intent.
        @Suppress("DEPRECATION")
        return intent.data ?: intent.getParcelableExtra(Intent.EXTRA_STREAM)
    }

    override fun onResume() {
        super.onResume()
        storageReady = GuestStorage.isAvailable()
        guest.resume()
        keepScreenOn()
        hideSystemNav()
    }

    /**
     * A guest that draws its own frames expects the display to stay awake while
     * it runs, but it asks through its own window, and that window is a window
     * inside this one: the flag never reaches the real display. A display
     * timeout would pause the guest mid-load, so the container holds the flag
     * for as long as a guest is running here.
     */
    private fun keepScreenOn() {
        if (guest.isRunning()) {
            window.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
    }

    /**
     * The launcher draws its own navigation bar, so the platform's has to stay
     * out of the way; it comes back with a swipe from the edge, as expected.
     */
    private fun hideSystemNav() {
        val controller = WindowCompat.getInsetsController(window, window.decorView)
        controller.systemBarsBehavior =
            WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        controller.hide(WindowInsetsCompat.Type.navigationBars())
    }

    override fun onPause() {
        guest.pause()
        super.onPause()
    }

    /**
     * The window was rotated. The guest's canvas is the shape of the window it
     * draws into, so it has to follow; the shell's own layout follows on its
     * own.
     */
    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        guest.onConfigurationChanged(newConfig)
    }

    /**
     * A back key from a keyboard or a remote reaches the activity this way
     * rather than through the dispatcher. It is the archive's back all the
     * same, so it is offered to the guest first and never treated as the
     * container's own; the key up of a press the guest already got is
     * swallowed with it, or the platform's key tracking would turn the same
     * press into a second back.
     */
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.keyCode == KeyEvent.KEYCODE_BACK) {
            if (event.action == KeyEvent.ACTION_DOWN) {
                backKeyDelivered = guest.pressBack()
                if (backKeyDelivered) {
                    return true
                }
            } else if (backKeyDelivered) {
                backKeyDelivered = false
                return true
            }
        }
        return super.dispatchKeyEvent(event)
    }

    override fun onDestroy() {
        guest.destroy()
        super.onDestroy()
    }

    /**
     * An archive activity is a child of this activity (see GuestLauncher), so
     * the platform hands its finish() and its startActivityForResult() here.
     * Both belong to the guest: the container ends the archive's activity and
     * resumes the one under it, and a request for another of its activities
     * stays inside the session. Anything else is the platform's business, as
     * before.
     */
    @Suppress("DEPRECATION")
    override fun finishFromChild(child: Activity) {
        if (guest.finishGuestActivity(child)) {
            return
        }
        super.finishFromChild(child)
    }

    @Suppress("DEPRECATION")
    override fun startActivityFromChild(child: Activity, intent: Intent, requestCode: Int) {
        if (guest.startGuestActivity(intent, requestCode)) {
            return
        }
        super.startActivityFromChild(child, intent, requestCode)
    }

    @Suppress("DEPRECATION")
    override fun startActivityFromChild(
        child: Activity,
        intent: Intent,
        requestCode: Int,
        options: Bundle?,
    ) {
        if (guest.startGuestActivity(intent, requestCode)) {
            return
        }
        super.startActivityFromChild(child, intent, requestCode, options)
    }

    /**
     * The archive has no activity left: its session is over, so the launcher
     * comes back instead of leaving the last frame of a finished game on
     * screen. The dev hook's extra is dropped first, or the shell would start
     * the same archive again the moment it is composed.
     */
    private fun showLibrary() {
        intent?.removeExtra("launch")
        setContent { Shell() }
    }

    @Composable
    private fun Shell() {
        val context = LocalContext.current
        val scope = rememberCoroutineScope()
        var apps by remember { mutableStateOf<List<ApkInfo>>(emptyList()) }
        var screen by remember { mutableStateOf<Screen>(Screen.Home) }
        var query by remember { mutableStateOf("") }
        var busy by remember { mutableStateOf(false) }
        var running by remember { mutableStateOf<String?>(null) }
        var notice by remember { mutableStateOf<Notice?>(null) }
        var launchError by remember { mutableStateOf("") }
        var waitingForPermission by remember { mutableStateOf<ApkInfo?>(null) }
        var localeTag by remember { mutableStateOf(AppLocale.current(context)) }
        var logLevel by remember { mutableStateOf(ArmbatLog.level()) }
        var widescreen by remember { mutableStateOf(GuestScreen.widescreen(context)) }
        val version = remember(context) {
            runCatching {
                context.packageManager.getPackageInfo(context.packageName, 0).versionName
            }.getOrNull().orEmpty()
        }

        suspend fun refresh() {
            apps = withContext(Dispatchers.IO) { library.read() }
            ArmbatLog.i("Armbat", "library: ${apps.size} apk")
        }

        lateinit var permission: androidx.activity.result.ActivityResultLauncher<Array<String>>

        /**
         * The runtime permissions the archive declares that the platform has
         * not granted yet.
         *
         * The guest cannot ask for one itself: the dialog belongs to a real
         * activity and the answer is delivered to a real ActivityThread, so a
         * guest that waits for it waits forever. The container asks here,
         * before the guest starts, and the guest then reads the platform's
         * answer through its own checks.
         */
        fun missingPermissions(apkPath: String): Array<String> =
            guest.requestablePermissions(apkPath)
                .filter { name ->
                    runCatching {
                        this@MainActivity.packageManager.getPermissionInfo(name, 0)
                            .protectionLevel and
                            android.content.pm.PermissionInfo.PROTECTION_DANGEROUS != 0
                    }.getOrDefault(false)
                }
                .filter {
                    androidx.core.content.ContextCompat.checkSelfPermission(
                        this@MainActivity,
                        it,
                    ) != android.content.pm.PackageManager.PERMISSION_GRANTED
                }
                .toTypedArray()

        fun startGuest(app: ApkInfo) {
            // Preparing an archive rewrites the extracted dex and rebinds its
            // JNI entry points in the shared native world, so a second prepare
            // does not just waste a launch: it leaves the first guest's native
            // methods unresolved. The dev hook and the launcher can both reach
            // here for the same start (a recomposition restarts the effect), and
            // a tap during a start is the same story.
            if (running != null) {
                return
            }
            launchError = ""
            launchStage = LaunchStage.PREPARE
            running = app.path
            // The native world is built on a worker thread: booting the guest
            // userland and mapping the archive's libraries takes seconds, and
            // this thread is the one the platform watches.
            guest.launch(app.path) { error ->
                running = null
                launchError = error ?: ""
                keepScreenOn()
            }
        }

        /**
         * The shared tree is the only place a guest can work in, so nothing is
         * imported or started until the platform grants access to it.
         */
        fun requireStorage(): Boolean {
            if (GuestStorage.isAvailable()) {
                return true
            }
            notice = Notice(
                title = context.getString(R.string.storage_access_title),
                text = context.getString(R.string.storage_access_detail),
            )
            GuestStorage.request(this@MainActivity)
            return false
        }

        fun run(app: ApkInfo) {
            if (!requireStorage()) {
                return
            }
            val missing = missingPermissions(app.path)
            if (missing.isEmpty()) {
                startGuest(app)
            } else {
                waitingForPermission = app
                permission.launch(missing)
            }
        }

        permission = rememberLauncherForActivityResult(
            ActivityResultContracts.RequestMultiplePermissions(),
        ) { granted ->
            val app = waitingForPermission ?: return@rememberLauncherForActivityResult
            waitingForPermission = null
            // The guest starts whether or not every answer was "allow": the
            // platform's answer is the truth the guest reads through its own
            // checks, and an application that needs a permission it did not get
            // says so in its own words. Refusing to start here turned one
            // denied optional permission into an application that never runs.
            val denied = granted.filterValues { !it }.keys
            if (denied.isNotEmpty()) {
                ArmbatLog.i("Armbat", "permissions denied: $denied")
            }
            startGuest(app)
        }

        LaunchedEffect(storageReady) {
            if (!storageReady) {
                // Asked once the shell exists, so the answer comes back to a
                // resumed activity rather than to a window still being built.
                GuestStorage.request(this@MainActivity)
                return@LaunchedEffect
            }
            refresh()
            // Dev hook: `am start -e launch <package>` starts an application
            // without a tap, which is how the launcher is checked over adb.
            val target = intent?.getStringExtra("launch") ?: return@LaunchedEffect
            val app = apps.firstOrNull { it.packageName == target } ?: return@LaunchedEffect
            run(app)
        }

        // One import path for both the file picker and an APK opened from
        // outside the container, so a tap on an archive behaves the same way
        // wherever it happens.
        suspend fun importArchive(uri: android.net.Uri) {
            if (!requireStorage()) {
                return
            }
            busy = true
            val result = withContext(Dispatchers.IO) { library.import(uri) }
            busy = false
            when (result) {
                is Library.Import.Added -> {
                    screen = Screen.Home
                    refresh()
                }

                is Library.Import.Duplicate -> notice = Notice(
                    title = context.getString(R.string.error_already_added),
                    text = context.getString(
                        R.string.error_already_added_detail,
                        result.app.label,
                    ),
                )

                Library.Import.Unreadable -> notice = Notice(
                    title = context.getString(R.string.error_read_apk),
                )
            }
        }

        val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            if (uri == null) return@rememberLauncherForActivityResult
            scope.launch { importArchive(uri) }
        }

        // An archive opened from a file manager arrives as a VIEW intent with a
        // read grant for its content URI; the activity hands it over once and
        // clears it so a recreation does not import the same file again. The
        // handover is cleared only after the import finishes: clearing it first
        // would change this effect's key and cancel the copy in flight.
        val incoming = pendingImport
        LaunchedEffect(incoming) {
            val uri = incoming ?: return@LaunchedEffect
            importArchive(uri)
            pendingImport = null
        }

        val filtering = query.isNotBlank()
        val visible = remember(apps, query) {
            if (!filtering) {
                apps
            } else {
                apps.filter {
                    it.label.contains(query, ignoreCase = true) ||
                        it.packageName.contains(query, ignoreCase = true)
                }
            }
        }

        BackHandler(enabled = screen != Screen.Home) { screen = Screen.Home }
        BackHandler(enabled = notice != null) { notice = null }

        Box(Modifier.fillMaxSize().background(HoloBlack)) {
            Column(
                Modifier
                    .fillMaxSize()
                    .padding(top = WindowInsets.statusBars.asPaddingValues().calculateTopPadding()),
            ) {
                Box(Modifier.weight(1f)) {
                    when (val current = screen) {
                        Screen.Home -> HomeScreen(
                            apps = visible,
                            filtering = filtering,
                            query = query,
                            onQueryChange = { query = it },
                            running = running,
                            busy = busy,
                            message = launchError,
                            onLaunch = { run(it) },
                            onDetails = { screen = Screen.Details(it) },
                            onAddApk = { picker.launch(arrayOf("*/*")) },
                            onSettings = { screen = Screen.Settings },
                            onApps = { screen = Screen.Apps },
                        )

                        is Screen.Details -> DetailsScreen(
                            app = current.app,
                            launching = running == current.app.path,
                            launchError = launchError,
                            onLaunch = { run(current.app) },
                            onDelete = {
                                scope.launch {
                                    busy = true
                                    withContext(Dispatchers.IO) { library.delete(current.app) }
                                    busy = false
                                    screen = Screen.Home
                                    refresh()
                                }
                            },
                        )

                        Screen.Settings -> SettingsScreen(
                            localeTag = localeTag,
                            logLevel = logLevel,
                            widescreen = widescreen,
                            version = version,
                            onLocale = { tag ->
                                localeTag = tag
                                AppLocale.set(this@MainActivity, tag)
                            },
                            onLogLevel = { level ->
                                logLevel = level
                                ArmbatLog.setLevel(level)
                            },
                            onWidescreen = { on ->
                                widescreen = on
                                GuestScreen.setWidescreen(this@MainActivity, on)
                            },
                            onAbout = { screen = Screen.About },
                        )

                        Screen.About -> AboutScreen(
                            appCount = apps.size,
                            version = version,
                        )

                        Screen.Apps -> AppsScreen(
                            apps = apps,
                            running = running,
                            onLaunch = { run(it) },
                            onDetails = { screen = Screen.Details(it) },
                        )
                    }

                    notice?.let { current ->
                        MessageDialog(
                            title = current.title,
                            text = current.text,
                            onDismiss = { notice = null },
                        )
                    }
                }

                NavBar(
                    onBack = {
                        when (screen) {
                            Screen.Home -> finish()
                            else -> screen = Screen.Home
                        }
                    },
                    onHome = { screen = Screen.Home },
                    onRecents = { screen = Screen.Apps },
                )
            }

            if (running != null) {
                LaunchScreen(
                    app = apps.firstOrNull { it.path == running },
                    stage = launchStage,
                )
            }
        }
    }
}
