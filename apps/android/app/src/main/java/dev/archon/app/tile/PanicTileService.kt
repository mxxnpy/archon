package dev.archon.app.tile

import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.service.quicksettings.Tile
import android.service.quicksettings.TileService
import android.widget.Toast
import dev.archon.app.R
import dev.archon.app.core.CoreEvent
import dev.archon.app.core.LinkStatus
import dev.archon.app.core.NativeClient
import dev.archon.app.data.PairingStore
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

/**
 * Quick-settings tile "Silenciar PC": toggles the panic mute on the last PC
 * used, without opening the app. It reads the current state from the PC
 * (AGENT_INFO) and sends the opposite, so it is a real toggle.
 */
class PanicTileService : TileService() {
    private val main = Handler(Looper.getMainLooper())

    override fun onStartListening() {
        qsTile?.apply {
            label = getString(R.string.tile_label)
            if (state == Tile.STATE_UNAVAILABLE) state = Tile.STATE_INACTIVE
            updateTile()
        }
    }

    override fun onClick() {
        val tile = qsTile ?: return
        tile.state = Tile.STATE_UNAVAILABLE  // working
        tile.updateTile()
        Thread {
            val result = PanicToggle.run(applicationContext)
            main.post {
                qsTile?.apply {
                    state = if (result == PanicToggle.Result.MUTED) Tile.STATE_ACTIVE else Tile.STATE_INACTIVE
                    updateTile()
                }
                val msg = when (result) {
                    PanicToggle.Result.MUTED -> R.string.tile_muted
                    PanicToggle.Result.UNMUTED -> R.string.tile_unmuted
                    PanicToggle.Result.NO_PC -> R.string.tile_no_pc
                    PanicToggle.Result.FAILED -> R.string.tile_failed
                }
                Toast.makeText(applicationContext, msg, Toast.LENGTH_SHORT).show()
            }
        }.start()
    }
}

/** One short connection: resume, read the panic state, send the opposite, disconnect. */
object PanicToggle {
    enum class Result { MUTED, UNMUTED, NO_PC, FAILED }

    fun run(ctx: Context): Result {
        val store = PairingStore(ctx)
        val pcs = store.all()
        if (pcs.isEmpty()) return Result.NO_PC
        val pc = pcs.firstOrNull { it.agentId == store.lastPc } ?: pcs.first()
        val done = CountDownLatch(1)
        val sent = AtomicBoolean(false)
        val result = AtomicReference(Result.FAILED)
        lateinit var client: NativeClient
        client = NativeClient(store.controllerId, deviceName(ctx)) { e ->
            when (e.type) {
                CoreEvent.AGENT_INFO -> if (sent.compareAndSet(false, true)) {
                    val on = !e.panic
                    client.panicMute(on)  // channel is up: sent right away
                    result.set(if (on) Result.MUTED else Result.UNMUTED)
                    Thread {
                        Thread.sleep(500)  // let the command leave before closing
                        done.countDown()
                    }.start()
                }
                CoreEvent.STATUS -> if (LinkStatus.of(e.status) == LinkStatus.FAILED) done.countDown()
            }
        }
        try {
            client.resume(pc.host, pc.port, pc.record)
            done.await(6, TimeUnit.SECONDS)
        } finally {
            client.close()  // joins the network thread: never called from it
        }
        return if (sent.get()) result.get() else Result.FAILED
    }

    private fun deviceName(ctx: Context): String =
        runCatching { Settings.Global.getString(ctx.contentResolver, Settings.Global.DEVICE_NAME) }.getOrNull()
            ?.takeIf { it.isNotBlank() } ?: "${Build.MANUFACTURER} ${Build.MODEL}".trim()
}
