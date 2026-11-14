package dev.archon.app.audio

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import dev.archon.app.MainActivity
import dev.archon.app.R

/**
 * Keeps the app alive while the PC's audio plays with the screen off (a
 * "media playback" foreground service, as Android requires). It holds no
 * logic: the ViewModel owns the connection and the player.
 */
class ListenService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopRequest?.invoke()  // the ViewModel stops the stream, then stops us
            return START_NOT_STICKY
        }
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL, getString(R.string.listen_channel), NotificationManager.IMPORTANCE_LOW),
        )
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE,
        )
        val stop = PendingIntent.getService(
            this, 1, Intent(this, ListenService::class.java).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE,
        )
        val n: Notification = Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_tile_panic)
            .setContentTitle(getString(R.string.listen_active))
            .setContentText(intent?.getStringExtra(EXTRA_PC) ?: "")
            .setContentIntent(open)
            .setOngoing(true)
            .addAction(Notification.Action.Builder(null, getString(R.string.listen_stop), stop).build())
            .build()
        if (Build.VERSION.SDK_INT >= 29) startForeground(ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        else startForeground(ID, n)
        return START_NOT_STICKY
    }

    companion object {
        private const val CHANNEL = "listen"
        private const val ID = 7
        private const val ACTION_STOP = "dev.archon.app.STOP_LISTENING"
        private const val EXTRA_PC = "pc"

        /** Set by the ViewModel (same process): the notification's "Stop" button. */
        @Volatile var stopRequest: (() -> Unit)? = null

        fun start(ctx: Context, pcName: String) {
            ctx.startForegroundService(Intent(ctx, ListenService::class.java).putExtra(EXTRA_PC, pcName))
        }

        fun stop(ctx: Context) {
            ctx.stopService(Intent(ctx, ListenService::class.java))
        }
    }
}
