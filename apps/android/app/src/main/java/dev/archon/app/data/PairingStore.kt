package dev.archon.app.data

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import dev.archon.app.core.NativeClient
import org.json.JSONObject
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/** A computer this phone has paired with. */
data class PairedPc(
    val agentId: String,
    val name: String,
    val host: String,
    val port: Int,
    val record: ByteArray,
)

/**
 * Persists pairing records encrypted with an AES-GCM key that never leaves the
 * Android Keystore (hardware-backed where available). Host/port are kept
 * alongside so a paired PC can be reached even when mDNS is blocked.
 */
class PairingStore(context: Context) {
    private val prefs = context.getSharedPreferences("archon_pairings", Context.MODE_PRIVATE)

    /** Stable random id of this phone, created on first use. */
    val controllerId: ByteArray by lazy {
        prefs.getString(KEY_CONTROLLER_ID, null)?.let { Base64.decode(it, Base64.NO_WRAP) }?.takeIf { it.size == 16 }
            ?: NativeClient.generateId().also {
                prefs.edit().putString(KEY_CONTROLLER_ID, Base64.encodeToString(it, Base64.NO_WRAP)).apply()
            }
    }

    fun all(): List<PairedPc> = prefs.all.keys.filter { it.startsWith(PREFIX) }.mapNotNull { load(it.removePrefix(PREFIX)) }
        .sortedBy { it.name.lowercase() }

    fun load(agentId: String): PairedPc? {
        val raw = prefs.getString(PREFIX + agentId, null) ?: return null
        return runCatching {
            val o = JSONObject(raw)
            val record = decrypt(Base64.decode(o.getString("r"), Base64.NO_WRAP))
            PairedPc(agentId, o.getString("n"), o.getString("h"), o.getInt("p"), record)
        }.getOrNull()
    }

    fun save(record: ByteArray, host: String, port: Int): PairedPc? {
        val (agentId, name) = NativeClient.recordInfo(record) ?: return null
        val o = JSONObject()
            .put("n", name)
            .put("h", host)
            .put("p", port)
            .put("r", Base64.encodeToString(encrypt(record), Base64.NO_WRAP))
        prefs.edit().putString(PREFIX + agentId, o.toString()).apply()
        return PairedPc(agentId, name, host, port, record)
    }

    /** Updates the last known address (the PC's IP can change via DHCP). */
    fun updateAddress(agentId: String, host: String, port: Int) {
        val raw = prefs.getString(PREFIX + agentId, null) ?: return
        val o = JSONObject(raw)
        if (o.optString("h") == host && o.optInt("p") == port) return
        prefs.edit().putString(PREFIX + agentId, o.put("h", host).put("p", port).toString()).apply()
    }

    fun forget(agentId: String) = prefs.edit().remove(PREFIX + agentId).apply()

    /** Agent id of the PC last connected to (used by the quick-settings tile). */
    var lastPc: String?
        get() = prefs.getString(KEY_LAST_PC, null)
        set(v) = prefs.edit().putString(KEY_LAST_PC, v).apply()

    private fun key(): SecretKey {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (ks.getEntry(KEY_ALIAS, null) as? KeyStore.SecretKeyEntry)?.let { return it.secretKey }
        val gen = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        gen.init(
            KeyGenParameterSpec.Builder(KEY_ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build()
        )
        return gen.generateKey()
    }

    private fun encrypt(plain: ByteArray): ByteArray {
        val c = Cipher.getInstance("AES/GCM/NoPadding")
        c.init(Cipher.ENCRYPT_MODE, key())
        return c.iv + c.doFinal(plain)  // 12-byte IV || ciphertext+tag
    }

    private fun decrypt(blob: ByteArray): ByteArray {
        val c = Cipher.getInstance("AES/GCM/NoPadding")
        c.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, blob, 0, 12))
        return c.doFinal(blob, 12, blob.size - 12)
    }

    private companion object {
        const val PREFIX = "pc."
        const val KEY_CONTROLLER_ID = "controller_id"
        const val KEY_LAST_PC = "last_pc"  // not under PREFIX: all() ignores it
        const val KEY_ALIAS = "archon.pairing"
    }
}
