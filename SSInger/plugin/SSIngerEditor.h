/* SSIngerEditor -- the plugin's own editor, built for keyboard and screen-reader use first (WCAG 2.2 AA, applied
 * to a desktop plugin):
 *   4.1.2 name, role, value   every control is a real slider or combo box whose accessible name is its label and
 *                             whose value is the parameter's own text ("5", not 0.714)
 *   1.3.1 relationships       controls sit in named groups (MIDI / Chip / Output), so a screen reader announces
 *                             the group on entry
 *   2.1.1 / 2.1.2 keyboard    Tab and Shift+Tab reach every control, with no trap; arrows step, Page Up/Down take
 *                             bigger steps, Home/End go to the ends; combo boxes change with the arrows
 *   2.4.3 focus order         top to bottom, in reading order
 *   2.4.7 focus visible       a 3 px ring around the focused control
 *   1.4.3 / 1.4.11 contrast   text >= 4.5:1, controls and the focus ring >= 3:1 against the background
 *                             (ratios in SSIngerEditor.cpp)
 *   2.5.3 label in name       each accessible name is exactly its visible label
 *   3.3.2 instructions        every control has a one-line help text (the screen reader's description, and a
 *                             tooltip)
 *   2.5.8 target size         every control is at least 28 px high
 * The visible labels are hidden from assistive technology, since each control already carries the same name.
 * JUCE's GenericAudioProcessorEditor, used before, put each name in a separate text element, left the sliders
 * unnamed with 0..1 values, and kept every control inside a tree that Tab never entered.
 */
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

class SSIngerProcessor;

class SSIngerEditor : public juce::AudioProcessorEditor, private juce::FocusChangeListener {
public:
    explicit SSIngerEditor(SSIngerProcessor&);
    ~SSIngerEditor() override;

    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;
    void resized() override;

private:
    struct Row;
    struct Group;

    void globalFocusChanged(juce::Component*) override;

    SSIngerProcessor& proc;
    juce::TooltipWindow tooltips{ this, 600 };
    std::vector<std::unique_ptr<Group>> groups;
    std::vector<std::unique_ptr<Row>> rows;
    juce::Component* firstControl = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SSIngerEditor)
};
