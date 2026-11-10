package dev.armbat

/**
 * The decisions the updater makes, kept apart from the I/O that feeds them.
 *
 * A release is identified by the version code in the asset name the workflow
 * writes (`armbat-0.1.2-3.apk`), so two builds of the same version are ordered
 * without trusting the tag text. A nightly carries no version of its own: its
 * asset name never changes and its version name is the one of the last release,
 * so the only thing that tells one nightly from another is the commit recorded
 * in the release notes.
 */
internal object UpdateRules {

    private val assetPattern = Regex("""armbat-(.+)-(\d+)(?:-unsigned)?\.apk""")
    private val buildLine = Regex("""(?m)^[\s\-*]*build:\s*([0-9a-fA-F]{7,40})\s*$""")

    /** `0.1.2` and `3` out of `armbat-0.1.2-3.apk`, or null for any other name. */
    fun versionOf(assetName: String): Pair<String, Int>? {
        val match = assetPattern.matchEntire(assetName) ?: return null
        val code = match.groupValues[2].toIntOrNull() ?: return null
        return match.groupValues[1] to code
    }

    /** The version code an asset name carries, or null when it carries none. */
    fun versionCode(assetName: String): Int? = versionOf(assetName)?.second

    /** Positive when [a] is newer than [b], zero when they are the same version. */
    fun compareVersions(a: String, b: String): Int {
        val left = parts(a)
        val right = parts(b)
        for (index in 0 until maxOf(left.size, right.size)) {
            val x = left.getOrNull(index) ?: 0
            val y = right.getOrNull(index) ?: 0
            if (x != y) return x.compareTo(y)
        }
        return 0
    }

    /** The commit a nightly was built from, as the workflow writes it in the notes. */
    fun buildId(notes: String): String? =
        buildLine.find(notes)?.groupValues?.get(1)?.lowercase()

    /**
     * Whether two ids name the same commit. CI passes the full hash, while the
     * notes and a hand-cut build may carry an abbreviation of it.
     */
    fun sameBuild(local: String, remote: String): Boolean {
        if (local.isEmpty() || remote.isEmpty()) return false
        return local.startsWith(remote, ignoreCase = true) ||
            remote.startsWith(local, ignoreCase = true)
    }

    private fun parts(version: String): List<Int> =
        version.trim()
            .removePrefix("v")
            .split('.', '-', '+')
            .map { it.takeWhile(Char::isDigit).toIntOrNull() ?: 0 }
}
