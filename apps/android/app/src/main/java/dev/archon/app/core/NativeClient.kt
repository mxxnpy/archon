package dev.archon.app.core

/** Thin Kotlin face of the C++ core. Events arrive on a native network thread. */
class NativeClient(controllerId: ByteArray, name: String, private val sink: (CoreEvent) -> Unit) : AutoCloseable {
    private var handle: Long = nativeCreate(controllerId, name.toByteArray())

    fun pair(host: String, port: Int) = nativePair(handle, host.toByteArray(), port)
    fun resume(host: String, port: Int, record: ByteArray): Boolean =
        nativeResume(handle, host.toByteArray(), port, record)
    /** Tries each address in turn; on CONNECTED the status message is "host|port". */
    fun resume(addresses: List<Pair<String, Int>>, record: ByteArray): Boolean =
        nativeResumeMulti(handle, addresses.map { it.first.toByteArray() }.toTypedArray(),
            addresses.map { it.second }.toIntArray(), record)
    fun submitCode(code: String) = nativeSubmitCode(handle, code.toByteArray())
    fun disconnect() = nativeDisconnect(handle)
    fun setVolume(endpoint: Boolean, id: String, volume: Int) =
        nativeSetVolume(handle, endpoint, id.toByteArray(), volume.coerceIn(0, MAX_VOLUME))
    fun setMute(endpoint: Boolean, id: String, muted: Boolean) = nativeSetMute(handle, endpoint, id.toByteArray(), muted)
    fun requestIcon(key: String) = nativeRequestIcon(handle, key.toByteArray())
    fun refresh() = nativeRefresh(handle)
    fun setMeters(on: Boolean) = nativeSetMeters(handle, on)
    /** Send an app to an output ("" = follow the PC's default). */
    fun setRoute(source: String, endpoint: String) = nativeSetRoute(handle, source.toByteArray(), endpoint.toByteArray())
    /** Mute everything on the PC / undo. Sent once the link is up if called right after resume(). */
    fun panicMute(on: Boolean) = nativePanic(handle, on)
    /** Listen to the PC on this phone; audio then goes to [audioSink]. */
    fun streamStart(mutePc: Boolean): Boolean = nativeStreamStart(handle, mutePc)
    fun streamStop() = nativeStreamStop(handle)
    /** 1.5: save a profile (new = from the mixer as it is; capture = take the mixer now). */
    fun saveProfile(p: dev.archon.app.ui.ProfileInfo, capture: Boolean) = nativeSaveProfile(
        handle, (if (p.isNew) "" else p.id).toByteArray(), p.name.toByteArray(),
        (if (p.automatic) PROFILE_AUTOMATIC else 0) or (if (p.restore) PROFILE_RESTORE else 0),
        p.days, p.startMin, p.endMin, p.network.toByteArray(), p.outputId.toByteArray(), p.outputName.toByteArray(), capture)
    fun deleteProfile(id: String) = nativeDeleteProfile(handle, id.toByteArray())
    fun applyProfile(id: String) = nativeApplyProfile(handle, id.toByteArray())
    /** Opus packets of the PC's audio (network thread, ~50/s). */
    @Volatile var audioSink: ((ByteArray?, Long, Boolean) -> Unit)? = null

    override fun close() {
        val h = handle
        handle = 0
        if (h != 0L) nativeDestroy(h)
    }

    // Called from native code (see archon_jni.cpp; signature must match).
    @Suppress("unused")
    private fun onEvent(
        type: Int, status: Int, error: Int, message: ByteArray?, id: ByteArray?, endpointId: ByteArray?,
        name: ByteArray?, iconKey: ByteArray?, pid: Int, volume: Int, flags: Int, meter: Int, data: ByteArray?,
        route: ByteArray?, features: Int, group: ByteArray?, detail: ByteArray?,
    ) {
        sink(
            CoreEvent(
                type = type, status = status, error = error, message = message.s(), id = id.s(),
                endpointId = endpointId.s(), name = name.s(), iconKey = iconKey.s(), pid = pid, volume = volume,
                muted = flags and 1 != 0, isDefault = flags and 2 != 0, playing = flags and 4 != 0,
                systemSounds = flags and 8 != 0, meter = meter, data = data,
                input = flags and 16 != 0, isDefaultInput = flags and 32 != 0, panic = flags and 64 != 0,
                active = flags and 128 != 0,
                route = route.s(), features = features,
                tab = flags and 256 != 0, group = group.s(), detail = detail.s(),
            )
        )
    }

    // Called from native code for ARC_EV_PROFILE (see archon_jni.cpp; signature must match).
    @Suppress("unused")
    private fun onProfile(
        id: ByteArray?, name: ByteArray?, flags: Int, days: Int, start: Int, end: Int, network: ByteArray?,
        outputId: ByteArray?, outputName: ByteArray?, nEndpoints: Int, nApps: Int,
    ) {
        val p = dev.archon.app.ui.ProfileInfo(
            id = id.s(), name = name.s(), active = flags and PROFILE_ACTIVE != 0, automatic = flags and PROFILE_AUTOMATIC != 0,
            restore = flags and PROFILE_RESTORE != 0, days = days, startMin = start, endMin = end, network = network.s(),
            outputId = outputId.s(), outputName = outputName.s(), nEndpoints = nEndpoints, nApps = nApps,
        )
        sink(CoreEvent(CoreEvent.PROFILE, 0, 0, "", p.id, "", p.name, "", 0, 0, false, false, false, false, 0, null,
            profile = p))
    }

    // Called from native code for ARC_EV_AUDIO (see archon_jni.cpp; signature must match).
    @Suppress("unused")
    private fun onAudio(data: ByteArray?, seq: Long, lost: Boolean) {
        audioSink?.invoke(data, seq, lost)
    }

