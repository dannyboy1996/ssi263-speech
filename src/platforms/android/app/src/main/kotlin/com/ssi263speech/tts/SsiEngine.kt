// The one engine in this process, shared by the TTS service and the settings screen's preview: opens the native
// side on the staged unit files, lists the voices, and owns the engine for one utterance at a time.  A stop from any
// thread reaches the render in progress (nativeStop); the synthesis thread then cancels, so the unit drops what it
// has not spoken and the next utterance starts clean -- as the speech-dispatcher module does after STOP.
package com.ssi263speech.tts

import android.content.Context
import android.util.Log
import java.util.Locale

object SsiEngine {
    data class VoiceInfo(val index: Int, val name: String, val label: String, val locale: Locale)

    private val ALL = listOf(
        VoiceInfo(SsiNative.ENGLISH, "en-US-braillelite", "Braille Lite 2000 (English)", Locale("en", "US")),
        VoiceInfo(SsiNative.SPANISH, "es-ES-braillelite", "Braille Lite 2000 (español)", Locale("es", "ES")))

    private val lock = Any()
    @Volatile private var opened = false

    /** Stage the unit's files and open the native side on them.  False when the APK carries no English unit. */
    fun open(ctx: Context): Boolean {
        if (opened) return true
        synchronized(lock) {
            if (opened) return true
            if (!SsiData.stage(ctx)) { Log.e("SsiEngine", "no English unit in the APK"); return false }
            opened = SsiNative.nativeOpen(SsiData.dir(ctx).absolutePath)
            return opened
        }
    }

    /** The voices this APK can speak with: English, and Spanish when its files were built in. */
    fun voices(ctx: Context): List<VoiceInfo> = ALL.filter { SsiData.ships(ctx, it.index) }

    fun voiceByName(ctx: Context, name: String?): VoiceInfo? = voices(ctx).firstOrNull { it.name == name }

    fun voiceFor(ctx: Context, index: Int): VoiceInfo = voices(ctx).firstOrNull { it.index == index } ?: ALL[0]

    fun spanish(ctx: Context): Boolean = SsiData.ships(ctx, SsiNative.SPANISH)

    /** Own the engine for one utterance: every native call but a stop happens inside. */
    fun <T> withEngine(block: () -> T): T = synchronized(lock) { block() }

    /** Boot the chosen voice's unit ahead of the first request, so that request does not wait for it. */
    fun warmUp(ctx: Context) {
        if (!open(ctx)) return
        val s = SsiSettings.snapshot(ctx)
        withEngine {
            configure(s)
            val rc = SsiNative.nativeLoad(voiceFor(ctx, s.voice).index)
            if (rc != 0) Log.w("SsiEngine", "warm-up: ${SsiNative.nativeError()}")
        }
    }

    /** Inside withEngine: the unit's boot settings (a change reboots the units on their next use). */
    fun configure(s: SsiSettings.Snapshot) =
        SsiNative.nativeConfigure(s.sampleRate, if (s.inflection) 1 else 0, s.whine)

    /** Inside withEngine: begin an utterance.  0 audio to pull, 1 nothing to say, negative on failure. */
    fun start(voice: VoiceInfo, text: String, s: SsiSettings.Snapshot, requestRate: Int, requestPitch: Int): Int =
        SsiNative.nativeStart(voice.index, text.toByteArray(Charsets.UTF_8), s.rate, s.pitch, s.tone, s.volume,
                              if (s.shortPauses) 1 else 0, requestRate, requestPitch)

    /** Inside withEngine: the next PCM bytes; 0 at the end, -2 when stopped. */
    fun pull(out: ByteArray): Int = SsiNative.nativePull(out)

    /** Inside withEngine, after a stop or an abandoned utterance. */
    fun cancel() = SsiNative.nativeCancel()

    /** Any thread. */
    fun stop() { if (opened) SsiNative.nativeStop() }

    fun sampleRate(): Int = if (opened) SsiNative.nativeSampleRate() else 22050
}
