#include "SSIngerEditor.h"

#include "SSIngerProcessor.h"

#include <cmath>

namespace {

/* Colours, and their WCAG 2.x contrast ratios (measured), against the background #1B1F24 / the control panel
 * #2A3038:  text #FFFFFF 16.6 / 13.3:1, value text #E6E9EC 13.6 / 10.9:1, outlines #9AA4AE 6.5 / 5.3:1, slider
 * thumb and filled track #5EC4F2 8.4 / 6.8:1, focus ring #FFD54A 11.7 / 9.4:1; the popup menu's highlighted row,
 * text #10141A on #5EC4F2, 9.4:1.  Text needs 4.5:1 (1.4.3), controls and focus 3:1 (1.4.11). */
const juce::Colour bg{ 0xff1b1f24 }, panel{ 0xff2a3038 }, text{ 0xffffffff }, valueText{ 0xffe6e9ec },
    outline{ 0xff9aa4ae }, accent{ 0xff5ec4f2 }, focusRing{ 0xffffd54a }, dark{ 0xff10141a };

struct Laf final : juce::LookAndFeel_V4 {
    Laf()
    {
        setColour(juce::ResizableWindow::backgroundColourId, bg);
        setColour(juce::Label::textColourId, text);
        setColour(juce::GroupComponent::textColourId, text);
        setColour(juce::GroupComponent::outlineColourId, outline);
        setColour(juce::Slider::backgroundColourId, panel);
        setColour(juce::Slider::trackColourId, accent);
        setColour(juce::Slider::thumbColourId, accent);
        setColour(juce::Slider::textBoxTextColourId, valueText);
        setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour(juce::ComboBox::backgroundColourId, panel);
        setColour(juce::ComboBox::textColourId, text);
        setColour(juce::ComboBox::outlineColourId, outline);
        setColour(juce::ComboBox::arrowColourId, text);
        setColour(juce::ComboBox::focusedOutlineColourId, focusRing);
        setColour(juce::PopupMenu::backgroundColourId, panel);
        setColour(juce::PopupMenu::textColourId, text);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, accent);
        setColour(juce::PopupMenu::highlightedTextColourId, dark);
        setColour(juce::TooltipWindow::backgroundColourId, panel);
        setColour(juce::TooltipWindow::textColourId, text);
        setColour(juce::TooltipWindow::outlineColourId, outline);
    }

    /* The slider track's empty part is drawn with an outline so the control's extent is visible (1.4.11). */
    void drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                          juce::Slider::SliderStyle style, juce::Slider& s) override
    {
        juce::LookAndFeel_V4::drawLinearSlider(g, x, y, w, h, pos, minPos, maxPos, style, s);
        const float cy = (float)y + (float)h * 0.5f;
        g.setColour(outline);
        g.drawRoundedRectangle((float)x, cy - 3.0f, (float)w, 6.0f, 3.0f, 1.0f);
    }
};

/* Arrows step by the parameter's interval (JUCE's own handling); Page Up/Down take bigger steps and Home/End go to
 * the ends, as in a native slider. */
class KeySlider final : public juce::Slider {
public:
    bool keyPressed(const juce::KeyPress& k) override
    {
        if (!k.getModifiers().isAnyModifierKeyDown()) {
            const double lo = getMinimum(), hi = getMaximum();
            const double step = getInterval() > 0.0 ? getInterval() : (hi - lo) / 100.0;
            const double n = (hi - lo) / step;
            const double big = step * (n > 100.0 ? 10.0 : juce::jmax(1.0, std::round(n / 4.0)));
            if (k.isKeyCode(juce::KeyPress::pageUpKey))
                return move(getValue() + big);
            if (k.isKeyCode(juce::KeyPress::pageDownKey))
                return move(getValue() - big);
            if (k.isKeyCode(juce::KeyPress::homeKey))
                return move(lo);
            if (k.isKeyCode(juce::KeyPress::endKey))
                return move(hi);
        }
        return juce::Slider::keyPressed(k);
    }

private:
    bool move(double v)
    {
        setValue(juce::jlimit(getMinimum(), getMaximum(), v), juce::sendNotificationSync);
        return true;
    }
};

