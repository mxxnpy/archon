package dev.archon.app.ui

import androidx.compose.ui.graphics.ImageBitmap
import kotlinx.coroutines.flow.StateFlow

/** What the screens need. Implemented by [ArchonViewModel]; faked in screenshot tests. */
interface ArchonUi {
    val screen: StateFlow<Screen>
    val link: StateFlow<LinkState>
    val icons: StateFlow<Map<String, ImageBitmap>>
    val levels: StateFlow<Map<Int, Float>>
    val selectedEndpoint: StateFlow<String?>
    val notice: StateFlow<Notice?>
    val searching: StateFlow<Boolean>
    val computers: StateFlow<List<PcRow>>

    fun open(target: Target)
    fun openAddress(input: String): Boolean
    fun back()
    fun forget(target: Target)
    fun dismissNotice()
    fun submitCode(code: String)
    fun retryPairing()
    fun reconnect()
    fun repair(target: Target)
    fun selectEndpoint(id: String)
    fun setVolume(endpoint: Boolean, id: String, volume: Int)
    fun setMute(endpoint: Boolean, id: String, muted: Boolean)
    fun setMetersVisible(visible: Boolean)
    /** Send an app to an output ("" = follow the PC's default). */
    fun setRoute(source: String, endpoint: String)
    /** Mute everything on the PC / undo. */
    fun togglePanic()
}
