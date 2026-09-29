/* SSIngerProcessor -- JUCE shell around the emulated Robovox system.
 *
 * MIDI in -> emulated 6850 ACIA -> clean-room translator (patent
 * EP0396141A2, embodiment 1 default) -> 1 or 4 ssi263 (SC-02) engines.
 * The 6502 socket, bus map and ROM slot live in emu/ssinger_bus.h;
 * Phase 2 moves the translator into a 6502-resident ROM image.
 *
 * Editor: SSIngerEditor, the settings built for keyboard and screen-reader
 * use (SSIngerEditor.h). The PEC-style phoneme score editor (text + segment grid + live pitch lanes, RESEARCH.md
 * section 7) is the planned custom UI.
 */
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "emu/ssinger_bus.h"

/* Parameter IDs, shared by the processor and the editor. */
namespace ssinger_ids {
inline constexpr const char* phonBase = "phonBase";
inline constexpr const char* voices = "voices";
inline constexpr const char* embod = "embod";
inline constexpr const char* ctlMap = "ctlMap";
inline constexpr const char* artic = "artic";
inline constexpr const char* filterFF = "filterFF";
inline constexpr const char* rate = "rate";
inline constexpr const char* glide = "glide";
inline constexpr const char* dur = "dur";
inline constexpr const char* velCurve = "velCurve";
inline constexpr const char* bendRange = "bendRange";
inline constexpr const char* clockSt = "clockSt";
inline constexpr const char* carrier = "carrier";
inline constexpr const char* volume = "volume";
}

class SSIngerProcessor : public juce::AudioProcessor {
public:
    SSIngerProcessor();
    ~SSIngerProcessor() override;

    const juce::String getName() const override { return "SSInger"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "SSInger"; }
    void changeProgramName(int, const juce::String&) override {}

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>& buffer,
                      juce::MidiBuffer& midiMessages) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

private:
    ssinger_bus_t bus{};
    bool busOk = false;
    ssi263_params engDefaults{};
    unsigned char engRom[SSI263_ROM_BYTES]{};
    std::vector<double> tmp;

    int voicesCache = 0;
    float volumeCache = 0.8f;
    int carrierCache = 0;

    void rebuildBus(double sampleRate, int nvoices);
    void applyParams();
    void renderChunk(juce::AudioBuffer<float>& out, int from, int to);
    void feedMessage(const juce::MidiMessage& m);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SSIngerProcessor)
};
