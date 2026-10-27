package dev.armbat

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Environment
import android.provider.Settings
import java.io.File
import androidx.core.net.toUri

/**
 * The shared tree the container keeps its guests in.
 *
 * Everything a guest owns lives under one folder on the shared storage instead
 * of the container's private directory: the archives themselves, a game's
 * internal data and its expansion files. The user can then put an APK or a
 * downloaded obb there with a file manager or adb, and back a save up by
 * copying it out. The layout mirrors what an installed application has, one
 * level below the container's own folder:
 *
 *     armbat/<name>.apk
 *     armbat/android/data/<package>/{files,cache,databases,shared_prefs}
 *     armbat/android/obb/<package>
 *
 * The shared storage is only reachable with "all files access": since Android
 * 11 an application cannot read an arbitrary folder on /sdcard without it.
 * While it is missing the tree is unusable, so the launcher asks for it before
 * anything else and the library stays empty until the answer is yes.
 */
object GuestStorage {

    /** The folder under the shared storage root. */
    private const val FOLDER = "armbat"

    /** Shared storage root, e.g. /storage/emulated/0. */
    @Suppress("DEPRECATION")
    fun root(): File = File(Environment.getExternalStorageDirectory(), FOLDER)

    fun isAvailable(): Boolean = Environment.isExternalStorageManager()

    /** The guest's external tree, the stand-in for Android/data on the shared storage. */
    fun dataDir(packageName: String): File = File(root(), "android/data/$packageName")

    /**
     * The external files directory of a guest. It is deliberately not the same
     * directory as the internal one (see GuestLauncher.getExternalFilesDir):
     * the platform separates them because an application may wipe the external
     * tree, and a guest that does so must not lose its private files.
     */
    fun externalFilesDir(packageName: String): File = File(externalDir(packageName), "files")

    fun externalCacheDir(packageName: String): File = File(externalDir(packageName), "cache")

    private fun externalDir(packageName: String): File = File(dataDir(packageName), "external")

    /** The guest's expansion files, the stand-in for Android/obb. */
    fun obbDir(packageName: String): File = File(root(), "android/obb/$packageName")

    private val DATA_FOLDERS = listOf("files", "cache", "databases", "shared_prefs")

    /**
     * Creates the directories an application expects to exist before it writes
     * into them, so a user can drop an obb or a save in place before the game
     * has ever been started.
     */
    fun ensure(packageName: String) {
        if (packageName.isEmpty()) {
            return
        }
        val data = dataDir(packageName)
        for (name in DATA_FOLDERS) {
            File(data, name).mkdirs()
        }
        externalFilesDir(packageName).mkdirs()
        externalCacheDir(packageName).mkdirs()
        obbDir(packageName).mkdirs()
    }

    /**
     * The system screen that grants the shared storage. The per-application
     * one is the only way to ask; the general list is the fallback for a
     * platform that does not answer the first intent.
     */
    fun request(context: Context) {
        val perApp = Intent(
            Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
            "package:${context.packageName}".toUri(),
        )
        runCatching { context.startActivity(perApp) }.onFailure {
            runCatching {
                context.startActivity(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
            }
        }
    }
}
