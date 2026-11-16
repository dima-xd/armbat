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
		GuestDexRuntime.initialize(this)
        installCrashHandler()
        installExitRecorder()
        GuestPendingIntents.host = this
    }

    /**
     * Records a process exit that the guest asked for.
     *
     * A guest is a whole application, and an application may end its own
     * process: a login SDK finishes its activity and calls System.exit so that
     * the game starts over with the account it has just received. From outside
     * that is indistinguishable from a crash -- the window disappears
     * mid-session and the log simply stops -- and the two need different
     * answers. A shutdown hook sees every exit that goes through the VM, and
     * the caller is still parked in Runtime.exit while the hooks run, so the
     * stack names the code that asked. A process taken by a signal leaves no
     * line, which is the answer in that case.
     */
    private fun installExitRecorder() {
        Runtime.getRuntime().addShutdownHook(Thread({
            runCatching {
                val exiting = Thread.getAllStackTraces().filterValues { frames ->
                    frames.any { it.className == "java.lang.Shutdown" }
                }
                if (exiting.isEmpty()) {
                    ArmbatLog.i("Armbat", "host exit: no Java caller")
                }
                for ((thread, frames) in exiting) {
                    val callers = frames.take(8)
                        .joinToString(" ") { "${it.className}.${it.methodName}" }
                    ArmbatLog.i("Armbat", "host exit: ${thread.name} -> $callers")
                }
            }
            ArmbatLog.flush()
        }, "armbat-exit"))
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
