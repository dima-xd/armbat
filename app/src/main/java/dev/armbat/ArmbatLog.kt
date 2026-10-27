package dev.armbat

import android.content.Context
import android.util.Log
import java.io.File

/**
 * The application's one log.
 *
 * Everything goes through the native logger: it timestamps a record, keeps the
 * thread name, writes it to a file and mirrors it to logcat. The Java side has
 * no writer of its own because two clocks and two formats in one session are
 * exactly what makes a log hard to read.
 *
 * The file lives in the shared tree (`/sdcard/armbat/logs/current.log`) so it
 * can be pulled with adb or a file manager without root, and falls back to the
 * private directory while all-files access is missing. Records older than the
 * rotation window are dropped by the native side.
 *
 * The levels mirror [armbat::LogLevel] in `runtime/include/armbat/log.h`.
 */
object ArmbatLog {

    const val TRACE = 0
    const val DEBUG = 1
    const val INFO = 2
    const val WARN = 3
    const val ERROR = 4
    const val OFF = 5

    /** The levels the settings screen offers, loudest first: the useful default is near the top. */
    val LEVELS = listOf(OFF, ERROR, WARN, INFO, DEBUG, TRACE)

    private const val PREFS = "armbat.log"
    private const val KEY_LEVEL = "level"

    private var context: Context? = null

    /** Set once [init] has run; a second call is a no-op. */
    private var installed = false

    /** Whether the native writer accepted the directory and owns the file. */
    private var native = false

    /**
     * Starts the native logger. Called from [ArmbatApp] before any activity, so
     * a launch is in the file from its first line.
     *
     * Logging keeps working when this fails: records then go straight to logcat
     * through the platform, which is what the application did before.
     */
    fun init(context: Context) {
        if (installed) {
            return
        }
        installed = true
        this.context = context.applicationContext
        native = runCatching {
            System.loadLibrary("armbat_jni")
            nativeStart(directory(context).absolutePath)
        }.getOrDefault(false)
        if (!native) {
            Log.w("Armbat", "log: file logging unavailable, logcat only")
            return
        }
        // The stored choice wins over the built-in default. `debug.armbat.log`
        // stays the developer override when nothing was ever chosen here.
        val stored = preferences()?.getInt(KEY_LEVEL, -1) ?: -1
        if (stored in LEVELS) {
            nativeSetLevel(stored)
        }
    }

    /** The level in force, for the settings screen and for the summary line. */
    fun level(): Int {
        if (native) {
            val value = runCatching { nativeLevel() }.getOrDefault(INFO)
            return if (value in LEVELS) value else INFO
        }
        return preferences()?.getInt(KEY_LEVEL, INFO) ?: INFO
    }

    /**
     * Applies a level to the file and to logcat, and remembers it for the next
     * start. It is a setting, not a debugging switch: a user who wants quiet
     * logs should not have to plug the device in to get them.
     */
    fun setLevel(level: Int) {
        if (level !in LEVELS) {
            return
        }
        preferences()?.edit()?.putInt(KEY_LEVEL, level)?.apply()
        if (native) {
            runCatching { nativeSetLevel(level) }
        }
        i("Armbat", "log level: ${nameOf(level)}")
    }

    private fun preferences(): android.content.SharedPreferences? =
        context?.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    /**
     * The directory a run is written to. The shared tree is the useful place:
     * `adb pull /sdcard/armbat/logs` needs no root and no `run-as`. The
     * all-files permission is not trusted as a flag -- it is checked by writing
     * a probe file, because the flag and the mount can disagree right after the
     * grant -- and the private directory is the fallback.
     */
    private fun directory(context: Context): File {
        val candidates = listOfNotNull(
            runCatching { File(GuestStorage.root(), "logs") }.getOrNull(),
            File(context.filesDir, "logs"),
        )
        for (candidate in candidates) {
            val usable = runCatching {
                candidate.mkdirs()
                File(candidate, ".probe").writeText("")
            }.isSuccess
            if (usable) {
                return candidate
            }
        }
        return candidates.last()
    }

    /** Where the records are going, or an empty string while the file is off. */
    fun path(): String = runCatching { nativePath() }.getOrDefault("")

    /** Blocks until everything queued is on disk; used before a crash exits. */
    fun flush() {
        if (native) {
            runCatching { nativeFlush() }
        }
    }

    fun v(tag: String, message: String) = write(TRACE, tag, message)

    fun d(tag: String, message: String) = write(DEBUG, tag, message)

    fun i(tag: String, message: String) = write(INFO, tag, message)

    fun w(tag: String, message: String) = write(WARN, tag, message)

    fun w(tag: String, message: String, error: Throwable) = write(WARN, tag, describe(message, error))

    fun e(tag: String, message: String) = write(ERROR, tag, message)

    fun e(tag: String, message: String, error: Throwable) = write(ERROR, tag, describe(message, error))

    private fun describe(message: String, error: Throwable): String {
        val text = StringBuilder(message)
        text.append("\n").append(Log.getStackTraceString(error))
        return text.toString()
    }

    private fun write(level: Int, tag: String, message: String) {
        if (message.isEmpty()) {
            return
        }
        if (native) {
            runCatching { nativeWrite(level, tag, message) }
            return
        }
        Log.println(priority(level), tag, message)
    }

    private fun priority(level: Int): Int = when (level) {
        TRACE -> Log.VERBOSE
        DEBUG -> Log.DEBUG
        WARN -> Log.WARN
        ERROR -> Log.ASSERT
        else -> Log.INFO
    }

    /** The name that goes into the log itself; the screen shows localized text. */
    private fun nameOf(level: Int): String = when (level) {
        TRACE -> "trace"
        DEBUG -> "debug"
        WARN -> "warn"
        ERROR -> "error"
        OFF -> "off"
        else -> "info"
    }

    private external fun nativeStart(directory: String): Boolean

    private external fun nativeWrite(level: Int, tag: String, message: String)

    private external fun nativeFlush()

    private external fun nativePath(): String

    private external fun nativeSetLevel(level: Int)

    private external fun nativeLevel(): Int
}
