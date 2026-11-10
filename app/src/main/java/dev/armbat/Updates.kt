package dev.armbat

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.Settings
import androidx.core.content.FileProvider
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import org.json.JSONObject

/**
 * Where a newer build of the container comes from.
 *
 * The two channels are published from the same repository but are not
 * interchangeable: a release follows the newest version tag, a nightly follows
 * the `nightly` pre-release, whose asset is overwritten on every push. An
 * installed APK therefore stays on the channel it was built for, and a phone
 * that deliberately runs nightlies is never walked back to a release rule.
 *
 * The check runs on the launcher's own network stack, not through the guest: it
 * is the container asking about itself, and a guest must never see this traffic.
 */
object Updates {

    const val RELEASE = "release"
    const val NIGHTLY = "nightly"

    private const val REPO = "dima-xd/armbat"
    private const val API = "https://api.github.com/repos/$REPO/releases"
    private const val NIGHTLY_TAG = "nightly"
    private const val TIMEOUT_MS = 15_000

    /** How long an answer is reused before GitHub is asked again. */
    private const val FRESH_MS = 10 * 60 * 1000L

    /** The channel this APK was built for; set by the workflow, see build.gradle.kts. */
    val channel: String
        get() = BuildConfig.CHANNEL

    /**
     * Whether this package can update itself at all. The debug build installs
     * beside the release one under another package name; an offer to replace
     * `dev.armbat` from there would only end in a signature error, so it is not
     * made.
     */
    val enabled: Boolean
        get() = BuildConfig.APPLICATION_ID == "dev.armbat"

    data class Update(
        /** The version to show: a release's tag, or a nightly's commit. */
        val version: String,
        val assetName: String,
        val url: String,
        val size: Long,
        /** `sha256:...` when the API reports the asset hash. */
        val digest: String?,
    )

    sealed interface Status {
        data object Idle : Status
        data object Checking : Status
        data object UpToDate : Status
        data class Available(val update: Update) : Status
        data class Failed(val reason: String) : Status
    }

    private var cachedAt = 0L
    private var cached: Status? = null
    private var dismissed: String? = null

    /**
     * One question at a time: two checks launched together would both miss the
     * cache and spend two of the hour's requests on the same answer.
     */
    private val asking = Mutex()

    /**
     * Asks GitHub for the newest build of this channel.
     *
     * A result younger than [FRESH_MS] is returned without a request: the
     * launcher recomposes on every return from a guest, and a check on each of
     * those would spend the unauthenticated rate limit for nothing.
     */
    suspend fun check(fresh: Boolean): Status = asking.withLock {
        val last = cached
        if (!fresh && last != null && System.currentTimeMillis() - cachedAt < FRESH_MS) {
            return@withLock last
        }
        val result = withContext(Dispatchers.IO) {
            runCatching { query() }.getOrElse { error ->
                ArmbatLog.w("Armbat", "update check failed: ${error.message}")
                Status.Failed(error.message ?: error.javaClass.simpleName)
            }
        }
        ArmbatLog.i("Armbat", "update check: $channel -> $result")
        cached = result
        cachedAt = System.currentTimeMillis()
        result
    }

    /** Whether an offer of [update] has not been turned down in this process yet. */
    fun shouldAnnounce(update: Update): Boolean = update.version != dismissed

    fun dismiss(update: Update) {
        dismissed = update.version
    }

    /**
     * Downloads the new build into the cache and verifies it against the size
     * and hash the API reported, so a truncated or substituted file never
     * reaches the installer.
     *
     * A verified copy from an earlier attempt is reused: the download is tens of
     * megabytes, and the platform sends the user to its own switch to allow
     * installs before the file is opened, which used to throw the work away and
     * start it again from the first byte.
     */
    suspend fun download(
        context: Context,
        update: Update,
        onProgress: (Int) -> Unit,
    ): File = withContext(Dispatchers.IO) {
        val directory = File(context.cacheDir, "updates").apply { mkdirs() }
        val target = File(directory, update.assetName)
        if (target.isFile) {
            // A file from an earlier offer is only good when it is the exact
            // asset: a newer nightly keeps the name and changes the hash.
            if (runCatching { verify(target, update) }.isSuccess) {
                onProgress(100)
                return@withContext target
            }
            target.delete()
        }
        directory.listFiles()?.forEach { if (it != target) it.delete() }
        val connection = open(update.url, api = false)
        try {
            if (connection.responseCode != HttpURLConnection.HTTP_OK) {
                throw IOException("HTTP ${connection.responseCode}")
            }
            val total = connection.contentLengthLong.takeIf { it > 0 } ?: update.size
            var read = 0L
            var reported = -1
            connection.inputStream.use { input ->
                target.outputStream().use { output ->
                    val buffer = ByteArray(64 * 1024)
                    while (true) {
                        val count = input.read(buffer)
                        if (count < 0) break
                        output.write(buffer, 0, count)
                        read += count
                        val percent = if (total > 0L) ((read * 100) / total).toInt() else 0
                        if (percent != reported) {
                            reported = percent
                            onProgress(percent)
                        }
                    }
                }
            }
            verify(target, update)
            target
        } catch (error: Throwable) {
            target.delete()
            throw error
        } finally {
            connection.disconnect()
        }
    }

