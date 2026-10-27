package dev.armbat

import java.io.File

/**
 * Answers whether a dex file contains any of the absolute paths the container
 * has to rewrite.
 *
 * The answer comes from the dex string table, not from a scan of the whole
 * file: an archive that stores a path in a string it never loads still pays for
 * the rewrite, but an archive with no such string never reaches dexlib2. The
 * table is already the thing the rewriter edits, so this is both the cheapest
 * and the most precise place to look.
 *
 * A malformed dex answers true: the caller then follows the old path and lets
 * dexlib2 report the real problem instead of silently loading a broken file.
 */
internal object GuestDexScan {

    fun containsAny(file: File, prefixes: List<String>): Boolean =
        runCatching { containsAny(file.readBytes(), prefixes) }.getOrDefault(true)

    fun containsAny(bytes: ByteArray, prefixes: List<String>): Boolean {
        // Header: magic, then string_ids_size at 0x38 and string_ids_off at
        // 0x3c in little-endian order.
        if (bytes.size < 0x70 || !hasDexMagic(bytes)) {
            return true
        }
        val count = intAt(bytes, 0x38)
        val table = intAt(bytes, 0x3C)
        if (count < 0 || table < 0x70 || table.toLong() + count.toLong() * 4L > bytes.size) {
            return true
        }
        val needles = prefixes
            .filter { it.isNotEmpty() }
            .map { it.toByteArray(Charsets.US_ASCII) }
        if (needles.isEmpty()) {
            return false
        }
        for (index in 0 until count) {
            val item = intAt(bytes, table + index * 4)
            if (item < 0 || item >= bytes.size) {
                return true
            }
            // string_data_item is a uleb128 utf16 length followed by MUTF-8 and
            // a NUL. The length is not needed; only the bytes are compared.
            var cursor = item
            var shift = 0
            while (true) {
                if (cursor >= bytes.size) {
                    return true
                }
                val byte = bytes[cursor++].toInt() and 0xFF
                if (byte and 0x80 == 0) {
                    break
                }
                shift += 7
                if (shift > 28) {
                    return true
                }
            }
            val start = cursor
            while (cursor < bytes.size && bytes[cursor] != 0.toByte()) {
                cursor++
            }
            if (cursor >= bytes.size) {
                return true
            }
            val length = cursor - start
            for (needle in needles) {
                if (length >= needle.size && matches(bytes, start, needle)) {
                    return true
                }
            }
        }
        return false
    }

    private fun hasDexMagic(bytes: ByteArray): Boolean =
        bytes[0] == 'd'.code.toByte() &&
            bytes[1] == 'e'.code.toByte() &&
            bytes[2] == 'x'.code.toByte() &&
            bytes[3] == '\n'.code.toByte() &&
            bytes[7] == 0.toByte()

    private fun intAt(bytes: ByteArray, offset: Int): Int =
        (bytes[offset].toInt() and 0xFF) or
            ((bytes[offset + 1].toInt() and 0xFF) shl 8) or
            ((bytes[offset + 2].toInt() and 0xFF) shl 16) or
            ((bytes[offset + 3].toInt() and 0xFF) shl 24)

    private fun matches(bytes: ByteArray, start: Int, needle: ByteArray): Boolean {
        for (index in needle.indices) {
            if (bytes[start + index] != needle[index]) {
                return false
            }
        }
        return true
    }
}
