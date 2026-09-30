// The app's settings, in device-protected storage so the service can read them before the phone is unlocked.
// The scales are the NVDA driver's (and the speech-dispatcher module's): rate and pitch 0-100 with 50 the unit's
// factory rate 11 and pitch 16, tone 0-26 (factory 7), volume 0-100, and the unit's own boot settings.
package com.ssi263speech.tts

import android.content.Context
import android.content.SharedPreferences

object SsiSettings {
    private const val PREFS = "ssi263speech"
    const val RATE = "rate"
    const val PITCH = "pitch"
    const val TONE = "tone"
    const val VOLUME = "volume"
    const val SHORT_PAUSES = "short_pauses"
    const val INFLECTION = "inflection"
    const val WHINE = "whine"
    const val SAMPLE_RATE = "sample_rate"
    const val VOICE = "voice"
    const val OVERRIDE_VOICE = "override_voice"

    val SAMPLE_RATES = listOf(11025, 22050, 44100)

    fun prefs(ctx: Context): SharedPreferences =
        SsiData.protectedContext(ctx).getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    /** Read together once per utterance. */
    data class Snapshot(val rate: Int = 50, val pitch: Int = 50, val tone: Int = 7, val volume: Int = 100,
                        val shortPauses: Boolean = true, val inflection: Boolean = true, val whine: Int = 0,
                        val sampleRate: Int = 22050, val voice: Int = SsiNative.ENGLISH,
                        val overrideVoice: Boolean = true)

    fun snapshot(ctx: Context): Snapshot {
        val p = prefs(ctx)
        return Snapshot(
            p.getInt(RATE, 50).coerceIn(0, 100),
            p.getInt(PITCH, 50).coerceIn(0, 100),
            p.getInt(TONE, 7).coerceIn(0, 26),
            p.getInt(VOLUME, 100).coerceIn(0, 100),
            p.getBoolean(SHORT_PAUSES, true),
            p.getBoolean(INFLECTION, true),
            p.getInt(WHINE, 0).coerceIn(0, 2),
            p.getInt(SAMPLE_RATE, 22050).takeIf { it in SAMPLE_RATES } ?: 22050,
            p.getInt(VOICE, SsiNative.ENGLISH).coerceIn(SsiNative.ENGLISH, SsiNative.SPANISH),
            p.getBoolean(OVERRIDE_VOICE, true))
    }
}
