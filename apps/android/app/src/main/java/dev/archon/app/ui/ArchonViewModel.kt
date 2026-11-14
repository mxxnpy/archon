package dev.archon.app.ui

import android.app.Application
import android.graphics.BitmapFactory
import android.os.Build
import android.provider.Settings
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import dev.archon.app.core.CoreError
import dev.archon.app.core.CoreEvent
import dev.archon.app.core.LinkStatus
import dev.archon.app.core.NativeClient
import dev.archon.app.data.Discovery
import dev.archon.app.data.PairedPc
import dev.archon.app.data.PairingStore
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update

/** Where to connect. agentId is null for a computer typed by address and not yet paired. */
data class Target(val agentId: String?, val name: String, val host: String, val port: Int)

data class Endpoint(
    val id: String, val name: String, val volume: Int, val muted: Boolean, val isDefault: Boolean, val meter: Int = 0,
)
data class Source(
    val id: String, val endpointId: String, val name: String, val iconKey: String, val volume: Int,
    val muted: Boolean, val playing: Boolean, val system: Boolean, val meter: Int = 0,
)

data class LinkState(
    val status: LinkStatus = LinkStatus.IDLE,
    val error: Int = CoreError.NONE,
    val synced: Boolean = false,
    val endpoints: Map<String, Endpoint> = emptyMap(),
    val sources: Map<String, Source> = emptyMap(),
)

/** One row on the computers screen. */
data class PcRow(val target: Target, val paired: Boolean, val online: Boolean)

sealed interface Screen {
    data object Computers : Screen
    data class Pairing(val target: Target) : Screen
    data class Mixer(val target: Target) : Screen
}

/** One-shot message shown on the computers screen (e.g. pairing removed on the PC). */
enum class Notice { PAIRING_REMOVED, CANNOT_CONNECT }

class ArchonViewModel(app: Application) : AndroidViewModel(app), ArchonUi {
    private val store = PairingStore(app)
    private val discovery = Discovery(app)
    private val client = NativeClient(store.controllerId, deviceName(app), ::onCoreEvent)

    private val _screen = MutableStateFlow<Screen>(Screen.Computers)
    override val screen: StateFlow<Screen> = _screen.asStateFlow()
    private val _link = MutableStateFlow(LinkState())
    override val link: StateFlow<LinkState> = _link.asStateFlow()
    private val _icons = MutableStateFlow<Map<String, ImageBitmap>>(emptyMap())
    override val icons: StateFlow<Map<String, ImageBitmap>> = _icons.asStateFlow()
    private val _selectedEndpoint = MutableStateFlow<String?>(null)
    override val selectedEndpoint: StateFlow<String?> = _selectedEndpoint.asStateFlow()
    private val _notice = MutableStateFlow<Notice?>(null)
    override val notice: StateFlow<Notice?> = _notice.asStateFlow()
    private val _paired = MutableStateFlow(store.all())

    /** Latest raw peak (0..1) per meter slot; the UI applies its own decay. */
    private val _levels = MutableStateFlow<Map<Int, Float>>(emptyMap())
    override val levels: StateFlow<Map<Int, Float>> = _levels.asStateFlow()
    override val searching: StateFlow<Boolean> = discovery.searching

    private val requestedIcons = java.util.Collections.synchronizedSet(mutableSetOf<String>())

    /** Paired PCs first (online ones on top), then unpaired PCs found on the network. */
    override val computers: StateFlow<List<PcRow>> = combine(_paired, discovery.found) { paired, found ->
        // Discovered addresses are only *candidates*: they are saved after an
        // authenticated resume succeeds (anyone can advertise a PC's public id).
        val rows = paired.map { p ->
            val live = found[p.agentId]
            PcRow(Target(p.agentId, live?.name ?: p.name, live?.host ?: p.host, live?.port ?: p.port), true, live != null)
        }.sortedByDescending { it.online }
        val others = found.values.filter { f -> paired.none { it.agentId == f.agentId } }
            .map { PcRow(Target(it.agentId, it.name, it.host, it.port), paired = false, online = true) }
        rows + others.sortedBy { it.target.name.lowercase() }
    }.stateIn(viewModelScope, SharingStarted.Eagerly, emptyList())

