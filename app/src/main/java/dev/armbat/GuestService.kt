package dev.armbat

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder

/**
 * Keeps the process in the foreground for as long as a guest runs.
 *
 * A guest is a whole application with its own threads, its own window surface
 * and its own GL context, but to the platform it is just one process that
 * belongs to this activity. The moment the activity is covered -- the player
 * switches to another app for a second, the launcher comes up, the screen is
 * locked -- the process becomes a cached one, and a cached process that holds
 * several hundred megabytes is the first thing both the platform and the OEM's
 * memory manager kill. A game must not be destroyed because its window is
 * behind another one, so the work is announced as foreground work.
 *
 * Nothing here talks to the guest: the service exists only to give the process
 * the importance a running application has.
 */
class GuestService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        val manager = getSystemService(NotificationManager::class.java)
        val channel = NotificationChannel(
            CHANNEL,
            getString(R.string.guest_channel_name),
            NotificationManager.IMPORTANCE_LOW,
        )
        channel.setShowBadge(false)
        channel.lockscreenVisibility = Notification.VISIBILITY_SECRET
        manager.createNotificationChannel(channel)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val notification = Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_guest)
            .setContentTitle("Armbat")
            .setContentText(getString(R.string.guest_notification_text))
            .setOngoing(true)
            .setShowWhen(false)
            .build()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
        } else {
            startForeground(ID, notification)
        }
        // A guest cannot be resumed after the process is gone: restarting the
        // service alone would leave a notification for an application that is
        // no longer running.
        return START_NOT_STICKY
    }

    companion object {
        private const val CHANNEL = "guest"
        private const val ID = 1

        fun start(context: Context) {
            val intent = Intent(context, GuestService::class.java)
            try {
                context.startForegroundService(intent)
            } catch (error: IllegalStateException) {
                // Starting a foreground service from the background is refused;
                // the guest runs anyway, it just loses the protection.
                ArmbatLog.w("Armbat", "guest service not started: ${error.message}")
            }
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, GuestService::class.java))
        }
    }
}
