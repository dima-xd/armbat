package dev.armbat

import android.content.Context
import android.hardware.display.DisplayManager
import android.view.Display
import kotlin.math.max
import kotlin.math.min

/**
 * The size and the refresh rate a guest is given.
 *
 * The default is the screen the archive was written for: an old game lays its
 * interface out in the pixels of a 480x800 phone, its layout has no room for a
 * 19.5:9 panel, and the picture is scaled to the panel with bars where the
 * aspect differs. Widescreen gives the guest the panel's aspect on its long
 * side instead -- the short side keeps the old pixel size -- so the layout has
 * the space it has on a modern phone and the picture fills the display, which
 * is what the same application does when installed on the device.
 *
 * On top of that the user may ask for a concrete size and rate: an engine that
 * renders into its window draws more detail on a bigger canvas, and one that
 * paces itself after the display follows the rate it is given. Both are
 * settings of the container, never a per-application branch in the emulator,
 * and an entry for one archive overrides the global one.
 *
 * A choice here takes effect on the next launch: the guest's resources and its
 * display are built once, before its own code runs.
 */
object GuestScreen {
    private const val PREFS = "armbat.screen"
    private const val KEY_WIDESCREEN = "widescreen"
    private const val KEY_RESOLUTION = "resolution"
    private const val KEY_REFRESH = "refresh"

    /**
     * The size stored for a virtual screen.
     *
     * [AUTO] is the era's own screen, computed from the archive's target SDK
     * (see GuestLauncher.virtualSize). The others are fixed short sides, and
     * [NATIVE] is the panel itself.
     */
    enum class Resolution(val stored: String) {
        AUTO("auto"),
        P720("720"),
        P1080("1080"),
        NATIVE("native"),
        ;

        companion object {
            fun of(text: String?): Resolution? = entries.firstOrNull { it.stored == text }
        }
    }

    /** The automatic rate: the rate the guest's era assumes (see GuestLauncher). */
    const val AUTO_RATE = 0

    /**
     * The rates offered besides the automatic one, in Hz. An engine that steps
     * its world once per presented frame runs faster on a faster display, so
     * anything above the era's sixty is a choice and never a default.
     */
    val OFFERED_RATES = listOf(60, 90, 120)

    fun widescreen(context: Context): Boolean =
        prefs(context).getBoolean(KEY_WIDESCREEN, false)

    fun setWidescreen(context: Context, on: Boolean) {
        prefs(context).edit().putBoolean(KEY_WIDESCREEN, on).apply()
    }

    /**
     * The size a guest gets: the package's own choice when it has one, else the
     * global one. A package without an entry follows the global setting as it
     * changes; it never freezes the value that was current when it was added.
     */
    fun resolution(context: Context, packageName: String? = null): Resolution {
        if (packageName != null) {
            resolutionOverride(context, packageName)?.let { return it }
        }
        return Resolution.of(prefs(context).getString(KEY_RESOLUTION, null)) ?: Resolution.AUTO
    }

    /** The size chosen for this package alone, or null when it follows the global one. */
    fun resolutionOverride(context: Context, packageName: String): Resolution? =
        Resolution.of(prefs(context).getString("$KEY_RESOLUTION.$packageName", null))

    /** Writes a choice for a package, or null to put it back on the global one. */
    fun setResolution(context: Context, packageName: String?, value: Resolution?) {
        val key = if (packageName == null) KEY_RESOLUTION else "$KEY_RESOLUTION.$packageName"
        val editor = prefs(context).edit()
        if (value == null) editor.remove(key) else editor.putString(key, value.stored)
        editor.apply()
    }

    /**
     * The rate a guest asks for, in Hz, or [AUTO_RATE] when it should keep the
     * era's rate. The package's own choice wins over the global one.
     */
    fun refreshRate(context: Context, packageName: String? = null): Int {
        if (packageName != null) {
            refreshOverride(context, packageName)?.let { return it }
        }
        return prefs(context).getInt(KEY_REFRESH, AUTO_RATE)
    }

    /** The rate chosen for this package alone, or null when it follows the global one. */
    fun refreshOverride(context: Context, packageName: String): Int? {
        val prefs = prefs(context)
        val key = "$KEY_REFRESH.$packageName"
        return if (prefs.contains(key)) prefs.getInt(key, AUTO_RATE) else null
    }

    /** Writes a rate for a package, or null to put it back on the global one. */
    fun setRefreshRate(context: Context, packageName: String?, hz: Int?) {
        val key = if (packageName == null) KEY_REFRESH else "$KEY_REFRESH.$packageName"
        val editor = prefs(context).edit()
        if (hz == null) editor.remove(key) else editor.putInt(key, hz)
        editor.apply()
    }

    /**
     * The offered rates this display can actually show, ascending.
     *
     * A panel that offers only 60 and 120 must not be promised 90: the platform
     * would round the request to a mode it has, and the row would keep saying
     * something the guest never gets.
     */
    fun refreshRates(context: Context): List<Int> {
        val display = display(context) ?: return listOf(60)
        // A mode may carry the rates it can switch between without a mode
        // change, and a panel with a single mode exposes its rates only there.
        val panel = display.supportedModes
            .flatMap { mode -> listOf(mode.refreshRate) + mode.alternativeRefreshRates.asList() }
            .map { Math.round(it) }
        return OFFERED_RATES.filter { rate -> panel.contains(rate) }.ifEmpty { listOf(60) }
    }

    /**
     * Every rate worth showing in a picker: the automatic one, the rates the
     * display supports, and whatever is stored even if this display no longer
     * supports it -- otherwise a choice made on another display would look like
     * the automatic one while still being in force.
     */
    fun refreshChoices(context: Context, current: Int): List<Int> {
        val supported = refreshRates(context)
        val rates = (supported + current).filter { it != AUTO_RATE }.distinct().sorted()
        return listOf(AUTO_RATE) + rates
    }

    /**
     * The panel's own pixels, short side first, in its natural orientation.
     *
     * The largest mode is the panel: the current one may be a reduced mode the
     * system picked for power, and "native" has to mean the display's limit,
     * not whatever it is running at this second.
     */
    fun panelSize(context: Context): Pair<Int, Int> {
        val display = display(context)
        val mode = display?.supportedModes?.maxByOrNull {
            it.physicalWidth.toLong() * it.physicalHeight
        } ?: display?.mode
        val width = mode?.physicalWidth ?: context.resources.displayMetrics.widthPixels
        val height = mode?.physicalHeight ?: context.resources.displayMetrics.heightPixels
        return min(width, height) to max(width, height)
    }

    private fun display(context: Context): Display? = runCatching {
        context.getSystemService(DisplayManager::class.java)?.getDisplay(Display.DEFAULT_DISPLAY)
    }.getOrNull()

    private fun prefs(context: Context) =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
}