    // ---- lifecycle -------------------------------------------------------------

    fun onForeground() {
        discovery.start()
        (_screen.value as? Screen.Mixer)?.let { connect(it.target) }
    }

    /** Level meters only while the mixer is on screen (bandwidth and battery). */
    override fun setMetersVisible(visible: Boolean) {
        client.setMeters(visible)
        if (!visible) _levels.value = emptyMap()
    }

    fun onBackground() {
        discovery.stop()
        // Nothing runs in the background: no battery or data use while hidden.
        if (_screen.value is Screen.Mixer) client.disconnect()
    }

    override fun onCleared() {
        discovery.stop()
        client.close()
    }

    // ---- navigation ---------------------------------------------------------------

    override fun open(target: Target) {
        _notice.value = null
        val paired = target.agentId?.let { store.load(it) }
        if (paired != null) {
            _screen.value = Screen.Mixer(target)
            connect(target)
        } else {
            _screen.value = Screen.Pairing(target)
            resetLink()
            client.pair(target.host, target.port)
        }
    }

    override fun openAddress(input: String): Boolean {
        val (host, port) = parseAddress(input) ?: return false
        open(Target(null, host, host, port))
        return true
    }

    override fun back() {
        client.disconnect()
        resetLink()
        _screen.value = Screen.Computers
    }

    override fun forget(target: Target) {
        target.agentId?.let { store.forget(it) }
        _paired.value = store.all()
        back()
    }

    override fun dismissNotice() {
        _notice.value = null
    }

    // ---- pairing -------------------------------------------------------------------

    override fun submitCode(code: String) = client.submitCode(code)

    override fun retryPairing() {
        val t = (_screen.value as? Screen.Pairing)?.target ?: return
        resetLink()
        client.pair(t.host, t.port)
    }

    // ---- mixer ---------------------------------------------------------------------

    override fun selectEndpoint(id: String) {
        _selectedEndpoint.value = id
    }

    override fun setVolume(endpoint: Boolean, id: String, volume: Int) {
        // Optimistic: the PC echoes the real value back right after.
        updateVolume(endpoint, id, volume)
        client.setVolume(endpoint, id, volume)
    }

    override fun setMute(endpoint: Boolean, id: String, muted: Boolean) {
        _link.update { s ->
            if (endpoint) s.copy(endpoints = s.endpoints.mapValues { (k, v) -> if (k == id) v.copy(muted = muted) else v })
            else s.copy(sources = s.sources.mapValues { (k, v) -> if (k == id) v.copy(muted = muted) else v })
        }
        client.setMute(endpoint, id, muted)
    }

    override fun reconnect() {
        (_screen.value as? Screen.Mixer)?.let { connect(it.target) }
    }

    private fun updateVolume(endpoint: Boolean, id: String, volume: Int) = _link.update { s ->
        if (endpoint) s.copy(endpoints = s.endpoints.mapValues { (k, v) -> if (k == id) v.copy(volume = volume) else v })
        else s.copy(sources = s.sources.mapValues { (k, v) -> if (k == id) v.copy(volume = volume) else v })
    }

    private fun connect(target: Target) {
        val p: PairedPc = target.agentId?.let { store.load(it) } ?: run {
            _screen.value = Screen.Computers
            return
        }
        val addresses = listOf(target.host to target.port, p.host to p.port).distinct()
        connectedVia = null
        client.resume(addresses, p.record)
    }

    private fun resetLink() {
        _link.value = LinkState()
        _selectedEndpoint.value = null
    }

    // ---- core events (native network thread) ------------------------------------------

