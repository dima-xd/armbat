package dev.armbat

import android.content.Context
import android.system.Os
import android.system.OsConstants
import dalvik.system.DexClassLoader
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.security.MessageDigest

/**
 * A DexClassLoader used by guest code on current Android releases.
 *
 * Android 14 and later reject a DEX/JAR/APK that is writable when ART opens it.
 * Older SDKs commonly download a JAR into Context.getCacheDir() and immediately
 * hand that path to DexClassLoader without clearing its write bit. Guest cache
 * paths are intentionally on shared storage in Armbat, so ART rejects them.
 *
 * Stage regular-file dex-path entries into Armbat's private code cache, mark the
 * copies read-only, and leave the guest's original cache file untouched. This
 * both meets ART's requirement and lets the guest SDK continue updating or
 * deleting its original download later.
 */
class GuestDexClassLoader(
    dexPath: String,
    optimizedDirectory: String?,
    librarySearchPath: String?,
    parent: ClassLoader?,
) : DexClassLoader(
    GuestDexRuntime.stageDexPath(dexPath),
    optimizedDirectory,
    librarySearchPath,
    parent,
)

/** Host-side state and staging for [GuestDexClassLoader]. */
internal object GuestDexRuntime {
    @Volatile
    private var hostContext: Context? = null

    private val lock = Any()

    fun initialize(context: Context) {
        hostContext = context.applicationContext
    }

    fun stageDexPath(dexPath: String): String {
        val context = hostContext
            ?: throw IllegalStateException("Armbat DEX runtime was not initialized")
        val entries = dexPath.split(File.pathSeparatorChar)
        val root = File(context.codeCacheDir, "guest-dex")
        if (!root.isDirectory && !root.mkdirs() && !root.isDirectory) {
            throw IOException("Cannot create Armbat DEX cache: ${root.absolutePath}")
        }
        return synchronized(lock) {
            entries.joinToString(File.pathSeparator) { entry ->
                if (entry.isEmpty()) {
                    entry
                } else {
                    stageEntry(File(entry), root).absolutePath
                }
            }
        }
    }

    private fun stageEntry(source: File, root: File): File {
        // Keep directories and missing entries as-is. DexClassLoader accepts
        // directories on its path, and preserving missing paths leaves ART to
        // produce its normal diagnostic rather than hiding the original error.
        if (!source.isFile) return source.absoluteFile

        val temporary = File.createTempFile("stage-", ".tmp", root)
        try {
            source.inputStream().use { input ->
                FileOutputStream(temporary).use { output ->
                    // Match Android's recommended DCL sequence: open the new
                    // file, remove its write bit, then write through the already
                    // open descriptor so no writable DEX ever exists on disk.
                    if (!temporary.setReadOnly()) {
                        throw IOException("Cannot make staged DEX read-only: ${temporary.absolutePath}")
                    }
                    input.copyTo(output)
                    output.fd.sync()
                }
            }
            val digest = sha256(temporary)
            val extension = source.extension
                .takeIf { it.isNotBlank() && it.length <= 10 }
                ?.let { ".$it" } ?: ".dex"
            val destination = File(root, "$digest$extension")

            if (destination.isFile && sha256(destination) != digest) {
                // The filename is content-addressed; a mismatch means a stale
                // or altered cached copy, so don't hand it to ART.
                if (!destination.delete()) {
                    throw IOException("Cannot replace altered staged DEX: ${destination.absolutePath}")
                }
            }
            if (!destination.isFile && !temporary.renameTo(destination) && !destination.isFile) {
                throw IOException("Cannot publish staged DEX: ${destination.absolutePath}")
            }
            temporary.delete()

            // If a correct content-addressed file already existed, verify it
            // above, then repair its mode before allowing ART to open it.
            requireReadOnly(destination)
            ArmbatLog.i(
                "Armbat",
                "staged dynamic dex: ${source.absolutePath} -> ${destination.absolutePath}",
            )
            return destination
        } catch (error: Throwable) {
            temporary.delete()
            throw error
        }
    }

    private fun requireReadOnly(file: File) {
        if (!file.setReadOnly()) {
            throw IOException("Cannot make staged DEX read-only: ${file.absolutePath}")
        }
        // File.canWrite() is affected by superuser capabilities. ART's check
        // is about the file's actual mode bits, so inspect those directly.
        val writeMask = OsConstants.S_IWUSR or OsConstants.S_IWGRP or OsConstants.S_IWOTH
        if (Os.stat(file.absolutePath).st_mode and writeMask != 0) {
            throw SecurityException("Staged DEX is still writable: ${file.absolutePath}")
        }
    }

    private fun sha256(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                if (count > 0) digest.update(buffer, 0, count)
            }
        }
        val bytes = digest.digest()
        return buildString(bytes.size * 2) {
            for (byte in bytes) {
                append("%02x".format(byte.toInt() and 0xff))
            }
        }
    }
}
