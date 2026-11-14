package dev.archon.app.audio

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Process
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * Plays the PC's audio on the phone ("Ouvir no celular"). Opus packets
 * (48 kHz stereo, 20 ms) from the core are decoded with the platform's Opus
 * decoder (MediaCodec) and written to a low-latency AudioTrack on a
 * high-priority audio thread. Packets arrive on the core's network thread.
 */
class StreamPlayer {
    private class Packet(val data: ByteArray?, val lost: Boolean)

    private val queue = LinkedBlockingQueue<Packet>()
    @Volatile private var running = false
    private var worker: Thread? = null

    fun start() {
        if (running) return
        queue.clear()
        running = true
        worker = Thread({ run() }, "archon-audio").apply { start() }
    }

    fun stop() {
        running = false
        worker?.interrupt()
        worker?.join(1000)
        worker = null
        queue.clear()
    }

    /** Core network thread: one packet in order, or a lost frame. */
    fun feed(packet: ByteArray?, lost: Boolean) {
        if (!running) return
        if (queue.size > MAX_QUEUED) return  // > 200 ms behind: drop to catch up
        queue.offer(Packet(packet, lost))
    }

    private fun run() {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
        val codec = runCatching { MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_AUDIO_OPUS) }.getOrNull() ?: return
        val format = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_OPUS, RATE, 2).apply {
            setByteBuffer("csd-0", opusHead())
            setByteBuffer("csd-1", leLong(PRE_SKIP_NS))
            setByteBuffer("csd-2", leLong(SEEK_PREROLL_NS))
        }
        val minBuf = AudioTrack.getMinBufferSize(RATE, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT)
        val track = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build(),
            )
            .setAudioFormat(
                AudioFormat.Builder().setSampleRate(RATE).setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build(),
            )
            .setBufferSizeInBytes(maxOf(minBuf, FRAME_BYTES * 4))
            .setTransferMode(AudioTrack.MODE_STREAM)
            .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
            .build()
        val silence = ByteArray(FRAME_BYTES)
        val info = MediaCodec.BufferInfo()
        var pts = 0L
        var buffered = 0
        try {
            codec.configure(format, null, null, 0)
            codec.start()
            while (running) {
                val p = queue.poll(100, TimeUnit.MILLISECONDS) ?: continue
                if (p.lost || p.data == null || p.data.isEmpty()) {
                    track.write(silence, 0, silence.size)  // a gap keeps its 20 ms
                } else {
                    val idx = codec.dequeueInputBuffer(10_000)
                    if (idx >= 0) {
                        codec.getInputBuffer(idx)?.apply { clear(); put(p.data) }
                        codec.queueInputBuffer(idx, 0, p.data.size, pts, 0)
                        pts += 20_000
                    }
                    while (true) {
                        val out = codec.dequeueOutputBuffer(info, 2_000)
                        if (out < 0) break
                        val buf = codec.getOutputBuffer(out)
                        if (buf != null && info.size > 0) {
                            val bytes = ByteArray(info.size)
                            buf.position(info.offset)
                            buf.get(bytes, 0, info.size)
                            track.write(bytes, 0, bytes.size)  // blocks: paces the decoder
                        }
                        codec.releaseOutputBuffer(out, false)
                    }
                }
                if (track.playState != AudioTrack.PLAYSTATE_PLAYING && ++buffered >= START_FRAMES) track.play()
            }
        } catch (_: InterruptedException) {
        } catch (_: Exception) {
        } finally {
            runCatching { track.stop() }
            track.release()
            runCatching { codec.stop() }
            codec.release()
        }
    }

    private companion object {
        const val RATE = 48_000
        const val FRAME_BYTES = 960 * 2 * 2  // 20 ms, stereo, 16-bit
        const val START_FRAMES = 3           // 60 ms buffered before playback starts
        const val MAX_QUEUED = 10
        const val PRE_SKIP = 312
        const val PRE_SKIP_NS = PRE_SKIP * 1_000_000_000L / RATE
        const val SEEK_PREROLL_NS = 80_000_000L

        /** RFC 7845 identification header the platform decoder needs. */
        fun opusHead(): ByteBuffer = ByteBuffer.allocate(19).order(ByteOrder.LITTLE_ENDIAN).apply {
            put("OpusHead".toByteArray(Charsets.US_ASCII))
            put(1)                     // version
            put(2)                     // channels
            putShort(PRE_SKIP.toShort())
            putInt(RATE)               // input sample rate
            putShort(0)                // output gain
            put(0)                     // channel mapping family
            flip()
        }

        fun leLong(v: Long): ByteBuffer = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(v).apply { flip() }
    }
}