struct Spec {
    const char* group;
    const char* id;
    const char* help;
};

/* Reading order, one group after another. */
const Spec specs[] = {
    { "MIDI", ssinger_ids::phonBase, "The MIDI channel for phoneme notes. Pitch notes go on the next channel up." },
    { "MIDI", ssinger_ids::embod,
      "Phoneme and pitch: phonemes on one channel, pitch on the next (the patent's first embodiment). "
      "Expander: pitch notes on one channel, and program changes choose the phoneme." },
    { "MIDI", ssinger_ids::voices,
      "One chip, or four chips answering the channel pairs 1 and 2, 3 and 4, 5 and 6, 7 and 8, counted from the "
      "phoneme channel." },
    { "MIDI", ssinger_ids::velCurve, "How note velocity maps to the chip's amplitude." },
    { "MIDI", ssinger_ids::ctlMap,
      "What the pitch wheel does: the patent's filter frequency, or bending the chip's clock, which moves the "
      "pitch." },
    { "MIDI", ssinger_ids::bendRange, "How far the clock bend reaches at full wheel, in semitones." },
    { "Chip", ssinger_ids::artic, "Articulation: how fast the chip moves between sounds (register 3, bits 4 to 6)." },
    { "Chip", ssinger_ids::filterFF, "Filter frequency: the size of the voice's vocal tract (register 4)." },
    { "Chip", ssinger_ids::rate, "The chip's speaking rate (register 2, high bits)." },
    { "Chip", ssinger_ids::glide, "How fast the pitch glides to a new note, in glide mode (register 1, low bits)." },
    { "Chip", ssinger_ids::dur, "Phoneme duration and the chip's timing mode; 3 turns on pitch glides." },
    { "Output", ssinger_ids::clockSt,
      "The chip's master clock, in semitones from 1 MHz. It moves pitch, formants and timing together, like the "
      "original's variable oscillator." },
    { "Output", ssinger_ids::carrier,
      "The voice's source: the chip's own, or the sidechain input fed in as an external carrier." },
    { "Output", ssinger_ids::volume, "The output level." },
};

const char* groupTitle(const juce::String& g)
{
    if (g == "MIDI")
        return "MIDI";
    if (g == "Chip")
        return "Chip (SC-02 registers)";
    return "Output";
}

constexpr int rowH = 34, ctlH = 28, labelW = 190, ctlW = 290, pad = 12, headH = 26;

std::unique_ptr<Laf> theLaf;
int lafUsers = 0;

} // namespace

struct SSIngerEditor::Row {
    juce::Label label;
    std::unique_ptr<KeySlider> slider;
    std::unique_ptr<juce::ComboBox> combo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> cAtt;

    juce::Component& control() { return slider ? (juce::Component&)*slider : (juce::Component&)*combo; }
};

struct SSIngerEditor::Group {
    juce::GroupComponent box;
    std::vector<Row*> rows;
};

