package dev.armbat

/**
 * The activity an archive declares as the one its icon starts.
 *
 * The package manager answers an archive's activities but drops their intent
 * filters, so the entry point cannot be recognised from PackageInfo alone. The
 * container started the first activity the manifest lists, and an archive may
 * list a helper before its entry point -- a store sign-in wrapper, a licence
 * check -- which then asks the platform for a service the guest does not have
 * and shows a dialog instead of the game.
 *
 * Only the manifest knows, and only in its compiled form, so that form is read
 * here: the string pool, the element names, and the handful of attributes that
 * identify a component. Anything malformed answers null and the caller keeps
 * its own fallback.
 */
internal object ApkManifest {
    private const val CHUNK_STRING_POOL = 0x0001
    private const val CHUNK_XML = 0x0003
    private const val CHUNK_START_ELEMENT = 0x0102
    private const val CHUNK_END_ELEMENT = 0x0103

    private const val TYPE_STRING = 0x03
    private const val TYPE_INT_BOOLEAN = 0x12

    private const val ACTION_MAIN = "android.intent.action.MAIN"
    private const val CATEGORY_LAUNCHER = "android.intent.category.LAUNCHER"
    private const val CATEGORY_LEANBACK = "android.intent.category.LEANBACK_LAUNCHER"

    /** The compiled manifest, at the archive's root. */
    const val FILE = "AndroidManifest.xml"

    /** The entry point declared by [manifest], or null when it declares none. */
    fun launcherActivity(manifest: ByteArray): String? =
        runCatching { parse(Bin(manifest)) }.getOrNull()

    private fun parse(bin: Bin): String? {
        require(bin.u16(0) == CHUNK_XML)
        val end = minOf(bin.size, bin.u32(4))
        var strings: List<String>? = null
        var offset = 8
        var packageName: String? = null
        val candidates = ArrayList<Candidate>()
        var current: Candidate? = null
        var filter: Filter? = null

        while (offset + 8 <= end) {
            val type = bin.u16(offset)
            val header = bin.u16(offset + 2)
            val size = bin.u32(offset + 4)
            require(header >= 8 && size >= header && offset + size <= end)
            when (type) {
                CHUNK_STRING_POOL -> strings = readStrings(bin, offset)

                CHUNK_START_ELEMENT -> {
                    val pool = strings ?: return null
                    val name = pool[bin.u32(offset + header + 4)]
                    val attributes = readAttributes(bin, offset, header, pool)
                    when (name) {
                        "manifest" -> packageName = attributes["package"]?.text

                        "activity", "activity-alias" -> if (current == null) {
                            current = Candidate(
                                name = resolve(
                                    attributes["name"]?.text ?: return null,
                                    packageName,
                                ),
                                alias = name == "activity-alias",
                                target = attributes["targetActivity"]?.text?.let {
                                    resolve(it, packageName)
                                },
                                enabled = attributes["enabled"]?.flag ?: true,
                            )
                        }

                        "intent-filter" -> current?.let {
                            filter = Filter().also(it.filters::add)
                        }

                        "action" -> attributes["name"]?.text?.let { filter?.actions?.add(it) }
                        "category" -> attributes["name"]?.text?.let { filter?.categories?.add(it) }
                    }
                }

                CHUNK_END_ELEMENT -> {
                    val pool = strings ?: return null
                    when (pool[bin.u32(offset + header + 4)]) {
                        "intent-filter" -> filter = null
                        "activity", "activity-alias" -> {
                            current?.let(candidates::add)
                            current = null
                        }
                    }
                }
            }
            offset += size
        }

        var chosen: Candidate? = null
        var chosenRank = Int.MAX_VALUE
        for (candidate in candidates) {
            val rank = rank(candidate) ?: continue
            if (rank < chosenRank) {
                chosen = candidate
                chosenRank = rank
            }
        }
        val entry = chosen ?: return null
        // An alias has no class of its own; the platform starts its target.
        return if (entry.alias) entry.target ?: entry.name else entry.name
    }

    /**
     * How well a component fits "the activity an icon starts": a launcher
     * category beats a leanback one (the archive runs on a phone here), and
     * both beat a bare MAIN filter, which no launcher shows.
     */
    private fun rank(candidate: Candidate): Int? {
        if (!candidate.enabled) {
            return null
        }
        var best: Int? = null
        for (filter in candidate.filters) {
            if (ACTION_MAIN !in filter.actions) {
                continue
            }
            val value = when {
                CATEGORY_LAUNCHER in filter.categories -> 0
                CATEGORY_LEANBACK in filter.categories -> 1
                else -> 2
            }
            val previous = best
            if (previous == null || value < previous) {
                best = value
            }
        }
        return best
    }

