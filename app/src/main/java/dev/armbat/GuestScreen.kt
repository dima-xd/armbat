package dev.armbat

import android.content.Context

/**
 * How much of the panel a guest's screen takes.
 *
 * The default is the screen the archive was written for: an old game lays its
 * interface out in the pixels of a 480x800 phone, its layout has no room for a
 * 19.5:9 panel, and the picture is scaled to the panel with bars where the
 * aspect differs. Widescreen gives the guest the panel's aspect on its long
 * side instead -- the short side keeps the old pixel size -- so the layout has
 * the space it has on a modern phone and the picture fills the display, which
 * is what the same application does when installed on the device.
 */
object GuestScreen {
    private const val PREFS = "armbat.screen"
    private const val KEY_WIDESCREEN = "widescreen"

    fun widescreen(context: Context): Boolean =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getBoolean(KEY_WIDESCREEN, false)

    fun setWidescreen(context: Context, on: Boolean) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit()
            .putBoolean(KEY_WIDESCREEN, on)
            .apply()
    }
}
