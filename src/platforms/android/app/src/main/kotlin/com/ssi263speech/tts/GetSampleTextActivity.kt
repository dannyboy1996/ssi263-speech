// ACTION_GET_SAMPLE_TEXT: the phrase the system speaks when the user previews the engine in Settings -- in Spanish
// when that is asked for and the Spanish unit is here.
package com.ssi263speech.tts

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.speech.tts.TextToSpeech

class GetSampleTextActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val lang = intent?.getStringExtra("language")
        val spanish = (lang == "spa" || lang == "es") && SsiEngine.spanish(this)
        val data = Intent().putExtra(
            TextToSpeech.Engine.EXTRA_SAMPLE_TEXT,
            if (spanish) "Hola. Este es un Braille Lite, hablando en tu teléfono."
            else "Hello there. This is a Braille Lite, speaking on your phone.")
        setResult(TextToSpeech.LANG_AVAILABLE, data)
        finish()
    }
}
