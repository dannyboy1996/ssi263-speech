// The app's screen, laid out as outspoken's: two pages behind two plain buttons (a tab bar is hard to hit on a small
// screen, and TalkBack reads a selected/unselected button pair well).  Setup: what this is, a preview, the way to the
// system's TTS settings, the licences and source.  Voice settings: the voice and the unit's own settings.
package com.ssi263speech.tts

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast

class SettingsActivity : Activity() {

    private lateinit var ui: SettingsWidgets
    private lateinit var status: TextView
    private lateinit var sampleText: EditText
    private lateinit var speakButton: Button
    private lateinit var setupTab: Button
    private lateinit var voiceTab: Button
    private var pages: List<View> = emptyList()
    private var voiceButton: Button? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        ui = SettingsWidgets(this)
        val setupPage = ui.column().also { buildSetup(it) }
        val voicePage = ui.column().also { buildVoice(it) }

        setupTab = Button(this).apply { text = "Setup"; setOnClickListener { show(0) } }
        voiceTab = Button(this).apply { text = "Voice settings"; setOnClickListener { show(1) } }
        val wide = resources.configuration.screenWidthDp >= 340
        val tabs = LinearLayout(this).apply {
            orientation = if (wide) LinearLayout.HORIZONTAL else LinearLayout.VERTICAL
            addView(setupTab, tabParams(wide))
            addView(voiceTab, tabParams(wide))
        }
        val holder = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(ScrollView(this@SettingsActivity).apply { addView(setupPage) })
            addView(ScrollView(this@SettingsActivity).apply { addView(voicePage) })
        }
        pages = listOf(holder.getChildAt(0), holder.getChildAt(1))
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(tabs)
            addView(holder)
        }
        // Android 15 draws every activity edge to edge: without the insets the tab strip sits under the status bar.
        root.setOnApplyWindowInsetsListener { view, insets ->
            val bars = systemBarInsets(insets)
            view.setPadding(bars[0], bars[1], bars[2], bars[3])
            insets
        }
        setContentView(root)
        show(savedInstanceState?.getInt("page") ?: 0)
        refreshStatus()

        // Test hook: `am start -n com.ssi263speech.tts/.SettingsActivity --ez autospeak true [--es text "..."]`
        // speaks without navigating to the button.  Harmless in normal use (the extra is never set).
        if (intent?.getBooleanExtra("autospeak", false) == true) {
            intent.getStringExtra("text")?.let { sampleText.setText(it) }
            speak()
        }
        // Test hook: the TTS service through Android's client (TtsSelfTest.kt).
        if (intent?.getBooleanExtra("ttstest", false) == true) {
            TtsSelfTest(this, intent.getStringExtra("text") ?: sampleText.text.toString(),
                intent.getFloatExtra("rate", 1f), intent.getFloatExtra("pitch", 1f),
                intent.getBooleanExtra("aloud", false), intent.getIntExtra("stop", 0)).run()
        }
    }

    override fun onSaveInstanceState(out: Bundle) {
        out.putInt("page", if (pages.getOrNull(1)?.visibility == View.VISIBLE) 1 else 0)
        super.onSaveInstanceState(out)
    }

    override fun onDestroy() {
        PreviewPlayer.stop()
        super.onDestroy()
    }

    @Suppress("DEPRECATION")
    private fun systemBarInsets(insets: WindowInsets): IntArray =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val bars = insets.getInsets(WindowInsets.Type.systemBars())
            intArrayOf(bars.left, bars.top, bars.right, bars.bottom)
        } else {
            intArrayOf(insets.systemWindowInsetLeft, insets.systemWindowInsetTop,
                       insets.systemWindowInsetRight, insets.systemWindowInsetBottom)
        }

    private fun tabParams(wide: Boolean) =
        if (wide) LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        else LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)

    private fun show(page: Int) {
        pages.forEachIndexed { i, v -> v.visibility = if (i == page) View.VISIBLE else View.GONE }
        setupTab.isSelected = page == 0
        voiceTab.isSelected = page == 1
        // A screen-reader user whose focus is still on the tab hears nothing of the swap unless it is announced.
        setupTab.contentDescription = "Setup, tab 1 of 2"
        voiceTab.contentDescription = "Voice settings, tab 2 of 2"
        try { window.decorView.announceForAccessibility(if (page == 0) "Setup page" else "Voice settings page") }
        catch (e: Throwable) { /* a courtesy, never a failure */ }
    }

    // ---- page 1: setup ---------------------------------------------------------------------------------------------

    private fun buildSetup(root: LinearLayout) {
        root.addView(TextView(this).apply { text = "SSI-263 Speech"; textSize = 26f; gravity = Gravity.CENTER })
        root.addView(ui.body(
            "A Blazie Braille Lite 2000 in speech-box mode, emulated: the unit's own June 2003 firmware runs on an " +
            "emulated Z180 and drives a model of its Silicon Systems SSI-263 speech chip. The rules, the number " +
            "reading and the inflection are the firmware's own, live; nothing is recorded. It is the same voice " +
            "as the NVDA add-on and the Linux module, byte for byte."))

        root.addView(ui.heading("Status"))
        status = ui.body("")
        root.addView(status)

        root.addView(ui.heading("Try it"))
        val sampleLabel = ui.body("Text to speak").also { root.addView(it) }
        sampleText = EditText(this).apply {
            id = View.generateViewId()
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE
            setText("Hello there. This is the Braille Lite, speaking on your phone. You owe 1,234 dollars.")
            textSize = 15f
        }
        sampleLabel.labelFor = sampleText.id
        root.addView(sampleText)
        speakButton = Button(this).apply { text = "Speak"; setOnClickListener { speak() } }
        root.addView(speakButton)
        root.addView(Button(this).apply { text = "Stop"; setOnClickListener { PreviewPlayer.stop() } })

        root.addView(ui.body("\nThen choose this engine in the system's Text-to-speech settings:"))
        root.addView(Button(this).apply { text = "Open Text-to-speech settings"; setOnClickListener { openTtsSettings() } })

        root.addView(ui.heading("Licenses and source"))
        root.addView(ui.body(
            "This app is free software under the GNU General Public License, version 2 or later, because it " +
            "carries z180emu. Its complete source is inside the app and at github.com/tgeczy/ssi263-speech. The " +
            "Braille Lite's firmware is shared with permission and is not covered by that license."))
        root.addView(Button(this).apply { text = "Licenses and source"; setOnClickListener { showLicenses() } })
    }

    private fun refreshStatus() {
        val voices = SsiEngine.voices(this)
        status.text = if (voices.isEmpty()) "This build carries no Braille Lite firmware, so it cannot speak."
            else "Voices: " + voices.joinToString(", ") { it.label } + "."
        speakButton.isEnabled = voices.isNotEmpty()
    }

    private fun speak() {
        val text = sampleText.text.toString().ifBlank { "Hello there." }
        speakButton.isEnabled = false
        status.text = "Speaking…"
        Thread({
            val n = PreviewPlayer.speak(this, text)
            runOnUiThread {
                if (isFinishing || isDestroyed) return@runOnUiThread
                speakButton.isEnabled = true
                status.text = if (n < 0) "The voice could not start: ${SsiNative.nativeError()}" else "Spoke $n samples."
            }
        }, "ssi263-preview").start()
    }

    private fun openTtsSettings() {
        for (i in listOf(Intent("com.android.settings.TTS_SETTINGS"), Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS),
                         Intent(Settings.ACTION_SETTINGS))) {
            try { i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK); startActivity(i); return }
            catch (e: Exception) { /* try the next */ }
        }
        Toast.makeText(this, "Couldn't open settings on this device.", Toast.LENGTH_SHORT).show()
    }

    /** outspoken's flow: the distribution notice, then every licence the APK carries. */
    private fun showLicenses() {
        fun read(name: String) = assets.open("licenses/$name").bufferedReader().use { it.readText() }
        fun title(name: String) = name.removeSuffix(".txt").replace('-', ' ')
        AlertDialog.Builder(this).setTitle("Licenses and source").setMessage(read("DISTRIBUTION.txt"))
            .setPositiveButton("Close", null)
            .setNeutralButton("Full licenses") { _, _ ->
                val files = assets.list("licenses").orEmpty().filter { it != "DISTRIBUTION.txt" }.sorted()
                AlertDialog.Builder(this).setTitle("Full licenses")
                    .setItems(files.map { title(it) }.toTypedArray()) { _, which ->
                        AlertDialog.Builder(this).setTitle(title(files[which])).setMessage(read(files[which]))
                            .setPositiveButton("Close", null).show()
                    }.setNegativeButton("Close", null).show()
            }.show()
    }

    // ---- page 2: voice settings ------------------------------------------------------------------------------------

    private fun buildVoice(root: LinearLayout) {
        val p = SsiSettings.prefs(this)
        val s = SsiSettings.snapshot(this)
        fun put(key: String, v: Int) = p.edit().putInt(key, v).apply()
        fun put(key: String, v: Boolean) = p.edit().putBoolean(key, v).apply()

        val voiceLabel = ui.heading("Voice").also { root.addView(it) }
        val voices = SsiEngine.voices(this)
        voiceButton = Button(this).apply {
            id = View.generateViewId()
            text = "Voice: " + SsiEngine.voiceFor(this@SettingsActivity, s.voice).label
            setOnClickListener {
                val at = voices.indexOfFirst { it.index == SsiSettings.snapshot(this@SettingsActivity).voice }
                AlertDialog.Builder(this@SettingsActivity).setTitle("Voice")
                    .setSingleChoiceItems(voices.map { it.label }.toTypedArray(), at) { dialog, which ->
                        dialog.dismiss()
                        put(SsiSettings.VOICE, voices[which].index)
                        text = "Voice: " + voices[which].label
                    }.setNegativeButton("Cancel", null).show()
            }
        }
        voiceLabel.labelFor = voiceButton!!.id
        root.addView(voiceButton)
        ui.checkBox(root, "Use selected voice in all apps", s.overrideVoice) { put(SsiSettings.OVERRIDE_VOICE, it) }
        root.addView(ui.body("A screen reader asks this engine for a voice once and keeps it; with this on, the " +
            "voice chosen here is heard straight away. A request in Spanish gets the Spanish unit either way, " +
            "when it is here."))

        root.addView(ui.heading("Rate"))
        root.addView(ui.body("The NVDA add-on's scale: 50 is the unit's factory rate. The rate an app asks for -- " +
            "the system's speech rate or a screen reader's own -- is applied on top."))
        ui.slider(root, "Speech rate", 100, s.rate, { if (it == 50) "50, the unit's factory rate" else "$it" }) {
            put(SsiSettings.RATE, it)
        }

        root.addView(ui.heading("Pitch"))
        root.addView(ui.body("50 is the unit's factory pitch; an app's pitch is applied on top."))
        ui.slider(root, "Pitch", 100, s.pitch, { if (it == 50) "50, the unit's factory pitch" else "$it" }) {
            put(SsiSettings.PITCH, it)
        }

        root.addView(ui.heading("Tone"))
        root.addView(ui.body("The unit's tone setting, 0 to 26. 7 is the factory tone."))
        ui.slider(root, "Tone", 26, s.tone, { if (it == 7) "7, factory" else "$it" }) { put(SsiSettings.TONE, it) }

        root.addView(ui.heading("Volume"))
        root.addView(ui.body("The engine's own level. Android's accessibility or media volume still applies."))
        ui.slider(root, "Engine volume", 100, s.volume, { "$it percent" }) { put(SsiSettings.VOLUME, it) }

        root.addView(ui.heading("The unit"))
        ui.checkBox(root, "Voice inflection", s.inflection) { put(SsiSettings.INFLECTION, it) }
        root.addView(ui.body("The unit's own status-menu setting. Off, questions stay flat."))
        ui.checkBox(root, "Short pauses", s.shortPauses) { put(SsiSettings.SHORT_PAUSES, it) }
        root.addView(ui.body("Sentences packed onto one line from the second on, as the NVDA add-on's default."))

        ui.choice(root, "Idle sound", listOf("Off", "Hiss", "Whine"), s.whine) { put(SsiSettings.WHINE, it) }
        root.addView(ui.body("The faint sound a real unit makes under its speech: hiss at even volumes (the factory " +
            "setting), whine at odd ones."))

        ui.choice(root, "Sample rate", listOf("11 kHz, like the unit's own speaker", "22 kHz (recommended)",
            "44 kHz"), SsiSettings.SAMPLE_RATES.indexOf(s.sampleRate)) { put(SsiSettings.SAMPLE_RATE, SsiSettings.SAMPLE_RATES[it]) }
        root.addView(ui.body("Inflection, the idle sound and the sample rate restart the unit on the next " +
            "utterance, which takes a moment."))
    }
}
