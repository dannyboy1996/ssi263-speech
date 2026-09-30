// A check of the TTS service itself, through Android's own TextToSpeech client bound to this engine by package name
// -- so the system's default engine is left alone.
//
// File: synthesizeToFile at the rate and pitch asked for (then, with aloud, the same text spoken).  The file is the
// service's PCM as the framework received it; src/platforms/android/test/test_device_service.py compares it.
//   adb shell am start -n com.ssi263speech.tts/.SettingsActivity --ez ttstest true \
//       [--es text "..."] [--ef rate 2.0] [--ef pitch 1.0] [--ez aloud true]
//   adb shell run-as com.ssi263speech.tts cat files/tts-test.wav > tts-test.wav
//
// Stop: the text spoken aloud, a stop after that many milliseconds, then "Next message." -- the service logs how
// long after the stop its synthesis thread was free (logcat -s SsiTts).
//   adb shell am start -n com.ssi263speech.tts/.SettingsActivity --ez ttstest true --ei stop 1500 [--es text "..."]
package com.ssi263speech.tts

import android.content.Context
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.speech.tts.TextToSpeech
import android.speech.tts.UtteranceProgressListener
import android.util.Log
import java.io.File

class TtsSelfTest(private val ctx: Context, private val text: String, private val rate: Float,
                  private val pitch: Float, private val aloud: Boolean, private val stopAfterMs: Int = 0) {
    private var tts: TextToSpeech? = null
    private val main = Handler(Looper.getMainLooper())

    fun run() {
        tts = TextToSpeech(ctx.applicationContext, { status ->
            val t = tts ?: return@TextToSpeech
            if (status != TextToSpeech.SUCCESS) { Log.e(TAG, "the engine did not bind: $status"); return@TextToSpeech }
            t.setSpeechRate(rate)
            t.setPitch(pitch)
            t.setOnUtteranceProgressListener(object : UtteranceProgressListener() {
                override fun onStart(id: String?) { Log.i(TAG, "start: $id") }
                override fun onDone(id: String?) {
                    Log.i(TAG, "done: $id")
                    when {
                        id == "file" && aloud -> t.speak(text, TextToSpeech.QUEUE_ADD, Bundle(), "aloud")
                        id == "long" -> {}                   // the stop's utterance: "next" follows the stop
                        else -> shutdownSoon()
                    }
                }
                override fun onStop(id: String?, interrupted: Boolean) { Log.i(TAG, "stopped: $id") }
                @Deprecated("the framework still calls it") override fun onError(id: String?) {
                    Log.e(TAG, "error: $id"); shutdownSoon()
                }
            })
            if (stopAfterMs > 0) {
                t.speak(text, TextToSpeech.QUEUE_FLUSH, Bundle(), "long")
                main.postDelayed({
                    Log.i(TAG, "stop")
                    t.stop()
                    t.speak("Next message.", TextToSpeech.QUEUE_ADD, Bundle(), "next")
                }, stopAfterMs.toLong())
                return@TextToSpeech
            }
            val out = File(ctx.filesDir, "tts-test.wav")
            val rc = t.synthesizeToFile(text, Bundle(), out, "file")
            Log.i(TAG, "synthesizeToFile rate=$rate pitch=$pitch -> $rc ($out)")
        }, ctx.packageName)
    }

    private fun shutdownSoon() {
        main.postDelayed({ tts?.shutdown(); tts = null }, 500)
    }

    private companion object { const val TAG = "SsiSelfTest" }
}
