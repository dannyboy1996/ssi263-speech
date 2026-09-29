#include "SSIngerProcessor.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace ids {
static const char* phonBase = "phonBase";
static const char* voices = "voices";
static const char* embod = "embod";
static const char* ctlMap = "ctlMap";
static const char* artic = "artic";
static const char* filterFF = "filterFF";
static const char* rate = "rate";
static const char* glide = "glide";
static const char* dur = "dur";
static const char* velCurve = "velCurve";
static const char* bendRange = "bendRange";
static const char* clockSt = "clockSt";
static const char* carrier = "carrier";
static const char* volume = "volume";
}

SSIngerProcessor::SSIngerProcessor()
    : AudioProcessor(BusesProperties()
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)
                         .withInput("Carrier", juce::AudioChannelSet::stereo(), false)),
      apvts(*this, nullptr, "PARAMS", createLayout())
{
    ssi263_default_params(&engDefaults);
    std::memcpy(engRom, ssi263_default_rom(), sizeof(engRom));
}

SSIngerProcessor::~SSIngerProcessor()
{
    if (busOk) {
        ssinger_bus_free(&bus);
        busOk = false;
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout SSIngerProcessor::createLayout()
{
    using PID = juce::ParameterID;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    /* MIDI front end. */
    layout.add(std::make_unique<juce::AudioParameterInt>(
        PID{ ids::phonBase, 1 }, "Phoneme channel", 1, 16, 1));
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        PID{ ids::voices, 1 }, "Voices",
        juce::StringArray{ "SEQ (1 voice)", "Quad (tour rig)" }, 0));
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        PID{ ids::embod, 1 }, "MIDI embodiment",
        juce::StringArray{ "N/N+1 phoneme+pitch", "Expander (PC)" }, 0));
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        PID{ ids::ctlMap, 1 }, "Wheel map",
        juce::StringArray{ "Patent (bend=filter)", "Clock bend (Polaxis-style)" }, 1));
    /* SC-02 registers, straight off the chip (RESEARCH.md section 3). */
    layout.add(std::make_unique<juce::AudioParameterInt>(
        PID{ ids::artic, 1 }, "Articulation (ART)", 0, 7, 5));
    layout.add(std::make_unique<juce::AudioParameterInt>(
        PID{ ids::filterFF, 1 }, "Filter frequency (FF)", 0, 255, 0xE4));
    layout.add(std::make_unique<juce::AudioParameterInt>(
        PID{ ids::rate, 1 }, "Rate", 0, 15, 8));
    layout.add(std::make_unique<juce::AudioParameterInt>(
        PID{ ids::glide, 1 }, "Pitch glide (R1)", 0, 7, 4));
    layout.add(std::make_unique<juce::AudioParameterInt>(
        PID{ ids::dur, 1 }, "Phoneme DUR", 0, 3, 2));
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        PID{ ids::velCurve, 1 }, "Velocity curve",
        juce::StringArray{ "Linear", "Log (:L:)" }, 0));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        PID{ ids::bendRange, 1 }, "Bend range (st)",
        juce::NormalisableRange<float>(1.0f, 48.0f, 0.5f), 24.0f));
    /* System. */
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        PID{ ids::clockSt, 1 }, "Master clock (st)",
        juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f), 0.0f));
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        PID{ ids::carrier, 1 }, "Carrier",
        juce::StringArray{ "Internal", "External (sidechain)" }, 0));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        PID{ ids::volume, 1 }, "Volume",
        juce::NormalisableRange<float>(0.0f, 1.5f, 0.01f), 0.8f));
    return layout;
}

void SSIngerProcessor::rebuildBus(double sampleRate, int nvoices)
{
    if (busOk)
        ssinger_bus_free(&bus);
    busOk = ssinger_bus_init(&bus, sampleRate, nvoices, &engDefaults, engRom) != 0;
    voicesCache = nvoices;
    if (busOk)
        applyParams();
}

