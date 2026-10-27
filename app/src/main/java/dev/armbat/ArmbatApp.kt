package dev.armbat

import android.app.Application

/**
 * Starts the log before anything else in the process.
 *
 * An activity cannot do it: the container's native library is loaded from a
 * worker thread during a launch, and a record written before the file exists
 * only reaches logcat. Starting here means the file begins with the process, so
 * the first thing a reader sees is the header rather than a launch halfway
 * through.
 *
 * A Java crash is also a crash of the whole run, and ART's default handler
 * prints it to stderr, which a zygote child does not keep. It goes through the
 * same file, right after the native records that led to it.
 */
class ArmbatApp : Application() {

    override fun onCreate() {
        super.onCreate()
        ArmbatLog.init(this)
        installCrashHandler()
        GuestPendingIntents.host = this
    }

    private fun installCrashHandler() {
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, error ->
            ArmbatLog.e("Java", "uncaught in ${thread.name}", error)
            ArmbatLog.flush()
            if (previous != null) {
                previous.uncaughtException(thread, error)
            } else {
                android.os.Process.killProcess(android.os.Process.myPid())
            }
        }
    }
}
