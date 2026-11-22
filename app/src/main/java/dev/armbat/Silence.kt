package dev.armbat

import java.io.File

/**
 * Entries that keep an archive's own protection SDK out of its way.
 *
 * One file per package on the shared storage, one entry per line:
 *
 *     libstub                                          a library, or a native
 *                                                      method, the container
 *                                                      answers instead of runs
 *     flag:android.support.v4.security.coM5#true=true  a static field of the
 *                                                      archive kept at a value
 *
 * It exists because an archive may carry a protection SDK that decides inside
 * its own code whether the process is the device it expects, and ends the
 * process from there when it is not: an emulated device is exactly what such an
 * SDK refuses, and no container fix changes that, because the SDK is working.
 * These entries are how the user tells the container to leave that SDK out of
 * the picture -- its native half is not called, and the flag its engine raises
 * to say "I am running" is held raised, which is the state a process on an
 * accepted device is in.
 *
 * A file always wins over the shipped default, even an empty one: that is how a
 * default is turned off without deleting the file back into existence.
 */
object Silence {

    /**
     * Entries shipped for archives known to end the process on a container.
     * Data, not a branch in the emulator: the same mechanism the user edits.
     */
    private const val CRISIS_FIRE =
        "libstub\nflag:android.support.v4.security.coM5#true=true"

    private val DEFAULTS = mapOf(
        "com.crisisfire.cmge.classichug" to CRISIS_FIRE,
        "com.crisisfire.android.uc" to CRISIS_FIRE,
    )

    class Plan(
        /** Library or method names for [GuestNatives.silence], one per line. */
        val silenced: String,
        /** Static fields to hold, "class#field=value". */
        val holds: List<String>,
    )

    enum class State {
        /** No file; the shipped entries apply. */
        DEFAULT,

        /** The file exists and holds at least one entry. */
        EDITED,

        /** No entries in force: no file and no shipped default, or an empty file. */
        OFF,
    }

    fun file(packageName: String): File = File(GuestStorage.root(), "silence/$packageName")

    /**
     * The entries in force: the file if it exists, else the shipped default.
     * An empty file is a decision of its own ("no entries"), not a missing one.
     */
    fun effective(packageName: String): String? {
        val marker = file(packageName)
        if (marker.isFile) {
            return runCatching { marker.readText() }.getOrNull() ?: ""
        }
        return DEFAULTS[packageName]
    }

    fun state(packageName: String): State {
        val marker = file(packageName)
        if (marker.isFile) {
            val text = runCatching { marker.readText() }.getOrNull()
            return if (text.isNullOrBlank()) State.OFF else State.EDITED
        }
        return if (DEFAULTS.containsKey(packageName)) State.DEFAULT else State.OFF
    }

    fun plan(packageName: String): Plan? = parse(effective(packageName) ?: return null)

    fun parse(text: String): Plan? {
        val silenced = StringBuilder()
        val holds = ArrayList<String>()
        for (line in text.lineSequence()) {
            val entry = line.trim()
            if (entry.isEmpty() || entry.startsWith("#")) {
                continue
            }
            if (entry.startsWith("flag:")) {
                holds.add(entry.removePrefix("flag:").trim())
            } else {
                silenced.append(entry).append('\n')
            }
        }
        if (silenced.isEmpty() && holds.isEmpty()) {
            return null
        }
        return Plan(silenced.toString().trim(), holds)
    }

    /** Writes the file as given; an empty text turns the entries off. */
    fun save(packageName: String, text: String) {
        val marker = file(packageName)
        runCatching {
            marker.parentFile?.mkdirs()
            marker.writeText(text)
        }
    }

    /** Drops the file, which returns the package to its shipped default, if any. */
    fun reset(packageName: String) {
        runCatching { file(packageName).delete() }
    }
}
