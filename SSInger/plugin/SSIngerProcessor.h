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

#include <atomic>
#include <vector>

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

class SSIngerProcessor : public juce::AudioProcessor, private juce::Timer {
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
    /* Two emulated systems, built off the audio thread: slot 0 is SEQ (one
     * chip), slot 1 the quad tour rig. Switching Voices swaps the active
     * slot on the audio thread (no allocation, no lock); the slot left
     * behind is marked stale and rebuilt fresh on the message thread
     * (timerCallback), so the next switch lands on a powered-on system the
     * way the old in-place rebuild did. */
    enum SlotState { slotReady = 0, slotStale = 1, slotBuilding = 2 };
    struct Slot {
        ssinger_bus_t bus{};
        bool ok = false;
        std::atomic<int> state{ slotStale };
    };
    Slot slots[2];
    int active = 0;                    /* audio thread only */
    double preparedRate = 0.0;         /* 0 until prepareToPlay */
    juce::CriticalSection buildLock;   /* prepare/release vs timer rebuilds; never the audio thread */
    ssi263_params engDefaults{};
    unsigned char engRom[SSI263_ROM_BYTES]{};
    std::vector<double> tmp;           /* mono render scratch, sized once in the constructor */

    float volumeCache = 0.8f;
    int carrierCache = 0;

    static int slotForVoicesParam(float choice) { return (int)choice == 0 ? 0 : 1; }
    void buildSlot(int i, double sampleRate);
    void freeSlots();
    void timerCallback() override;
    void applyParams(ssinger_bus_t& bus);
    void renderSpan(ssinger_bus_t& bus, juce::AudioBuffer<float>& out, const juce::AudioBuffer<float>& carrier,
                    int from, int to);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SSIngerProcessor)
};
