package dev.armbat

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.file.Files
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

class GuestDataTest {

    @Test
    fun readsExpansionName() {
        val expansion = GuestData.expansion("main.42.com.example.game.obb")
        assertNotNull(expansion)
        assertEquals("main", expansion!!.kind)
        assertEquals("42", expansion.version)
        assertEquals("com.example.game", expansion.packageName)
    }

    @Test
    fun readsPatchWithUppercaseExtension() {
        val expansion = GuestData.expansion("patch.7.com.example.obb")
        assertNotNull(expansion)
        assertEquals("patch", expansion!!.kind)
        assertEquals("com.example", expansion.packageName)
    }

    @Test
    fun ignoresNamesThatAreNotExpansionFiles() {
        assertNull(GuestData.expansion("main.42.com.example.game.obb.bak"))
        assertNull(GuestData.expansion("com.example.game.obb"))
        assertNull(GuestData.expansion("main.42.game.obb"))
        assertNull(GuestData.expansion("main..com.example.obb"))
        assertNull(GuestData.expansion("main.42.com.example.zip"))
        assertNull(GuestData.expansion("xmain.42.com.example.obb"))
    }

    @Test
    fun keepsPlainRelativeZipNames() {
        assertEquals("a.txt", GuestData.safePath("a.txt"))
        assertEquals("Android/obb/com.example/main.1.com.example.obb", GuestData.safePath(
            "Android/obb/com.example/main.1.com.example.obb",
        ))
        assertEquals("a b/c.d", GuestData.safePath("a b/c.d"))
    }

    @Test
    fun refusesPathsThatLeaveTheRoot() {
        assertNull(GuestData.safePath("../escape.txt"))
        assertNull(GuestData.safePath("a/../../escape.txt"))
        assertNull(GuestData.safePath("/absolute.txt"))
        assertNull(GuestData.safePath("./relative.txt"))
        assertNull(GuestData.safePath("a//b.txt"))
        assertNull(GuestData.safePath("windows\\path.txt"))
        assertNull(GuestData.safePath("C:/drive.txt"))
        assertNull(GuestData.safePath(""))
    }

    @Test
    fun extractsFilesAndDropsTraversalEntries() {
        val root = Files.createTempDirectory("guestdata").toFile()
        try {
            val archive = ByteArrayOutputStream()
            ZipOutputStream(archive).use { zip ->
                zip.putNextEntry(ZipEntry("game/save.dat"))
                zip.write(byteArrayOf(1, 2, 3))
                zip.closeEntry()
                zip.putNextEntry(ZipEntry("readme.txt"))
                zip.write("hello".toByteArray())
                zip.closeEntry()
                zip.putNextEntry(ZipEntry("../escape.txt"))
                zip.write(byteArrayOf(9))
                zip.closeEntry()
                zip.putNextEntry(ZipEntry("__MACOSX/junk"))
                zip.write(byteArrayOf(9))
                zip.closeEntry()
            }

            val result = GuestData.extract(
                ByteArrayInputStream(archive.toByteArray()),
                root,
                expected = archive.size().toLong(),
            )

            assertEquals(2, result.files)
            assertEquals(5L + 3L, result.bytes)
            assertEquals(2, result.skipped)
            assertEquals(3, File(root, "game/save.dat").readBytes().size)
            assertEquals("hello", File(root, "readme.txt").readText())
            assertFalse(File(root.parentFile, "escape.txt").exists())
        } finally {
            root.deleteRecursively()
        }
    }

    @Test
    fun stopsWhenCancelled() {
        val root = Files.createTempDirectory("guestdata").toFile()
        try {
            val archive = ByteArrayOutputStream()
            ZipOutputStream(archive).use { zip ->
                zip.putNextEntry(ZipEntry("one.txt"))
                zip.write(ByteArray(1024))
                zip.closeEntry()
                zip.putNextEntry(ZipEntry("two.txt"))
                zip.write(ByteArray(1024))
                zip.closeEntry()
            }

            val result = GuestData.extract(
                ByteArrayInputStream(archive.toByteArray()),
                root,
                expected = 0,
                cancelled = { true },
            )

            assertEquals(0, result.files)
            assertFalse(File(root, "one.txt").exists())
        } finally {
            root.deleteRecursively()
        }
    }
}