    private fun ByteArray?.s(): String = this?.toString(Charsets.UTF_8) ?: ""

    private external fun nativeCreate(id: ByteArray, name: ByteArray): Long
    private external fun nativeDestroy(handle: Long)
    private external fun nativePair(handle: Long, host: ByteArray, port: Int)
    private external fun nativeResume(handle: Long, host: ByteArray, port: Int, record: ByteArray): Boolean
    private external fun nativeSubmitCode(handle: Long, code: ByteArray)
    private external fun nativeResumeMulti(handle: Long, hosts: Array<ByteArray>, ports: IntArray, record: ByteArray): Boolean
    private external fun nativeDisconnect(handle: Long)
    private external fun nativeSetVolume(handle: Long, endpoint: Boolean, id: ByteArray, volume: Int)
    private external fun nativeSetMute(handle: Long, endpoint: Boolean, id: ByteArray, muted: Boolean)
    private external fun nativeRequestIcon(handle: Long, key: ByteArray)
    private external fun nativeRefresh(handle: Long)
    private external fun nativeSetMeters(handle: Long, on: Boolean)
    private external fun nativeSetRoute(handle: Long, source: ByteArray, endpoint: ByteArray)
    private external fun nativePanic(handle: Long, on: Boolean)
    private external fun nativeStreamStart(handle: Long, mutePc: Boolean): Boolean
    private external fun nativeStreamStop(handle: Long)
    private external fun nativeSaveProfile(handle: Long, id: ByteArray, name: ByteArray, flags: Int, days: Int, start: Int,
        end: Int, network: ByteArray, outputId: ByteArray, outputName: ByteArray, capture: Boolean)
    private external fun nativeDeleteProfile(handle: Long, id: ByteArray)
    private external fun nativeApplyProfile(handle: Long, id: ByteArray)

    companion object {
        const val MAX_VOLUME = 10000
        const val FEATURE_INPUTS = 1
        const val FEATURE_ROUTING = 2
        const val FEATURE_PANIC = 4
        const val FEATURE_STREAM = 8
        const val FEATURE_TABS = 16
        const val FEATURE_PROFILES = 32
        const val PROFILE_ACTIVE = 1
        const val PROFILE_AUTOMATIC = 2
        const val PROFILE_RESTORE = 4
        const val DEFAULT_PORT = 47321
        const val SERVICE_TYPE = "_archon._tcp."

        init { System.loadLibrary("archon_jni") }

        @JvmStatic private external fun nativeGenerateId(): ByteArray
        @JvmStatic private external fun nativeRecordInfo(record: ByteArray): ByteArray?

        fun generateId(): ByteArray = nativeGenerateId()

        /** (agent id hex, PC name) stored inside a pairing record, or null if corrupt. */
        fun recordInfo(record: ByteArray): Pair<String, String>? {
            val raw = nativeRecordInfo(record) ?: return null
            if (raw.size < 16) return null
            val hex = raw.copyOfRange(0, 16).joinToString("") { "%02x".format(it) }
            return hex to raw.copyOfRange(16, raw.size).toString(Charsets.UTF_8)
        }
    }
}

/** Mirrors arc_event (archon.h). */
data class CoreEvent(
    val type: Int, val status: Int, val error: Int, val message: String, val id: String, val endpointId: String,
    val name: String, val iconKey: String, val pid: Int, val volume: Int, val muted: Boolean, val isDefault: Boolean,
    val playing: Boolean, val systemSounds: Boolean, val meter: Int, val data: ByteArray?,
    // 1.2
    val input: Boolean = false, val isDefaultInput: Boolean = false, val panic: Boolean = false,
    val route: String = "", val features: Int = 0,
    val active: Boolean = false,  // 1.3 STREAM_STATE
    // 1.4: a browser tab (SOURCE)
    val tab: Boolean = false, val group: String = "", val detail: String = "",
    val profile: dev.archon.app.ui.ProfileInfo? = null,  // 1.5 PROFILE
) {
    companion object {
        const val STATUS = 0
        const val PAIRED = 1
        const val STATE_RESET = 2
        const val ENDPOINT = 3
        const val ENDPOINT_REMOVED = 4
        const val SOURCE = 5
        const val SOURCE_REMOVED = 6
        const val STATE_SYNCED = 7
        const val ICON = 8
        const val METERS = 9
        const val AGENT_INFO = 10
        const val STREAM_STATE = 11
        const val PROFILE = 13
        const val PROFILE_REMOVED = 14
        const val CONTEXT = 15
    }
}

/** arc_status */
enum class LinkStatus { IDLE, CONNECTING, AWAITING_CODE, VERIFYING, CONNECTED, RECONNECTING, FAILED;
    companion object { fun of(v: Int) = entries.getOrElse(v) { FAILED } }
}

/** arc_error */
object CoreError {
    const val NONE = 0
    const val PROTOCOL = 1
    const val UNSUPPORTED_VERSION = 2
    const val UNKNOWN_CONTROLLER = 3
    const val WRONG_CODE = 4
    const val PAIRING_LOCKED = 5
    const val PAIRING_REJECTED = 6
    const val BUSY = 7
    const val AUTH_FAILED = 8
    const val TIMEOUT = 9
    const val REVOKED = 10
    const val CONNECT_FAILED = 100
    const val CONNECTION_LOST = 101
    /** Unverified refusal (could be spoofed): ask the user, never auto-forget. */
    const val NOT_RECOGNIZED = 102
    const val WRONG_AGENT = 103

    /** Only an authenticated revocation (inside the encrypted channel) forgets the pairing. */
    fun invalidatesPairing(e: Int) = e == REVOKED
}
