package dev.archon.app.data

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Build
import android.os.Handler
import android.os.Looper
import dev.archon.app.core.NativeClient
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.net.Inet4Address
import java.util.ArrayDeque

/** A computer currently announcing itself on the local network. */
data class DiscoveredPc(val agentId: String, val name: String, val host: String, val port: Int)

/**
 * mDNS/DNS-SD browsing for `_archon._tcp` with NsdManager. Resolution is
 * serialized: older Android versions fail concurrent resolves.
 */
class Discovery(context: Context) {
    private val nsd = context.getSystemService(Context.NSD_SERVICE) as NsdManager
    private val main = Handler(Looper.getMainLooper())
    private val _found = MutableStateFlow<Map<String, DiscoveredPc>>(emptyMap())
    val found: StateFlow<Map<String, DiscoveredPc>> = _found.asStateFlow()
    private val _searching = MutableStateFlow(false)
    val searching: StateFlow<Boolean> = _searching.asStateFlow()

    private var listener: NsdManager.DiscoveryListener? = null
    private val queue = ArrayDeque<NsdServiceInfo>()
    private var resolving = false
    private val byServiceName = mutableMapOf<String, String>()  // service name -> agent id

    /** Ids of paired PCs: always admitted, so fake announcements filling the cap can't hide them. */
    @Volatile var pinned: Set<String> = emptySet()

    fun start() {
        if (listener != null) return
        val l = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(serviceType: String) = main.post { _searching.value = true }.let { }
            override fun onDiscoveryStopped(serviceType: String) = main.post { _searching.value = false }.let { }
            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) =
                main.post { _searching.value = false; listener = null }.let { }
            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) {}
            override fun onServiceFound(info: NsdServiceInfo) = main.post { enqueue(info) }.let { }
            override fun onServiceLost(info: NsdServiceInfo) = main.post {
                byServiceName.remove(info.serviceName)?.let { id -> _found.value = _found.value - id }
            }.let { }
        }
        listener = l
        runCatching { nsd.discoverServices(NativeClient.SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, l) }
            .onFailure { listener = null }
    }

    fun stop() {
        listener?.let { runCatching { nsd.stopServiceDiscovery(it) } }
        listener = null
        queue.clear()
        _searching.value = false
    }

    private fun enqueue(info: NsdServiceInfo) {
        // Bounded: a flood of announcements must not grow memory without limit.
        // The TXT id is only known after resolving, so dedupe by service name.
        if (queue.size >= MAX_QUEUE || queue.any { it.serviceName == info.serviceName }) return
        queue.add(info)
        pump()
    }

    @Suppress("DEPRECATION")  // resolveService works on every API level we support
    private fun pump() {
        if (resolving) return
        val next = queue.poll() ?: return
        resolving = true
        nsd.resolveService(next, object : NsdManager.ResolveListener {
            override fun onResolveFailed(info: NsdServiceInfo, errorCode: Int) = main.post {
                resolving = false
                pump()
            }.let { }

            override fun onServiceResolved(info: NsdServiceInfo) = main.post {
                resolving = false
                accept(info)
                pump()
            }.let { }
        })
    }

    private fun accept(info: NsdServiceInfo) {
        val id = info.attributes["id"]?.toString(Charsets.UTF_8)?.lowercase() ?: return
        if (id.length != 32) return
        val host = hostOf(info) ?: return
        // Bounded and sanitized: mDNS is unauthenticated, anyone can advertise.
        if (id !in pinned && !_found.value.containsKey(id) && _found.value.size >= MAX_FOUND) return
        val name = cleanName(info.serviceName).ifBlank { host }
        byServiceName[info.serviceName] = id
        _found.value = _found.value + (id to DiscoveredPc(id, name, host, info.port))
    }

    internal companion object {
        const val MAX_FOUND = 32
        const val MAX_QUEUE = 64

        /**
         * Drops control and format characters (Unicode Cc and Cf: bidi overrides
         * and isolates, U+061C, zero-width marks) and keeps at most 64 code
         * points, never splitting a surrogate pair. Same rule as iOS.
         */
        fun cleanName(raw: String): String {
            val sb = StringBuilder()
            var count = 0
            var i = 0
            while (i < raw.length && count < 64) {
                val cp = raw.codePointAt(i)
                i += Character.charCount(cp)
                val type = Character.getType(cp)
                if (type == Character.CONTROL.toInt() || type == Character.FORMAT.toInt()) continue
                sb.appendCodePoint(cp)
                count++
            }
            return sb.toString()
        }
    }

    private fun hostOf(info: NsdServiceInfo): String? {
        val addrs = if (Build.VERSION.SDK_INT >= 34) info.hostAddresses else listOfNotNull(@Suppress("DEPRECATION") info.host)
        // Prefer IPv4: link-local IPv6 needs a scope id that is easy to lose.
        val a = addrs.firstOrNull { it is Inet4Address } ?: addrs.firstOrNull() ?: return null
        return a.hostAddress
    }
}
