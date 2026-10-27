package dev.armbat

import java.io.ByteArrayOutputStream
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class GuestDexScanTest {

    @Test
    fun findsSdcardInStringTable() {
        val dex = dexWith("hello", "/sdcard/games/com.example")

        assertTrue(GuestDexScan.containsAny(dex, listOf("/sdcard")))
    }

    @Test
    fun ignoresUnrelatedStrings() {
        val dex = dexWith("hello", "com.example.hello")

        assertFalse(GuestDexScan.containsAny(dex, listOf("/sdcard", "/data/data/com.example")))
    }

    @Test
    fun findsPackagePath() {
        val dex = dexWith("hello", "/data/data/com.example/files")

        assertTrue(GuestDexScan.containsAny(dex, listOf("/data/data/com.example")))
    }

    @Test
    fun malformedDexFallsBackToRewrite() {
        assertTrue(GuestDexScan.containsAny(byteArrayOf(1, 2, 3), listOf("/sdcard")))
    }

    /** A dex-shaped buffer with a valid header and a string table, no code. */
    private fun dexWith(vararg strings: String): ByteArray {
        val header = 0x70
        val ids = strings.size * 4
        val dataStart = header + ids
        val data = ByteArrayOutputStream()
        val offsets = IntArray(strings.size)

        strings.forEachIndexed { index, value ->
            offsets[index] = dataStart + data.size()
            val bytes = value.toByteArray(Charsets.US_ASCII)
            uleb(data, bytes.size)
            data.write(bytes)
            data.write(0)
        }

        val out = ByteArray(dataStart + data.size())
        val magic = "dex\n035\u0000".toByteArray(Charsets.US_ASCII)
        magic.copyInto(out, 0)
        putInt(out, 0x38, strings.size)
        putInt(out, 0x3C, header)
        offsets.forEachIndexed { index, offset -> putInt(out, header + index * 4, offset) }
        data.toByteArray().copyInto(out, dataStart)
        return out
    }

    private fun uleb(out: ByteArrayOutputStream, value: Int) {
        var remaining = value
        while (remaining >= 0x80) {
            out.write((remaining and 0x7F) or 0x80)
            remaining = remaining ushr 7
        }
        out.write(remaining)
    }

    private fun putInt(out: ByteArray, offset: Int, value: Int) {
        out[offset] = value.toByte()
        out[offset + 1] = (value ushr 8).toByte()
        out[offset + 2] = (value ushr 16).toByte()
        out[offset + 3] = (value ushr 24).toByte()
    }
}
