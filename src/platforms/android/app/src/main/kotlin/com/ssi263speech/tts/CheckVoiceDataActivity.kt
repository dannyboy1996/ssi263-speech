// The framework fires ACTION_CHECK_TTS_DATA to ask whether the engine's voice data is usable.  English is always
// there: the Accent SA is built in.  Spanish is there once the user has imported the Spanish Braille Lite's firmware
// (the settings screen, which also answers INSTALL_TTS_DATA); until then it is reported unavailable.  ISO-3 locales,
// not voice names.
package com.ssi263speech.tts

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.speech.tts.TextToSpeech

class CheckVoiceDataActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val available = ArrayList<String>()
        val unavailable = ArrayList<String>()
        (if (SsiEngine.english(this)) available else unavailable).add("eng-USA")
        (if (SsiEngine.spanish(this)) available else unavailable).add("spa-ESP")
        val data = Intent().apply {
            putStringArrayListExtra(TextToSpeech.Engine.EXTRA_AVAILABLE_VOICES, available)
            putStringArrayListExtra(TextToSpeech.Engine.EXTRA_UNAVAILABLE_VOICES, unavailable)
        }
        setResult(if (available.isNotEmpty()) TextToSpeech.Engine.CHECK_VOICE_DATA_PASS
                  else TextToSpeech.Engine.CHECK_VOICE_DATA_FAIL, data)
        finish()
    }
}
