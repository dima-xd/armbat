package dev.armbat

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.util.zip.ZipInputStream

/**
 * Expansion files and cache archives brought in through the system picker.
 *
 * A current platform leaves an application no way to read the shared storage
 * except through the picker, and the one folder a game looks its own files up
 * in -- `Android/obb` -- is not even listed there: the tree the guest writes
 * into is this application's, and no file manager can open it. So the two files
 * users actually have on hand, an `.obb` from a download and a `.zip` packed
 * from a game's cache, are taken in here and placed by the container itself.
 *
 * Nothing in this file knows about a particular game: an expansion file says
 * whose it is in its name, and an archive lands where its own paths say.
 */
object GuestData {

    /** What an expansion file name claims about its owner. */
    data class Expansion(val kind: String, val version: String, val packageName: String)

    /** What an extraction wrote, for the report the launcher shows. */
    data class Extract(val files: Int, val bytes: Long, val skipped: Int)

    /**
     * The folder a cache archive is unpacked into.
     *
     * A game's cache is shipped in one of four layouts, and the archive itself
     * only says which by where its paths start: a dump of `Android/obb` keeps
     * that prefix, a dump of the game's `files` does not. ROOT is the first
     * case and needs no package at all -- the prefix already names it.
     */
    enum class Target { ROOT, INTERNAL, EXTERNAL, DATA }

    /**
     * `main.<version>.<package>.obb` and `patch.<version>.<package>.obb`, the
     * only two names an expansion file can have.
     */
    private val EXPANSION_NAME = Regex(
        "^(main|patch)\\.(\\d+)\\.([A-Za-z][A-Za-z0-9_]*(?:\\.[A-Za-z0-9_]+)+)\\.obb$",
        RegexOption.IGNORE_CASE,
    )

    /** An archive that decompresses to more than this is not game data. */
    private const val MAX_BYTES = 8L * 1024 * 1024 * 1024

    /** Same reasoning: no game ships this many files. */
    private const val MAX_ENTRIES = 200_000

    /** A name the archive creator left behind, not something a game reads. */
    private fun isJunk(path: String): Boolean =
        path.startsWith("__MACOSX/") || path.endsWith("/.DS_Store") || path.endsWith("/Thumbs.db")

    /** The expansion file a name describes, or null when it describes none. */
    fun expansion(name: String): Expansion? {
        val match = EXPANSION_NAME.matchEntire(name.trim()) ?: return null
        return Expansion(
            kind = match.groupValues[1].lowercase(),
            version = match.groupValues[2],
            packageName = match.groupValues[3],
        )
    }

    /**
     * Where a zip entry may be written, relative to the extraction root, or null
     * when it must be skipped.
     *
     * An archive is an untrusted file: it can name `../../files/x` or an
     * absolute path, and a writer that joins names as they come is the classic
     * zip slip. Only plain relative names with no empty, `.` or `..` segment
     * survive. The caller still compares the canonical result with the root,
     * because a name is not the only thing that can be wrong with an archive.
     */
    fun safePath(name: String): String? {
        if (name.isEmpty() || name.startsWith("/") || name.contains('\\') || name.contains('\u0000')) {
            return null
        }
        val parts = name.split('/')
        if (parts.any { it.isEmpty() || it == "." || it == ".." }) {
            return null
        }
        // A drive letter or a scheme-looking first segment is not a relative
        // path either, and a game never ships one.
        if (parts.first().contains(':')) {
            return null
        }
        return parts.joinToString("/")
    }

    /** The name the picker's provider reports for a document. */
    fun displayName(context: Context, uri: Uri): String? {
        if (uri.scheme == "file") {
            return uri.lastPathSegment
        }
        return runCatching {
            context.contentResolver.query(
                uri,
                arrayOf(OpenableColumns.DISPLAY_NAME),
                null,
                null,
                null,
            )?.use { cursor ->
                if (cursor.moveToFirst() && !cursor.isNull(0)) cursor.getString(0) else null
            }
        }.getOrNull()
    }

