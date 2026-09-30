// The settings screen's "Speak": the same engine path as the TTS service (the chosen voice, the app's settings, a
// request at 100%), rendered under the engine's lock -- the unit runs many times faster than real time -- and played
// after it is released, so a screen reader speaking through this engine meanwhile is not held up (as outspoken's
// preview).  The render is kept as last-render.wav in the app's files: a debug build's
// `adb shell run-as com.ssi263speech.tts cat files/last-render.wav` pulls it off the device.
package com.ssi263speech.tts

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.util.Log
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileOutputStream

object PreviewPlayer {
    @Volatile private var stopRequested = false

    /** Blocking: call off the UI thread.  -> samples spoken, or -1 when the engine could not speak. */
    fun speak(ctx: Context, text: String): Int {
        if (!SsiEngine.open(ctx)) return -1
        stopRequested = false
        val s = SsiSettings.snapshot(ctx)
        val voice = SsiEngine.voiceFor(ctx, s.voice)
        var rate = 22050
        val pcm = SsiEngine.withEngine {
            SsiEngine.configure(s)
            rate = SsiNative.nativeSampleRate()
            val started = SsiEngine.start(voice, text, s, 100, 100)
            if (started < 0) { Log.w("Preview", "start: ${SsiNative.nativeError()}"); return@withEngine null }
            val all = ByteArrayOutputStream()
            if (started == 1) return@withEngine all.toByteArray()
            val bytes = ByteArray(8192)
            var finished = false
            while (!stopRequested) {
                val n = SsiEngine.pull(bytes)
                if (n <= 0) { finished = n == 0; break }
                all.write(bytes, 0, n)
            }
            if (!finished) SsiEngine.cancel()
            all.toByteArray()
        } ?: return -1
        try { writeWav(File(ctx.filesDir, "last-render.wav"), pcm, rate) }
        catch (e: Exception) { Log.w("Preview", "wav not written", e) }
        Log.i("Preview", "${voice.name}: ${pcm.size / 2} samples at $rate Hz")
        if (pcm.isNotEmpty() && !stopRequested) play(pcm, rate)
        return pcm.size / 2
    }

    /** Any thread. */
    fun stop() {
        stopRequested = true
        SsiEngine.stop()
    }

    // MODE_STREAM, written in pieces so a stop cuts it short.  Accessibility usage, as a screen reader's speech.
    private fun play(pcm: ByteArray, rate: Int) {
        val min = AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT)
        val track = AudioTrack.Builder()
            .setAudioAttributes(AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_ASSISTANCE_ACCESSIBILITY)
                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build())
            .setAudioFormat(AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(rate)
                .setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build())
            .setBufferSizeInBytes(maxOf(min, rate))
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
        try {
            track.play()
            var off = 0
            while (off < pcm.size && !stopRequested) {
                val w = track.write(pcm, off, minOf(8192, pcm.size - off))   // blocks, pacing playback
                if (w <= 0) break
                off += w
            }
            // a write only queues the audio: wait for the playback head, with a deadline
            val frames = off / 2
            val deadline = android.os.SystemClock.elapsedRealtime() + frames * 1000L / rate + 2000
            while (!stopRequested && track.playbackHeadPosition < frames &&
                   android.os.SystemClock.elapsedRealtime() < deadline) Thread.sleep(20)
        } finally {
            try { track.stop() } catch (e: Exception) {}
            track.release()
        }
    }

    /** 16-bit mono WAV. */
    private fun writeWav(file: File, pcm: ByteArray, rate: Int) {
        FileOutputStream(file).use { o ->
            fun i32(v: Int) = o.write(byteArrayOf(v.toByte(), (v shr 8).toByte(), (v shr 16).toByte(), (v shr 24).toByte()))
            fun i16(v: Int) = o.write(byteArrayOf(v.toByte(), (v shr 8).toByte()))
            o.write("RIFF".toByteArray()); i32(36 + pcm.size); o.write("WAVE".toByteArray())
            o.write("fmt ".toByteArray()); i32(16); i16(1); i16(1); i32(rate); i32(rate * 2); i16(2); i16(16)
            o.write("data".toByteArray()); i32(pcm.size)
            o.write(pcm)
        }
    }
}
