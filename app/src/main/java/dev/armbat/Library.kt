package dev.armbat

import android.content.Context
import android.net.Uri
import java.io.File
import java.util.zip.ZipFile

/** An imported archive and the metadata the launcher draws. */
data class ApkInfo(
    val path: String,
    val label: String,
    val packageName: String,
    val version: String,
    val sizeBytes: Long,
    val arm32: List<String>,
    val arm64: List<String>,
    val iconPath: String?,
)

/**
 * The library on disk: armbat on the shared storage holds the archives, and a
 * game's data and expansion files live next to them under android/, so the
 * whole tree can be filled and copied with a file manager. Metadata is read
 * from the archives themselves.
 *
 * A library refresh runs on a worker thread and touches every archive, so the
 * only per-file work that can be cached is stored next to the data instead: the
 * icon PNG in files/icons.
 */
class Library(private val context: Context) {

    /** What an import did with the archive the user picked. */
    sealed interface Import {
        data class Added(val app: ApkInfo) : Import
        data class Duplicate(val app: ApkInfo) : Import
        data object Unreadable : Import
    }

    /**
     * Every application in the shared folder, newest first, one entry per
     * package.
     *
     * Two archives of the same package are the same application: the newest file
     * wins and the older one is left on disk untouched, so the library never
     * shows one game twice.
     */
    fun read(): List<ApkInfo> = scan().distinctBy { it.packageName }

    fun delete(app: ApkInfo) {
        File(app.path).delete()
        app.iconPath?.let { File(it).delete() }
    }

    /**
     * Copies the picked archive into the library.
     *
     * The same build of the same package cannot be added twice: the library is a
     * list of applications, and two tiles of one game is a mistake. A different
     * version replaces the old file instead, which is how an application is
     * updated.
     */
    fun import(uri: Uri): Import {
        val directory = GuestStorage.root()
        if (!directory.isDirectory && !directory.mkdirs()) {
            return Import.Unreadable
        }
        // Staged in the container's private directory on purpose: scan() reads
        // the library folder, and a half-written archive next to the library is
        // both an invisible extra entry and a duplicate of itself while its
        // metadata is being compared.
        val incoming = File(context.filesDir, "import.apk")
        incoming.delete()
        try {
            context.contentResolver.openInputStream(uri)?.use { input ->
                incoming.outputStream().use { output -> input.copyTo(output, 1 shl 16) }
            } ?: return Import.Unreadable
        } catch (error: Throwable) {
            incoming.delete()
            return Import.Unreadable
        }

        val parsed = inspect(incoming.absolutePath) ?: run {
            incoming.delete()
            return Import.Unreadable
        }

        val existing = scan().filter { it.packageName == parsed.packageName }
        if (existing.any { it.version == parsed.version }) {
            incoming.delete()
            return Import.Duplicate(parsed)
        }

        for (previous in existing) {
            delete(previous)
        }

        val stems = "${parsed.packageName}-${parsed.version.ifBlank { "app" }}"
        val target = File(directory, stems.replace(Regex("[^A-Za-z0-9._-]"), "_") + ".apk")
        if (target.exists()) {
            target.delete()
        }
        val written = runCatching {
            if (!incoming.renameTo(target)) {
                incoming.copyTo(target, overwrite = true)
                incoming.delete()
            }
        }.isSuccess
        if (!written) {
            incoming.delete()
            return Import.Unreadable
        }
        // The data and obb folders exist before the game is ever started, so a
        // downloaded cache can be put in place by hand.
        GuestStorage.ensure(parsed.packageName)
        // The first launch would otherwise spend seconds rewriting the dex.
        // Build the same cache now, while the user is still looking at the
        // library, and let a launch that happens sooner wait on its lock.
        prewarm(target, parsed.packageName)
        return inspect(target.absolutePath)?.let { Import.Added(it) } ?: Import.Unreadable
    }

    private fun prewarm(apk: File, packageName: String) {
        val data = GuestStorage.dataDir(packageName)
        val obb = GuestStorage.obbDir(packageName)
        Thread({
            runCatching {
                GuestDex.prepare(context, apk.absolutePath, packageName, data, obb)
            }.onFailure {
                ArmbatLog.w("Armbat", "dex prewarm: $it")
            }
        }, "armbat-dex-prewarm").apply { isDaemon = true }.start()
    }

    /** Every archive on disk, newest first, before duplicates are collapsed. */
    private fun scan(): List<ApkInfo> {
        val directory = GuestStorage.root()
        val files = directory.listFiles { file -> file.isFile && file.extension.equals("apk", true) }
            ?: return emptyList()
        return files
            .mapNotNull { inspect(it.absolutePath) }
            .onEach { GuestStorage.ensure(it.packageName) }
            .sortedByDescending { File(it.path).lastModified() }
    }

    private fun inspect(path: String): ApkInfo? {
        val manager = context.packageManager
        @Suppress("DEPRECATION")
        val archive = manager.getPackageArchiveInfo(path, 0) ?: return null
        val app = archive.applicationInfo ?: return null
        app.sourceDir = path
        app.publicSourceDir = path

        val fallback = File(path).nameWithoutExtension
        val resources = ApkResources.of(context, path)
        val label = (if (resources != null && app.labelRes != 0) {
            runCatching { resources.getText(app.labelRes).toString() }.getOrNull()
        } else {
            null
        }) ?: runCatching { manager.getApplicationLabel(app).toString() }.getOrNull()
            ?.takeIf { it.isNotBlank() && it != archive.packageName }
            ?: fallback

        val arm32 = mutableListOf<String>()
        val arm64 = mutableListOf<String>()
        runCatching {
            ZipFile(path).use { zip ->
                for (entry in zip.entries()) {
                    val name = entry.name
                    if (!name.startsWith("lib/") || !name.endsWith(".so")) continue
                    when (name.split('/').getOrNull(1)) {
                        "armeabi-v7a", "armeabi" -> arm32 += name.substringAfterLast('/')
                        "arm64-v8a" -> arm64 += name.substringAfterLast('/')
                    }
                }
            }
        }

        val iconKey = "${archive.packageName}-${archive.versionName ?: "0"}"
            .replace(Regex("[^A-Za-z0-9._-]"), "_") + ".png"
        val icon = runCatching { ApkResources.icon(context, path, app.icon, iconKey) }.getOrNull()

        return ApkInfo(
            path = path,
            label = label,
            packageName = archive.packageName,
            version = archive.versionName ?: "",
            sizeBytes = File(path).length(),
            arm32 = arm32,
            arm64 = arm64,
            iconPath = icon?.absolutePath,
        )
    }
}
