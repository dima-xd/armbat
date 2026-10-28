package dev.armbat

import android.content.Context
import android.content.res.AssetManager
import android.content.res.Configuration
import android.content.res.Resources
import android.graphics.Bitmap
import android.graphics.Canvas
import android.util.DisplayMetrics
import java.io.File
import java.io.FileOutputStream

/**
 * Resources of an APK that is not installed.
 *
 * The package manager only knows about installed packages, so an archive's
 * label, theme and layouts have to be read through an AssetManager with the APK
 * added as an asset path. Both the AssetManager constructor and addAssetPath are
 * hidden APIs; they work here because the container is debuggable.
 */
object ApkResources {
    private const val ICON_PIXELS = 256

    // What an installed application's resource table is an overlay of. Without
    // it the archive's own resources are all there is: every android:* style,
    // attribute and drawable an application refers to -- and every platform
    // widget inflates itself with -- is missing, so even the default theme
    // resolves to nothing and the first View constructor throws.
    private const val FRAMEWORK_RESOURCES = "/system/framework/framework-res.apk"

    fun of(context: Context, path: String, screen: Screen? = null): Resources? = runCatching {
        val constructor = AssetManager::class.java.getDeclaredConstructor()
        constructor.isAccessible = true
        val assets = constructor.newInstance() as AssetManager
        val addAssetPath = AssetManager::class.java
            .getDeclaredMethod("addAssetPath", String::class.java)
        addAssetPath.isAccessible = true
        // The framework goes in first: the archive's own resources are an
        // overlay on top of it, and a reference the archive does not define
        // has to fall through to the platform's copy.
        addAssetPath.invoke(assets, FRAMEWORK_RESOURCES)
        val cookie = addAssetPath.invoke(assets, path) as Int
        if (cookie == 0) {
            return@runCatching null
        }
        // The archive is told how big its screen is, and it answers from these
        // metrics: an engine there reads the display size to map a touch into
        // its own canvas and to lay its menus out in dp. The panel's metrics
        // answer for a screen the guest never draws on -- 2772x1272 at the
        // panel's density -- so a 1280x720 surface got touches scaled for a
        // canvas nearly twice as wide, and every touch landed short of its
        // target.
        val metrics = DisplayMetrics()
        val configuration = Configuration(context.resources.configuration)
        if (screen == null) {
            metrics.setTo(context.resources.displayMetrics)
        } else {
            val fontScale =
                context.resources.displayMetrics.scaledDensity /
                    context.resources.displayMetrics.density
            metrics.setTo(context.resources.displayMetrics)
            metrics.widthPixels = screen.width
            metrics.heightPixels = screen.height
            metrics.densityDpi = screen.densityDpi
            metrics.density = screen.densityDpi / 160f
            metrics.scaledDensity = metrics.density * fontScale
            metrics.xdpi = screen.densityDpi.toFloat()
            metrics.ydpi = screen.densityDpi.toFloat()
            configuration.densityDpi = screen.densityDpi
            configuration.screenWidthDp = screen.width * 160 / screen.densityDpi
            configuration.screenHeightDp = screen.height * 160 / screen.densityDpi
            configuration.smallestScreenWidthDp = minOf(
                configuration.screenWidthDp,
                configuration.screenHeightDp,
            )
            configuration.orientation = if (screen.width > screen.height) {
                Configuration.ORIENTATION_LANDSCAPE
            } else {
                Configuration.ORIENTATION_PORTRAIT
            }
            val shortDp = minOf(configuration.screenWidthDp, configuration.screenHeightDp)
            val sizeClass = when {
                shortDp >= 720 -> Configuration.SCREENLAYOUT_SIZE_XLARGE
                shortDp >= 480 -> Configuration.SCREENLAYOUT_SIZE_LARGE
                shortDp >= 320 -> Configuration.SCREENLAYOUT_SIZE_NORMAL
                else -> Configuration.SCREENLAYOUT_SIZE_SMALL
            }
            configuration.screenLayout =
                (configuration.screenLayout and Configuration.SCREENLAYOUT_SIZE_MASK.inv()) or
                sizeClass
        }
        @Suppress("DEPRECATION")
        val resources = Resources(assets, metrics, configuration)
        if (screen != null) {
            // The constructor builds the table against the default display's
            // adjustment: it keeps the density written above but replaces the
            // pixel size with the panel's, so the archive laid itself out for
            // 1272x2772 while drawing on a 480x800 surface -- every screen and
            // sprite of an old game came out stretched and cut at the bottom.
            // getDisplayMetrics() is the live object the archive reads, so the
            // virtual size is written back once the table exists.
            resources.displayMetrics.widthPixels = screen.width
            resources.displayMetrics.heightPixels = screen.height
        }
        resources
    }.getOrNull()

    /** The virtual screen a guest draws into, in its own pixels and density. */
    data class Screen(val width: Int, val height: Int, val densityDpi: Int)

    /**
     * Draws an archive's launcher icon into a cached PNG and returns the file.
     *
     * The icon is a resource id, and no installed package exists to resolve it,
     * so it is decoded through the archive's own Resources and flattened onto a
     * bitmap: adaptive icons (foreground + background layers, v26+) draw the same
     * way the launcher draws them. The result is cached by package and version,
     * because a library refresh reads every archive and only the icon read is
     * expensive. Missing or undecodable icons return null and the caller falls
     * back to a letter.
     */
    fun icon(context: Context, apkPath: String, iconRes: Int, key: String): File? {
        if (iconRes == 0) return null
        val cacheDir = File(context.filesDir, "icons")
        val cached = File(cacheDir, key)
        if (cached.isFile && cached.length() > 0) return cached

        val drawable = runCatching {
            of(context, apkPath)?.getDrawable(iconRes, null)
        }.getOrNull() ?: return null

        return runCatching {
            cacheDir.mkdirs()
            val bitmap = Bitmap.createBitmap(ICON_PIXELS, ICON_PIXELS, Bitmap.Config.ARGB_8888)
            val canvas = Canvas(bitmap)
            drawable.setBounds(0, 0, ICON_PIXELS, ICON_PIXELS)
            drawable.draw(canvas)

            val staging = File(cacheDir, "$key.tmp")
            FileOutputStream(staging).use { out ->
                bitmap.compress(Bitmap.CompressFormat.PNG, 100, out)
            }
            bitmap.recycle()
            if (!staging.renameTo(cached)) {
                staging.copyTo(cached, overwrite = true)
                staging.delete()
            }
            cached
        }.getOrNull()
    }
}
