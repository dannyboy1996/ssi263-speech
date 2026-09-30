// The settings screen's building blocks, from outspoken's SettingsActivity: headings a screen reader knows are
// headings, a slider that says its name and value and takes one-step arrows and Home/End, a set of radio buttons
// that each say their own name and state, and a check box.
package com.ssi263speech.tts

import android.app.Activity
import android.view.KeyEvent
import android.view.View
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.SeekBar
import android.widget.TextView

class SettingsWidgets(private val activity: Activity) {
    val pad = (16 * activity.resources.displayMetrics.density).toInt()

    fun column() = LinearLayout(activity).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(pad, pad, pad, pad)
    }

    fun heading(t: String) = TextView(activity).apply {
        text = t; textSize = 20f; setPadding(0, pad, 0, pad / 2)
        if (android.os.Build.VERSION.SDK_INT >= 28) isAccessibilityHeading = true
    }

    fun body(t: String) = TextView(activity).apply { text = t; textSize = 15f }

    /** A slider that says its name and value: "Speech rate, 50, the unit's factory rate". */
    class ValueSlider(context: android.content.Context, private val label: TextView, private val name: String,
                      private val describe: (Int) -> String) : SeekBar(context) {
        fun refreshValue() {
            label.text = describe(progress)
            if (android.os.Build.VERSION.SDK_INT >= 30) {
                contentDescription = name
                stateDescription = describe(progress)
            } else contentDescription = "$name, ${describe(progress)}"
        }
    }

    fun slider(root: LinearLayout, name: String, limit: Int, value: Int, describe: (Int) -> String,
               save: (Int) -> Unit): ValueSlider {
        val label = body(describe(value)).apply { importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO }
        root.addView(label)
        val s = ValueSlider(activity, label, name, describe).apply {
            id = View.generateViewId()
            max = limit
            progress = value
            keyProgressIncrement = 1
            refreshValue()
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(s: SeekBar, v: Int, user: Boolean) {
                    refreshValue()
                    if (user) save(v)
                }
                override fun onStartTrackingTouch(s: SeekBar) {}
                override fun onStopTrackingTouch(s: SeekBar) {}
            })
            setOnKeyListener { _, key, event ->
                val target = when (key) {
                    KeyEvent.KEYCODE_DPAD_LEFT -> (progress - 1).coerceAtLeast(0)
                    KeyEvent.KEYCODE_DPAD_RIGHT -> (progress + 1).coerceAtMost(max)
                    KeyEvent.KEYCODE_MOVE_HOME -> 0
                    KeyEvent.KEYCODE_MOVE_END -> max
                    else -> return@setOnKeyListener false
                }
                if (event.action == KeyEvent.ACTION_DOWN) {
                    progress = target
                    save(target)
                }
                true
            }
        }
        label.labelFor = s.id
        root.addView(s)
        return s
    }

    /** Radio buttons: one focus stop per option, each saying its name and whether it is the one in use. */
    fun choice(root: LinearLayout, name: String, items: List<String>, selected: Int, onPick: (Int) -> Unit): RadioGroup {
        val label = heading(name)
        root.addView(label)
        val group = RadioGroup(activity).apply {
            id = View.generateViewId()
            orientation = RadioGroup.VERTICAL
            items.forEachIndexed { i, item ->
                addView(RadioButton(activity).apply {
                    id = View.generateViewId()
                    text = item
                    textSize = 16f
                    isChecked = i == selected
                })
            }
            setOnCheckedChangeListener { g, checkedId ->
                val i = (0 until g.childCount).indexOfFirst { g.getChildAt(it).id == checkedId }
                if (i >= 0) onPick(i)
            }
        }
        label.labelFor = group.id
        root.addView(group)
        return group
    }

    fun checkBox(root: LinearLayout, text: String, checked: Boolean, onChange: (Boolean) -> Unit): CheckBox =
        CheckBox(activity).apply {
            this.text = text
            isChecked = checked
            setOnCheckedChangeListener { _, c -> onChange(c) }
            root.addView(this)
        }
}
