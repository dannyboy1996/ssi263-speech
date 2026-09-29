/* RobovoxProcessor -- JUCE shell around the emulated Robovox system.
 *
 * MIDI in -> emulated 6850 ACIA -> clean-room translator (patent
 * EP0396141A2, embodiment 1 default) -> 1 or 4 ssi263 (SC-02) engines.
 * The 6502 socket, bus map and ROM slot live in emu/robovox_bus.h;
 * Phase 2 moves the translator into a 6502-resident ROM image.
 *
 * Editor is the generic APVTS editor (Phase 1). The PEC-style phoneme
 * score editor (text + segment grid + live pitch lanes, RESEARCH.md
 * section 7) is the planned custom UI.
 */
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "emu/robovox_bus.h"

class RobovoxProcessor : public juce::AudioProcessor {
public:
    RobovoxProcessor();
    ~RobovoxProcessor() override;

    const juce::String getName() const override { return "Robovox"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Robovox"; }
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
    robovox_bus_t bus{};
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RobovoxProcessor)
};
