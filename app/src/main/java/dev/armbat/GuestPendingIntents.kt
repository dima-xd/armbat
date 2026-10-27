package dev.armbat

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.os.Bundle

/**
 * PendingIntent factories that keep the behaviour an old archive was written
 * for.
 *
 * A PendingIntent without FLAG_IMMUTABLE or FLAG_MUTABLE was mutable on every
 * release the archive could have run on. The framework now rejects that call
 * when the process targets Android 12 or newer, and the process here is the
 * container, so the mutability flags are supplied at the call site (the guest
 * dex is redirected here by [GuestDex]). The unsafe implicit flag does the same
 * for the later rule that blocks mutable implicit intents at send time, which
 * the system server also evaluates against the container's package.
 *
 * An activity PendingIntent that names the guest's own package is another
 * thing the platform refuses to create: the uid belongs to the container, so
 * sending as `com.ea.game.fifa14_row` is denied before the object exists. The
 * archive's components are not in the container's manifest either, so the
 * intent is pointed at the launcher with the archive in the `launch` extra --
 * the same path `adb shell am start -e launch` uses. Tapping such a
 * notification opens the archive the way its own component would have.
 *
 * A guest that already passes either mutability flag is not touched.
 */
object GuestPendingIntents {

    private const val FLAG_IMMUTABLE = 1 shl 26
    private const val FLAG_MUTABLE = 1 shl 25
    private const val FLAG_ALLOW_UNSAFE_IMPLICIT_INTENT = 1 shl 24

    private val CONTAINER_PACKAGE = MainActivity::class.java.packageName
    private val MAIN_ACTIVITY = MainActivity::class.java.name

    /**
     * The container's own context. A PendingIntent belongs to the package of
     * the context it was created from, and the platform checks that package
     * against the uid; the guest's own context names a package the uid does
     * not own, so the object is created on behalf of the container instead.
     */
    @Volatile
    var host: Context? = null

    private fun legacy(flags: Int): Int =
        if (flags and (FLAG_MUTABLE or FLAG_IMMUTABLE) != 0) {
            flags
        } else {
            flags or FLAG_MUTABLE or FLAG_ALLOW_UNSAFE_IMPLICIT_INTENT
        }

    /**
     * Points an intent that addresses the guest's own package at the launcher.
     *
     * The system refuses to create a PendingIntent that sends as a package the
     * caller does not own; the package the guest thinks it is exists only in
     * its own manifest and resources, not in the platform's package list.
     */
    private fun route(context: Context, intent: Intent): Intent {
        val guest = context.packageName
        if (guest == CONTAINER_PACKAGE) {
            return intent
        }
        val own = intent.component?.packageName == guest || intent.`package` == guest
        if (!own) {
            return intent
        }
        return Intent(Intent.ACTION_MAIN)
            .setClassName(CONTAINER_PACKAGE, MAIN_ACTIVITY)
            .putExtra("launch", guest)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    }

    @JvmStatic
    fun getActivity(
        context: Context,
        requestCode: Int,
        intent: Intent,
        flags: Int,
    ): PendingIntent = PendingIntent.getActivity(
        sender(context),
        requestCode,
        route(context, intent),
        legacy(flags),
    )

    @JvmStatic
    fun getActivity(
        context: Context,
        requestCode: Int,
        intent: Intent,
        flags: Int,
        options: Bundle?,
    ): PendingIntent = PendingIntent.getActivity(
        sender(context),
        requestCode,
        route(context, intent),
        legacy(flags),
        options,
    )

    @JvmStatic
    fun getActivities(
        context: Context,
        requestCode: Int,
        intents: Array<Intent>,
        flags: Int,
    ): PendingIntent = PendingIntent.getActivities(
        sender(context),
        requestCode,
        intents.map { route(context, it) }.toTypedArray(),
        legacy(flags),
    )

    @JvmStatic
    fun getActivities(
        context: Context,
        requestCode: Int,
        intents: Array<Intent>,
        flags: Int,
        options: Bundle?,
    ): PendingIntent = PendingIntent.getActivities(
        sender(context),
        requestCode,
        intents.map { route(context, it) }.toTypedArray(),
        legacy(flags),
        options,
    )

    @JvmStatic
    fun getBroadcast(
        context: Context,
        requestCode: Int,
        intent: Intent,
        flags: Int,
    ): PendingIntent =
        PendingIntent.getBroadcast(sender(context), requestCode, intent, legacy(flags))

    @JvmStatic
    fun getService(
        context: Context,
        requestCode: Int,
        intent: Intent,
        flags: Int,
    ): PendingIntent =
        PendingIntent.getService(sender(context), requestCode, intent, legacy(flags))

    @JvmStatic
    fun getForegroundService(
        context: Context,
        requestCode: Int,
        intent: Intent,
        flags: Int,
    ): PendingIntent =
        PendingIntent.getForegroundService(sender(context), requestCode, intent, legacy(flags))

    private fun sender(context: Context): Context = host ?: context
}
