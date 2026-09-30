// The framework fires ACTION_CHECK_TTS_DATA to ask whether the engine's voice data is usable.  The unit's files
// come inside the APK, so English always is; Spanish when its files were built in.  ISO-3 locales, not voice names.
package com.ssi263speech.tts

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.speech.tts.TextToSpeech

class CheckVoiceDataActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pass = SsiData.ships(this, SsiNative.ENGLISH)
        val voices = ArrayList<String>()
        if (pass) {
            voices.add("eng-USA")
            if (SsiEngine.spanish(this)) voices.add("spa-ESP")
        }
        val data = Intent().apply {
            putStringArrayListExtra(TextToSpeech.Engine.EXTRA_AVAILABLE_VOICES, voices)
            putStringArrayListExtra(TextToSpeech.Engine.EXTRA_UNAVAILABLE_VOICES,
                if (pass) arrayListOf() else arrayListOf("eng-USA"))
        }
        setResult(if (pass) TextToSpeech.Engine.CHECK_VOICE_DATA_PASS else TextToSpeech.Engine.CHECK_VOICE_DATA_FAIL,
                  data)
        finish()
    }
}
