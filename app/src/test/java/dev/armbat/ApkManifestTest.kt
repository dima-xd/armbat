package dev.armbat

import java.io.ByteArrayOutputStream
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class ApkManifestTest {

    @Test
    fun picksLauncherDeclaredAfterHelperActivity() {
        assertEquals(
            "com.nvidia.residentevil5.UE3JavaDownloaderActivity",
            ApkManifest.launcherActivity(residentEvilShaped(utf8 = false)),
        )
    }

    @Test
    fun readsUtf8StringPool() {
        assertEquals(
            "com.nvidia.residentevil5.UE3JavaDownloaderActivity",
            ApkManifest.launcherActivity(residentEvilShaped(utf8 = true)),
        )
    }

    @Test
    fun prefersLauncherOverMainOnlyActivity() {
        val manifest = document(
            element(
                "manifest",
                element(
                    "application",
                    element(
                        "activity",
                        mapOf("name" to "com.example.Service"),
                        element("intent-filter", element("action", main())),
                    ),
                    element("activity", mapOf("name" to "com.example.Launcher"), launcherFilter()),
                ),
            ),
        )

        assertEquals("com.example.Launcher", ApkManifest.launcherActivity(manifest))
    }

    @Test
    fun skipsDisabledLauncher() {
        val manifest = document(
            element(
                "manifest",
                element(
                    "application",
                    element(
                        "activity",
                        mapOf("name" to "com.example.Disabled", "enabled" to "false"),
                        launcherFilter(),
                    ),
                    element("activity", mapOf("name" to "com.example.Launcher"), launcherFilter()),
                ),
            ),
        )

        assertEquals("com.example.Launcher", ApkManifest.launcherActivity(manifest))
    }

    @Test
    fun followsAliasTarget() {
        val manifest = document(
            element(
                "manifest",
                element(
                    "application",
                    element("activity", mapOf("name" to "com.example.Target")),
                    element(
                        "activity-alias",
                        mapOf("name" to "com.example.Alias", "targetActivity" to "com.example.Target"),
                        launcherFilter(),
                    ),
                ),
            ),
        )

        assertEquals("com.example.Target", ApkManifest.launcherActivity(manifest))
    }

    @Test
    fun expandsRelativeComponentName() {
        val manifest = document(
            element(
                "manifest",
                mapOf("package" to "com.example"),
                element(
                    "application",
                    element("activity", mapOf("name" to ".Main"), launcherFilter()),
                ),
            ),
        )

        assertEquals("com.example.Main", ApkManifest.launcherActivity(manifest))
    }

    @Test
    fun manifestWithoutEntryPointAnswersNothing() {
        val manifest = document(
            element(
                "manifest",
                element(
                    "application",
                    element("activity", mapOf("name" to "com.example.Helper")),
                ),
            ),
        )

        assertNull(ApkManifest.launcherActivity(manifest))
    }

    @Test
    fun malformedManifestAnswersNothing() {
        assertNull(ApkManifest.launcherActivity(byteArrayOf(1, 2, 3)))
    }

    /** The shape Resident Evil 5 has: a Play Games wrapper first, the launcher second. */
    private fun residentEvilShaped(utf8: Boolean): ByteArray = document(
        utf8 = utf8,
        root = element(
            "manifest",
            element(
                "application",
                element("activity", mapOf("name" to "com.nvidia.residentevil5.NvidiaGPGS")),
                element(
                    "activity",
                    mapOf("name" to "com.nvidia.residentevil5.UE3JavaDownloaderActivity"),
                    launcherFilter(),
                ),
            ),
        ),
    )

    private fun launcherFilter(): Node = element(
        "intent-filter",
        element("action", main()),
        element("category", mapOf("name" to "android.intent.category.LAUNCHER")),
    )

    private fun main(): Map<String, String> =
        mapOf("name" to "android.intent.action.MAIN")

    private class Node(
        val name: String,
        val attributes: Map<String, String>,
        val children: List<Node>,
    )

    private fun element(
        name: String,
        attributes: Map<String, String> = emptyMap(),
        vararg children: Node,
    ): Node = Node(name, attributes, children.toList())

    private fun element(name: String, vararg children: Node): Node =
        Node(name, emptyMap(), children.toList())

    /**
     * Encodes a manifest the way aapt does: one string pool, then a start chunk
     * and an end chunk per element, each attribute held as an index into the
     * pool. `enabled` is typed as a boolean, as a build tool compiles it.
     */
    private fun document(root: Node, utf8: Boolean = false): ByteArray {
        val strings = collect(root)
        val chunks = ByteArrayOutputStream()
        writeNode(chunks, root, strings)
        val pool = stringPool(strings, utf8)
        val out = ByteArrayOutputStream()
        out.i16(XML)
        out.i16(8)
        out.i32(8 + pool.size + chunks.size())
        out.writeBytes(pool)
        chunks.writeTo(out)
        return out.toByteArray()
    }

    private fun collect(root: Node): List<String> {
        val strings = ArrayList<String>()
        fun intern(value: String) {
            if (value !in strings) {
                strings += value
            }
        }
        fun walk(node: Node) {
            intern(node.name)
            for ((name, value) in node.attributes) {
                intern(name)
                if (name != "enabled") {
                    intern(value)
                }
            }
            node.children.forEach(::walk)
        }
        walk(root)
        return strings
    }

    private fun writeNode(out: ByteArrayOutputStream, node: Node, strings: List<String>) {
        val attributes = node.attributes.entries.toList()
        val body = ByteArrayOutputStream()
        body.i32(0)
        body.i32(-1)
        body.i32(-1)
        body.i32(strings.indexOf(node.name))
        body.i16(20)
        body.i16(20)
        body.i16(attributes.size)
        body.i16(0)
        body.i16(0)
        body.i16(0)
        for ((name, value) in attributes) {
            val boolean = name == "enabled"
            val data = if (boolean) {
                if (value == "true") -1 else 0
            } else {
                strings.indexOf(value)
            }
            body.i32(-1)
            body.i32(strings.indexOf(name))
            body.i32(if (boolean) -1 else data)
            body.i16(8)
            body.write(0)
            body.write(if (boolean) TYPE_INT_BOOLEAN else TYPE_STRING)
            body.i32(data)
        }
        out.chunk(START_ELEMENT, body)

        node.children.forEach { writeNode(out, it, strings) }

        val end = ByteArrayOutputStream()
        end.i32(0)
        end.i32(-1)
        end.i32(-1)
        end.i32(strings.indexOf(node.name))
        out.chunk(END_ELEMENT, end)
    }

    private fun stringPool(strings: List<String>, utf8: Boolean): ByteArray {
        val data = ByteArrayOutputStream()
        val offsets = IntArray(strings.size)
        strings.forEachIndexed { index, value ->
            offsets[index] = data.size()
            if (utf8) {
                val encoded = value.toByteArray(Charsets.UTF_8)
                data.write(value.length)
                data.write(encoded.size)
                data.writeBytes(encoded)
                data.write(0)
            } else {
                data.i16(value.length)
                data.writeBytes(value.toByteArray(Charsets.UTF_16LE))
                data.i16(0)
            }
        }
        val header = 28
        val dataStart = header + strings.size * 4
        val out = ByteArrayOutputStream()
        out.i16(STRING_POOL)
        out.i16(header)
        out.i32(dataStart + data.size())
        out.i32(strings.size)
        out.i32(0)
        out.i32(if (utf8) UTF8_FLAG else 0)
        out.i32(dataStart)
        out.i32(0)
        offsets.forEach { out.i32(it) }
        data.writeTo(out)
        return out.toByteArray()
    }

    private fun ByteArrayOutputStream.chunk(type: Int, body: ByteArrayOutputStream) {
        i16(type)
        i16(16)
        // The sixteen-byte header covers the eight bytes written here plus the
        // line number and comment that open the body.
        i32(8 + body.size())
        body.writeTo(this)
    }

    private fun ByteArrayOutputStream.i16(value: Int) {
        write(value and 0xFF)
        write((value ushr 8) and 0xFF)
    }

    private fun ByteArrayOutputStream.i32(value: Int) {
        i16(value and 0xFFFF)
        i16((value ushr 16) and 0xFFFF)
    }

    private companion object {
        const val XML = 0x0003
        const val STRING_POOL = 0x0001
        const val START_ELEMENT = 0x0102
        const val END_ELEMENT = 0x0103
        const val TYPE_STRING = 0x03
        const val TYPE_INT_BOOLEAN = 0x12
        const val UTF8_FLAG = 0x100
    }
}