    private fun onCoreEvent(e: CoreEvent) {
        when (e.type) {
            CoreEvent.STATUS -> onStatus(LinkStatus.of(e.status), e.error, e.message)
            CoreEvent.PAIRED -> e.data?.let { record ->
                val pairing = _screen.value as? Screen.Pairing ?: return
                val saved = store.save(record, pairing.target.host, pairing.target.port) ?: return
                _paired.value = store.all()
                _screen.value = Screen.Mixer(Target(saved.agentId, saved.name, saved.host, saved.port))
            }
            CoreEvent.STATE_RESET -> _link.update { it.copy(endpoints = emptyMap(), sources = emptyMap(), synced = false) }
            CoreEvent.STATE_SYNCED -> _link.update { it.copy(synced = true) }
            CoreEvent.ENDPOINT -> _link.update {
                it.copy(endpoints = it.endpoints + (e.id to Endpoint(e.id, e.name, e.volume, e.muted, e.isDefault, e.meter)))
            }
            CoreEvent.ENDPOINT_REMOVED -> _link.update { it.copy(endpoints = it.endpoints - e.id) }
            CoreEvent.SOURCE -> {
                _link.update {
                    it.copy(sources = it.sources + (e.id to Source(e.id, e.endpointId, e.name, e.iconKey, e.volume,
                        e.muted, e.playing, e.systemSounds, e.meter)))
                }
                if (e.iconKey.isNotEmpty() && requestedIcons.add(e.iconKey)) client.requestIcon(e.iconKey)
            }
            CoreEvent.SOURCE_REMOVED -> _link.update { it.copy(sources = it.sources - e.id) }
            CoreEvent.METERS -> {
                val d = e.data ?: return
                _levels.update { old ->
                    val m = old.toMutableMap()
                    var i = 0
                    while (i + 2 < d.size) {
                        val slot = ((d[i].toInt() and 0xFF) shl 8) or (d[i + 1].toInt() and 0xFF)
                        m[slot] = (d[i + 2].toInt() and 0xFF) / 255f
                        i += 3
                    }
                    m
                }
            }
            CoreEvent.ICON -> {
                val png = e.data ?: return
                if (png.isEmpty()) return
                val bmp = BitmapFactory.decodeByteArray(png, 0, png.size) ?: return
                _icons.update { it + (e.iconKey to bmp.asImageBitmap()) }
            }
        }
    }

    private var connectedVia: String? = null

    /**
     * The PC didn't recognize us (unverified). Pair again with the last
     * *authenticated* address; the old key is replaced only when the new
     * pairing succeeds (store.save on PAIRED), never deleted up front.
     */
    override fun repair(target: Target) {
        val known = target.agentId?.let { store.load(it) }
        val t = if (known != null) target.copy(host = known.host, port = known.port) else target
        resetLink()
        _screen.value = Screen.Pairing(t)
        client.pair(t.host, t.port)
    }

    private fun onStatus(status: LinkStatus, error: Int, message: String) {
        _link.update { it.copy(status = status, error = error) }
        val screen = _screen.value
        if (status == LinkStatus.CONNECTED && screen is Screen.Mixer && message.contains('|') && connectedVia != message) {
            connectedVia = message
            val host = message.substringBeforeLast('|')
            val port = message.substringAfterLast('|').toIntOrNull()
            if (port != null) screen.target.agentId?.let { store.updateAddress(it, host, port) }
        }
        if (status != LinkStatus.FAILED) return
        if (screen is Screen.Mixer && CoreError.invalidatesPairing(error)) {
            screen.target.agentId?.let { store.forget(it) }
            _paired.value = store.all()
            resetLink()
            _notice.value = Notice.PAIRING_REMOVED
            _screen.value = Screen.Computers
        }
    }

    private companion object {
        fun deviceName(app: Application): String =
            runCatching { Settings.Global.getString(app.contentResolver, Settings.Global.DEVICE_NAME) }.getOrNull()
                ?.takeIf { it.isNotBlank() } ?: "${Build.MANUFACTURER} ${Build.MODEL}".trim()

        /** "192.168.0.10", "192.168.0.10:47321", "[fe80::1]:47321", "pc.local" */
        fun parseAddress(raw: String): Pair<String, Int>? {
            val s = raw.trim()
            if (s.isEmpty()) return null
            if (s.startsWith("[")) {
                val end = s.indexOf(']')
                if (end < 0) return null
                val port = s.substring(end + 1).removePrefix(":").toIntOrNull() ?: NativeClient.DEFAULT_PORT
                return s.substring(1, end) to port
            }
            val colon = s.lastIndexOf(':')
            if (colon > 0 && s.indexOf(':') == colon) {
                val port = s.substring(colon + 1).toIntOrNull()?.takeIf { it in 1..65535 } ?: return null
                return s.substring(0, colon) to port
            }
            return s to NativeClient.DEFAULT_PORT
        }
    }
}
