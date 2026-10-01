// The system TTS service: TalkBack and every other app speak through here.  The request's rate and pitch go on top
// of the app's sliders (the C side maps them, ssa_map.h); its volume Android applies to its own audio track.  The
// audio streams to the framework block by block as the unit speaks, and a stop lands between blocks.
package com.ssi263speech.tts

import android.media.AudioFormat
import android.speech.tts.SynthesisCallback
import android.speech.tts.SynthesisRequest
import android.speech.tts.TextToSpeech
import android.speech.tts.TextToSpeechService
import android.speech.tts.Voice
import android.util.Log

class SsiTtsService : TextToSpeechService() {

    @Volatile private var stopRequested = false
    @Volatile private var stopAt = 0L

    override fun onCreate() {
        super.onCreate()
        // Boot the unit when the service binds, so the first utterance does not wait for it.
        Thread({ try { SsiEngine.warmUp(applicationContext) } catch (e: Throwable) {
            Log.e(TAG, "warm-up failed", e) } }, "ssi263-warmup").start()
    }

    private fun isSpanish(lang: String?) = lang == "spa" || lang == "es"
    private fun isEnglish(lang: String?) = lang == "eng" || lang == "en"

    // English: the Accent SA is built in.  Spanish: the Braille Lite's firmware is imported by the user; until it is,
    // the language is supported but its data missing.
    private fun availability(lang: String?, country: String?): Int = when {
        isEnglish(lang) -> if (!SsiEngine.english(this)) TextToSpeech.LANG_MISSING_DATA
                           else if (country == "USA" || country == "US") TextToSpeech.LANG_COUNTRY_AVAILABLE
                           else TextToSpeech.LANG_AVAILABLE
        isSpanish(lang) -> if (!SsiEngine.spanish(this)) TextToSpeech.LANG_MISSING_DATA
                           else if (country == "ESP" || country == "ES") TextToSpeech.LANG_COUNTRY_AVAILABLE
                           else TextToSpeech.LANG_AVAILABLE
        else -> TextToSpeech.LANG_NOT_SUPPORTED
    }

    override fun onIsLanguageAvailable(lang: String?, country: String?, variant: String?): Int =
        availability(lang, country)

    override fun onGetLanguage(): Array<String> =
        if (!SsiEngine.english(this) && SsiEngine.spanish(this)) arrayOf("spa", "ESP", "") else arrayOf("eng", "USA", "")

    override fun onLoadLanguage(lang: String?, country: String?, variant: String?): Int = availability(lang, country)

    override fun onGetVoices(): MutableList<Voice> = SsiEngine.voices(this).map {
        Voice(it.name, it.locale, Voice.QUALITY_NORMAL, Voice.LATENCY_NORMAL, false, emptySet())
    }.toMutableList()

    override fun onIsValidVoiceName(name: String?): Int =
        if (SsiEngine.voiceByName(this, name) != null) TextToSpeech.SUCCESS else TextToSpeech.ERROR

    override fun onLoadVoice(name: String?): Int = onIsValidVoiceName(name)

    override fun onGetDefaultVoiceNameFor(lang: String?, country: String?, variant: String?): String? {
        if (lang != null && availability(lang, country) < 0) return null
        if (isSpanish(lang)) return SsiEngine.voiceFor(this, SsiNative.SPANISH).name
        return SsiEngine.englishVoiceFor(this, SsiSettings.snapshot(this).voice).name
    }

    override fun onStop() {
        stopAt = android.os.SystemClock.elapsedRealtime()
        stopRequested = true
        SsiEngine.stop()
    }

    override fun onSynthesizeText(request: SynthesisRequest, callback: SynthesisCallback) {
        android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_AUDIO)
        val text = request.charSequenceText?.toString() ?: ""
        // No voice at all (an APK without the Accent SA's ROMs, no firmware imported): the voice data is not installed.
        if (!SsiEngine.open(this)) { callback.error(TextToSpeech.ERROR_NOT_INSTALLED_YET); return }
        val s = SsiSettings.snapshot(this)

        // A request in Spanish gets the Spanish unit when it is here.  Otherwise the voice chosen in the settings
        // wins unless the user turned that off: a screen reader asks for a default once and sends it every time.
        val requested = SsiEngine.voiceByName(this, request.voiceName)
        val voice = when {
            isSpanish(request.language) && SsiEngine.spanish(this) -> SsiEngine.voiceFor(this, SsiNative.SPANISH)
            s.overrideVoice -> SsiEngine.voiceFor(this, s.voice)
            else -> requested ?: SsiEngine.voiceFor(this, s.voice)
        }

        SsiEngine.withEngine {
            SsiEngine.configure(s)
            val rate = SsiNative.nativeSampleRate()
            if (callback.start(rate, AudioFormat.ENCODING_PCM_16BIT, 1) != TextToSpeech.SUCCESS) return@withEngine
            if (text.isBlank()) { callback.done(); return@withEngine }
            // Armed only now, when this utterance becomes the one a stop should end: a stop aimed at the previous
            // one can arrive while this one is being set up (TGSpeechBox's stale-stop guard).
            stopRequested = false
            val started = SsiEngine.start(voice, text, s, request.speechRate, request.pitch)
            if (started < 0) {
                Log.w(TAG, "start failed: ${SsiNative.nativeError()}")
                callback.error(TextToSpeech.ERROR_SYNTHESIS)
                return@withEngine
            }
            if (started == 1) { callback.done(); return@withEngine }
            val size = minOf(8192, callback.maxBufferSize) and 1.inv()
            if (size <= 0) { SsiEngine.cancel(); callback.error(TextToSpeech.ERROR_OUTPUT); return@withEngine }
            val bytes = ByteArray(size)
            var total = 0
            var finished = false
            while (!stopRequested) {
                val n = SsiEngine.pull(bytes)
                if (n <= 0) { finished = n == 0; break }
                if (callback.audioAvailable(bytes, 0, n) != TextToSpeech.SUCCESS) break   // the framework let go
                total += n
            }
            // Stopped or abandoned: the unit drops the rest, so none of it reaches the next utterance.
            val loopEnd = android.os.SystemClock.elapsedRealtime()
            if (!finished) SsiEngine.cancel()
            val now = android.os.SystemClock.elapsedRealtime()
            val late = if (stopRequested) " ${now - stopAt} ms after the stop (the unit's cancel ${now - loopEnd} ms)" else ""
            // the request's rate and pitch (percent, 100 = normal): what TalkBack asks for, e.g. for a capital letter
            Log.i(TAG, "${voice.name}: ${total / 2} samples at $rate Hz, stopped=${!finished}$late, " +
                  "request rate ${request.speechRate} pitch ${request.pitch}")
            // A stop is an interruption, not a failure: screen readers treat an error as a failed utterance.
            callback.done()
        }
    }

    private companion object { const val TAG = "SsiTts" }
}