    /**
     * A component name as the platform stores it: a manifest may write it
     * relative to the package (".MainActivity"), and the answer is compared
     * against PackageInfo, which only has the expanded form.
     */
    private fun resolve(name: String, packageName: String?): String = when {
        packageName == null -> name
        name.startsWith(".") -> packageName + name
        '.' !in name -> "$packageName.$name"
        else -> name
    }

    private fun readStrings(bin: Bin, chunk: Int): List<String> {
        val header = bin.u16(chunk + 2)
        val count = bin.u32(chunk + 8)
        val flags = bin.u32(chunk + 16)
        val start = bin.u32(chunk + 20)
        require(count >= 0 && start >= 0)
        val offsets = chunk + header
        val base = chunk + start
        val utf8 = flags and UTF8_FLAG != 0
        val strings = ArrayList<String>(count)
        for (index in 0 until count) {
            strings += readString(bin, base + bin.u32(offsets + index * 4), utf8)
        }
        return strings
    }

    /**
     * One pool entry: a length prefix, then the characters, in whichever of the
     * two encodings the pool declares. The prefix is one unit, or two when its
     * high bit is set; the count is not needed, only the width of the text.
     */
    private fun readString(bin: Bin, at: Int, utf8: Boolean): String {
        if (!utf8) {
            var cursor = at
            var length = bin.u16(cursor)
            cursor += 2
            if (length and 0x8000 != 0) {
                length = ((length and 0x7FFF) shl 16) or bin.u16(cursor)
                cursor += 2
            }
            return bin.utf16(cursor, length)
        }
        var cursor = at
        var characters = bin.u8(cursor++)
        if (characters and 0x80 != 0) {
            characters = ((characters and 0x7F) shl 8) or bin.u8(cursor++)
        }
        require(characters >= 0)
        var length = bin.u8(cursor++)
        if (length and 0x80 != 0) {
            length = ((length and 0x7F) shl 8) or bin.u8(cursor++)
        }
        return bin.utf8(cursor, length)
    }

    private fun readAttributes(
        bin: Bin,
        chunk: Int,
        header: Int,
        strings: List<String>,
    ): Map<String, Attribute> {
        val attributeStart = bin.u16(chunk + header + 8)
        val attributeSize = bin.u16(chunk + header + 10)
        val count = bin.u16(chunk + header + 12)
        require(attributeSize >= 20)
        val attributes = HashMap<String, Attribute>(count)
        for (index in 0 until count) {
            val at = chunk + header + attributeStart + index * attributeSize
            val name = strings[bin.u32(at + 4)]
            val raw = bin.u32(at + 8)
            val valueType = bin.u8(at + 15)
            val data = bin.u32(at + 16)
            attributes[name] = when {
                // A string attribute keeps its text in the pool, named either
                // by the raw value or by the typed value.
                raw != -1 -> Attribute(strings[raw], null)
                valueType == TYPE_STRING -> Attribute(strings[data], null)
                valueType == TYPE_INT_BOOLEAN -> Attribute(null, data != 0)
                else -> Attribute(null, null)
            }
        }
        return attributes
    }

    private const val UTF8_FLAG = 0x100

    private class Attribute(val text: String?, val flag: Boolean?)

    private class Filter {
        val actions = HashSet<String>()
        val categories = HashSet<String>()
    }

    private class Candidate(
        val name: String,
        val alias: Boolean,
        val target: String?,
        val enabled: Boolean,
    ) {
        val filters = ArrayList<Filter>()
    }

    /** Bounds-checked little-endian reads; a broken manifest throws and is caught above. */
    private class Bin(private val bytes: ByteArray) {
        val size: Int get() = bytes.size

        fun u8(at: Int): Int {
            require(at in bytes.indices)
            return bytes[at].toInt() and 0xFF
        }

        fun u16(at: Int): Int {
            require(at >= 0 && at + 2 <= bytes.size)
            return u8(at) or (u8(at + 1) shl 8)
        }

        fun u32(at: Int): Int {
            require(at >= 0 && at + 4 <= bytes.size)
            return u8(at) or (u8(at + 1) shl 8) or (u8(at + 2) shl 16) or (u8(at + 3) shl 24)
        }

        fun utf8(at: Int, length: Int): String {
            require(at >= 0 && length >= 0 && at + length <= bytes.size)
            return String(bytes, at, length, Charsets.UTF_8)
        }

        fun utf16(at: Int, length: Int): String {
            require(at >= 0 && length >= 0 && at + length * 2 <= bytes.size)
            return String(bytes, at, length * 2, Charsets.UTF_16LE)
        }
    }
}