    /** The size the provider reports, or -1 when it does not report one. */
    fun size(context: Context, uri: Uri): Long {
        if (uri.scheme == "file") {
            return uri.path?.let { File(it).length() } ?: -1
        }
        return runCatching {
            context.contentResolver.query(uri, arrayOf(OpenableColumns.SIZE), null, null, null)
                ?.use { cursor ->
                    if (cursor.moveToFirst() && !cursor.isNull(0)) cursor.getLong(0) else -1
                } ?: -1
        }.getOrDefault(-1)
    }

    /**
     * Copies [input] to [target] through a sibling `.part` file.
     *
     * A game reads its expansion file by opening it, and a half-written one is
     * worse than a missing one: the game either fails on a truncated archive or
     * caches the failure. The temporary name keeps the target invisible until it
     * is whole, and a cancellation leaves nothing behind.
     */
    fun copy(
        input: InputStream,
        target: File,
        expected: Long,
        cancelled: () -> Boolean = { false },
        progress: (Int) -> Unit = {},
    ): Boolean {
        val part = File(target.parentFile, target.name + ".part")
        part.delete()
        var written = 0L
        try {
            input.use { source ->
                part.outputStream().buffered(1 shl 16).use { output ->
                    val buffer = ByteArray(1 shl 16)
                    while (true) {
                        if (cancelled()) {
                            part.delete()
                            return false
                        }
                        val read = source.read(buffer)
                        if (read < 0) {
                            break
                        }
                        output.write(buffer, 0, read)
                        written += read
                        if (expected > 0) {
                            progress(((written * 100) / expected).toInt().coerceIn(0, 99))
                        }
                    }
                }
            }
            target.delete()
            if (!part.renameTo(target)) {
                part.delete()
                return false
            }
            progress(100)
            return true
        } catch (error: IOException) {
            part.delete()
            return false
        }
    }

    /**
     * Streams every regular file of [input] under [root].
     *
     * Entries are written through a `.part` name and renamed, so a cancelled or
     * failed extraction leaves whole files rather than a truncated one a game
     * would try to read. [expected] is the size of the picked file, used only to
     * report progress.
     */
    fun extract(
        input: InputStream,
        root: File,
        expected: Long,
        cancelled: () -> Boolean = { false },
        progress: (Int) -> Unit = {},
    ): Extract {
        val source = Counting(input)
        val rootPath = root.canonicalPath + File.separator
        var files = 0
        var written = 0L
        var skipped = 0
        val buffer = ByteArray(1 shl 16)

        ZipInputStream(source.buffered(1 shl 16)).use { entries ->
            while (true) {
                if (cancelled()) {
                    return Extract(files, written, skipped)
                }
                val entry = entries.nextEntry ?: break
                val relative = safePath(entry.name)
                if (entry.isDirectory || relative == null || isJunk(relative)) {
                    skipped++
                    continue
                }
                val target = File(root, relative)
                if (!target.canonicalPath.startsWith(rootPath)) {
                    skipped++
                    continue
                }
                if (files >= MAX_ENTRIES) {
                    throw IOException("archive has too many entries")
                }
                target.parentFile?.mkdirs()
                val part = File(target.parentFile, target.name + ".part")
                part.delete()
                part.outputStream().buffered(1 shl 16).use { output ->
                    while (true) {
                        if (cancelled()) {
                            part.delete()
                            return Extract(files, written, skipped)
                        }
                        val read = entries.read(buffer)
                        if (read < 0) {
                            break
                        }
                        output.write(buffer, 0, read)
                        written += read
                        if (written > MAX_BYTES) {
                            throw IOException("archive unpacks past the limit")
                        }
                        if (expected > 0) {
                            progress(((source.read * 100) / expected).toInt().coerceIn(0, 99))
                        }
                    }
                }
                target.delete()
                if (!part.renameTo(target)) {
                    part.delete()
                    throw IOException("could not place ${target.name}")
                }
                files++
            }
        }
        if (expected > 0) {
            progress(100)
        }
        return Extract(files, written, skipped)
    }

    /** Counts the bytes the zip reader consumed, for the progress percentage. */
    private class Counting(private val source: InputStream) : InputStream() {

        var read = 0L
            private set

        override fun read(): Int = source.read().also { if (it >= 0) read++ }

        override fun read(buffer: ByteArray, offset: Int, length: Int): Int =
            source.read(buffer, offset, length).also { if (it > 0) read += it }

        override fun close() = source.close()
    }
}