    /**
     * Hands the downloaded archive to the platform installer. Returns false when
     * the platform has not been told to allow installs from this application;
     * the caller sends the user to that switch and offers the same file again.
     */
    fun install(context: Context, file: File): Boolean {
        if (!canInstall(context)) {
            return false
        }
        val uri = FileProvider.getUriForFile(
            context,
            "${BuildConfig.APPLICATION_ID}.files",
            file,
        )
        val intent = Intent(Intent.ACTION_VIEW).apply {
            setDataAndType(uri, "application/vnd.android.package-archive")
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        context.startActivity(intent)
        return true
    }

    /** Whether the platform lets this application open its installer at all. */
    fun canInstall(context: Context): Boolean =
        context.packageManager.canRequestPackageInstalls()

    /** Opens the system switch that lets this application install packages. */
    fun requestInstallPermission(context: Context) {
        val intent = Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES)
            .setData(Uri.parse("package:${BuildConfig.APPLICATION_ID}"))
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        runCatching { context.startActivity(intent) }.onFailure {
            ArmbatLog.w("Armbat", "no unknown-sources screen: ${it.message}")
        }
    }

    private fun query(): Status {
        val release = fetch(if (channel == NIGHTLY) "tags/$NIGHTLY_TAG" else "latest")
        val asset = release.assets.firstOrNull { it.name.endsWith(".apk") }
            ?: return Status.Failed("no apk asset in ${release.tag}")
        return if (channel == NIGHTLY) nightly(release, asset) else stable(release, asset)
    }

    private fun stable(release: Release, asset: Asset): Status {
        val version = release.tag.removePrefix("v")
        val remoteCode = UpdateRules.versionCode(asset.name)
        val newer = if (remoteCode != null) {
            remoteCode > BuildConfig.VERSION_CODE
        } else {
            UpdateRules.compareVersions(version, BuildConfig.VERSION_NAME) > 0
        }
        if (!newer) return Status.UpToDate
        return Status.Available(
            Update(
                version = version,
                assetName = asset.name,
                url = asset.url,
                size = asset.size,
                digest = asset.digest,
            ),
        )
    }

    /**
     * A nightly has no version of its own, and its asset name is the same
     * forever, so the commit in the notes is what makes one build different from
     * the next.
     *
     * A nightly published before the notes carried that line is offered all the
     * same: it is newer than anything this check could have been built from, and
     * the one copy it costs the user carries the marker.
     */
    private fun nightly(release: Release, asset: Asset): Status {
        val remote = UpdateRules.buildId(release.notes)
        if (remote != null && UpdateRules.sameBuild(BuildConfig.BUILD_ID, remote)) {
            return Status.UpToDate
        }
        return Status.Available(
            Update(
                version = remote?.take(7) ?: release.tag,
                assetName = asset.name,
                url = asset.url,
                size = asset.size,
                digest = asset.digest,
            ),
        )
    }

    private class Asset(
        val name: String,
        val url: String,
        val size: Long,
        val digest: String?,
    )

    private class Release(val tag: String, val notes: String, val assets: List<Asset>)

    private fun fetch(path: String): Release {
        val connection = open("$API/$path")
        try {
            if (connection.responseCode != HttpURLConnection.HTTP_OK) {
                throw IOException("HTTP ${connection.responseCode}")
            }
            val body = connection.inputStream.bufferedReader().use { it.readText() }
            val json = JSONObject(body)
            val assets = json.optJSONArray("assets").let { array ->
                if (array == null) {
                    emptyList()
                } else {
                    (0 until array.length()).mapNotNull { index ->
                        val asset = array.optJSONObject(index) ?: return@mapNotNull null
                        Asset(
                            name = asset.optString("name"),
                            url = asset.optString("browser_download_url"),
                            size = asset.optLong("size"),
                            // Older responses carry no digest; the download is
                            // then checked by size alone.
                            digest = asset.optString("digest")
                                .takeIf { it.startsWith("sha256:") },
                        )
                    }
                }
            }
            return Release(
                tag = json.optString("tag_name"),
                notes = json.optString("body"),
                assets = assets,
            )
        } finally {
            connection.disconnect()
        }
    }

    private fun open(url: String, api: Boolean = true): HttpURLConnection =
        (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = TIMEOUT_MS
            readTimeout = TIMEOUT_MS
            instanceFollowRedirects = true
            // GitHub answers a request without a user agent with 403.
            setRequestProperty("User-Agent", "Armbat/${BuildConfig.VERSION_NAME}")
            if (api) {
                setRequestProperty("Accept", "application/vnd.github+json")
            }
        }

    private fun verify(file: File, update: Update) {
        if (update.size > 0 && file.length() != update.size) {
            throw IOException("size ${file.length()}, expected ${update.size}")
        }
        val expected = update.digest?.substringAfter(':')?.takeIf { it.isNotEmpty() } ?: return
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buffer = ByteArray(64 * 1024)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                digest.update(buffer, 0, count)
            }
        }
        val actual = digest.digest().joinToString("") { "%02x".format(it) }
        if (!actual.equals(expected, ignoreCase = true)) {
            throw IOException("digest $actual, expected $expected")
        }
    }
}