void SSIngerProcessor::applyParams()
{
    if (!busOk)
        return;
    auto get = [this](const char* id) { return apvts.getRawParameterValue(id)->load(); };

    int wantVoices = (int)get(ids::voices) == 0 ? 1 : 4;
    if (wantVoices != bus.nvoices) {
        rebuildBus(bus.sample_rate, wantVoices);
        return; /* rebuildBus re-enters applyParams */
    }

    auto& cfg = bus.fw.cfg;
    cfg.base_channel = juce::jlimit(0, 15, (int)get(ids::phonBase) - 1);
    cfg.embodiment = (int)get(ids::embod) == 0 ? SG_EMB_PHONEME_PITCH : SG_EMB_EXPANDER;
    cfg.ctlmap = (int)get(ids::ctlMap) == 0 ? SG_MAP_PATENT : SG_MAP_POLAXIS;

    int newRate = (int)get(ids::rate);
    int newArt = (int)get(ids::artic);
    int newGlide = (int)get(ids::glide);
    int newDur = (int)get(ids::dur);
    if (newRate != cfg.rate || newGlide != cfg.glide) {
        cfg.rate = newRate & 15;
        cfg.glide = newGlide & 7;
        for (int v = 0; v < bus.nvoices; v++)
            ssinger_write_inflection(&bus.fw, v, bus.fw.v[v].inflection);
    }
    if (newArt != cfg.articulation) {
        cfg.articulation = newArt & 7;
        for (int v = 0; v < bus.nvoices; v++)
            ssinger_write_r3(&bus.fw, v, bus.fw.v[v].amplitude);
    }
    cfg.dur = newDur & 3;
    cfg.vel_curve = (int)get(ids::velCurve);
    cfg.bend_range_st = get(ids::bendRange);

    int newFF = (int)get(ids::filterFF);
    if (newFF != cfg.filter_ff) {
        cfg.filter_ff = newFF & 0xFF;
        for (int v = 0; v < bus.nvoices; v++) {
            bus.fw.v[v].filter_ff = cfg.filter_ff;
            ssinger_bus_sc_write(v, SG_SC_R4, cfg.filter_ff, &bus);
        }
    }

    double base = 1000000.0 * std::pow(2.0, get(ids::clockSt) / 12.0);
    if (base != bus.fw.nominal_xck)
        ssinger_bus_master_write(base, &bus);

    volumeCache = get(ids::volume);
    carrierCache = (int)get(ids::carrier);
}

void SSIngerProcessor::prepareToPlay(double sampleRate, int)
{
    rebuildBus(sampleRate, 1);
}

void SSIngerProcessor::releaseResources()
{
}

bool SSIngerProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono())
        return false;
    /* Carrier sidechain: anything the host offers, including disconnected. */
    return true;
}

void SSIngerProcessor::feedMessage(const juce::MidiMessage& m)
{
    auto* d = m.getRawData();
    for (int i = 0; i < m.getRawDataSize(); i++)
        ssinger_bus_midi_byte(&bus, d[i]);
}

void SSIngerProcessor::renderChunk(juce::AudioBuffer<float>& out, int from, int to)
{
    int n = to - from;
    if (n <= 0)
        return;
    if ((long)tmp.size() < n + 1)
        tmp.resize((size_t)n + 1);
    ssinger_bus_render(&bus, n, tmp.data());
    ssinger_bus_slew_filters(&bus, n / bus.sample_rate);
    ssinger_bus_service_all(&bus);
    int nch = out.getNumChannels();
    for (int ch = 0; ch < nch; ch++) {
        float* dst = out.getWritePointer(ch, from);
        for (int i = 0; i < n; i++) {
            float y = (float)(tmp[(size_t)i] * volumeCache);
            dst[i] += (float)std::tanh(y);
        }
    }
}

void SSIngerProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                    juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    if (!busOk) {
        buffer.clear();
        return;
    }
    applyParams();

    auto out = getBusBuffer(buffer, false, 0);
    out.clear();

    juce::MidiBuffer::Iterator it(midiMessages);
    juce::MidiMessage m;
    int pos = 0, last = 0;
    while (it.getNextEvent(m, pos)) {
        renderChunk(out, last, pos);
        feedMessage(m);
        last = pos;
    }
    renderChunk(out, last, buffer.getNumSamples());

    /* External carrier (sidechain monitor; TP1/TP2 filter injection is
     * Phase 2 -- RESEARCH.md section 5.4). */
    if (carrierCache == 1 && getTotalNumInputChannels() > 0) {
        auto car = getBusBuffer(buffer, true, 0);
        int nch = juce::jmin(out.getNumChannels(), car.getNumChannels());
        int n = juce::jmin(buffer.getNumSamples(), car.getNumSamples());
        for (int ch = 0; ch < nch; ch++) {
            float* dst = out.getWritePointer(ch);
            const float* src = car.getReadPointer(ch);
            for (int i = 0; i < n; i++)
                dst[i] = (float)std::tanh(dst[i] * 0.5f + src[i] * 0.5f * volumeCache);
        }
    }
}

juce::AudioProcessorEditor* SSIngerProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor(*this);
}

void SSIngerProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary(*xml, destData);
}

void SSIngerProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SSIngerProcessor();
}