SSIngerEditor::SSIngerEditor(SSIngerProcessor& p)
    : AudioProcessorEditor(p), proc(p)
{
    if (lafUsers++ == 0)
        theLaf = std::make_unique<Laf>();
    setLookAndFeel(theLaf.get());
    setTitle("SSInger");
    setDescription("SSInger settings");

    for (const auto& s : specs) {
        if (groups.empty() || groups.back()->box.getName() != s.group) {
            auto g = std::make_unique<Group>();
            g->box.setName(s.group);
            g->box.setText(groupTitle(s.group));
            g->box.setTitle(groupTitle(s.group));      // the group's accessible name (1.3.1)
            addAndMakeVisible(g->box);
            groups.push_back(std::move(g));
        }
        auto* param = proc.apvts.getParameter(s.id);
        jassert(param != nullptr);
        const auto name = param->getName(64);
        auto row = std::make_unique<Row>();
        row->label.setText(name, juce::dontSendNotification);
        row->label.setAccessible(false);                // the control carries the same name
        row->label.setFont(juce::FontOptions(15.0f));
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*>(param)) {
            row->combo = std::make_unique<juce::ComboBox>();
            row->combo->addItemList(choice->choices, 1);
            row->cAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(proc.apvts, s.id,
                                                                                                 *row->combo);
        } else {
            row->slider = std::make_unique<KeySlider>();
            row->slider->setSliderStyle(juce::Slider::LinearHorizontal);
            row->slider->setTextBoxStyle(juce::Slider::TextBoxRight, true, 64, ctlH);
            row->sAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(proc.apvts, s.id,
                                                                                               *row->slider);
            for (auto* child : row->slider->getChildren()) {   // the value box repeats the slider's own value
                child->setAccessible(false);
                child->setWantsKeyboardFocus(false);
            }
        }
        auto& c = row->control();
        c.setTitle(name);                                // 4.1.2 / 2.5.3: the name is the visible label
        c.setDescription(s.help);                        // 3.3.2
        c.setHelpText(s.help);
        if (auto* tc = dynamic_cast<juce::SettableTooltipClient*>(&c))
            tc->setTooltip(s.help);
        c.setWantsKeyboardFocus(true);
        auto& g = *groups.back();
        g.box.addAndMakeVisible(row->label);
        g.box.addAndMakeVisible(c);
        g.rows.push_back(row.get());
        if (firstControl == nullptr)
            firstControl = &c;
        rows.push_back(std::move(row));
    }

    juce::Desktop::getInstance().addFocusChangeListener(this);

    int h = pad;
    for (auto& g : groups)
        h += headH + (int)g->rows.size() * rowH + pad + pad;
    setSize(pad * 2 + labelW + ctlW + pad * 2, h);

    /* Start on the first control, so a screen reader lands inside the settings when the window opens. */
    juce::Component::SafePointer<juce::Component> first(firstControl);
    juce::Timer::callAfterDelay(250, [first] {
        if (first != nullptr && first->isShowing())
            first->grabKeyboardFocus();
    });
}

SSIngerEditor::~SSIngerEditor()
{
    juce::Desktop::getInstance().removeFocusChangeListener(this);
    rows.clear();
    groups.clear();
    setLookAndFeel(nullptr);
    if (--lafUsers == 0)
        theLaf.reset();
}

void SSIngerEditor::paint(juce::Graphics& g)
{
    g.fillAll(bg);
}

/* 2.4.7: a ring around whichever control has keyboard focus. */
void SSIngerEditor::paintOverChildren(juce::Graphics& g)
{
    auto* f = juce::Component::getCurrentlyFocusedComponent();
    if (f == nullptr || !isParentOf(f))
        return;
    while (f != nullptr && f->getParentComponent() != nullptr && !f->getWantsKeyboardFocus())
        f = f->getParentComponent();
    auto r = getLocalArea(f, f->getLocalBounds()).toFloat().expanded(3.0f);
    g.setColour(focusRing);
    g.drawRoundedRectangle(r, 5.0f, 3.0f);
}

void SSIngerEditor::globalFocusChanged(juce::Component*)
{
    repaint();
}

void SSIngerEditor::resized()
{
    int y = pad;
    for (auto& g : groups) {
        const int gh = headH + (int)g->rows.size() * rowH + pad;
        g->box.setBounds(pad, y, getWidth() - 2 * pad, gh);
        int ry = headH;
        for (auto* row : g->rows) {
            row->label.setBounds(pad, ry, labelW, ctlH);
            row->control().setBounds(pad + labelW, ry, ctlW, ctlH);
            ry += rowH;
        }
        y += gh + pad;
    }
}
