#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace {

juce::NormalisableRange<float> rangeFor (const mse::ParamSpec& s)
{
    juce::NormalisableRange<float> r (s.min, s.max, s.type == mse::ParamType::Int ? 1.0f : 0.0f);
    if (s.skewCentre > s.min && s.skewCentre < s.max)
        r.setSkewForCentre (s.skewCentre);
    return r;
}

} // namespace

MinervaSpaceEchoProcessor::MinervaSpaceEchoProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "MinervaSpaceEcho", createLayout())
{
    const auto& specs = mse::paramSpecs();
    for (size_t i = 0; i < specs.size(); ++i)
        rawParams[i] = parameters.getRawParameterValue (specs[i].id);

    startTimerHz (10);
}

MinervaSpaceEchoProcessor::~MinervaSpaceEchoProcessor()
{
    stopTimer();
}

juce::AudioProcessorValueTreeState::ParameterLayout MinervaSpaceEchoProcessor::createLayout()
{
    // Built from the engine's parameter table: IDs match mse-render presets.
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (const auto& s : mse::paramSpecs())
    {
        const juce::ParameterID id { s.id, 1 };
        switch (s.type)
        {
            case mse::ParamType::Float:
                layout.add (std::make_unique<juce::AudioParameterFloat> (
                    id, s.name, rangeFor (s), s.def,
                    juce::AudioParameterFloatAttributes().withLabel (s.unit).withAutomatable (s.automatable)));
                break;
            case mse::ParamType::Int:
                layout.add (std::make_unique<juce::AudioParameterInt> (
                    id, s.name, static_cast<int> (s.min), static_cast<int> (s.max), static_cast<int> (s.def),
                    juce::AudioParameterIntAttributes().withLabel (s.unit).withAutomatable (s.automatable)));
                break;
            case mse::ParamType::Bool:
                layout.add (std::make_unique<juce::AudioParameterBool> (
                    id, s.name, s.def > 0.5f, juce::AudioParameterBoolAttributes().withAutomatable (s.automatable)));
                break;
            case mse::ParamType::Choice:
            {
                juce::StringArray choices;
                for (int c = 0; c < s.numChoices; ++c)
                    choices.add (s.choices[c]);
                layout.add (std::make_unique<juce::AudioParameterChoice> (
                    id, s.name, choices, static_cast<int> (s.def),
                    juce::AudioParameterChoiceAttributes().withAutomatable (s.automatable)));
                break;
            }
        }
    }
    return layout;
}

mse::EngineParams MinervaSpaceEchoProcessor::readParams() const noexcept
{
    mse::ParamValues v {};
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = rawParams[i]->load (std::memory_order_relaxed);
    return mse::paramsFromValues (v);
}

bool MinervaSpaceEchoProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void MinervaSpaceEchoProcessor::applyMemoryConfig()
{
    mse::MemoryConfig cfg = engine.getMemoryConfig();
    cfg.capacity = readParams().capacity;
    engine.setMemoryConfig (cfg); // no-op when unchanged
}

void MinervaSpaceEchoProcessor::timerCallback()
{
    // Message thread: structural changes (memory reallocation) and freeing
    // memory the audio thread has let go of.
    applyMemoryConfig();
    engine.collectGarbage();
}

void MinervaSpaceEchoProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    applyMemoryConfig();
    engine.setParams (readParams());
    engine.prepare (sampleRate, samplesPerBlock, getTotalNumOutputChannels());
    fallbackPpq = 0.0;
}

double MinervaSpaceEchoProcessor::getTailLengthSeconds() const
{
    const auto p = readParams();
    const double bpm = std::max (20.0, uiClock.bpm.load());
    const double trace = p.syncMode == mse::SyncMode::Tempo
                             ? mse::divisionQuarters (p.traceDivision, 4, 4) * 60.0 / bpm
                             : p.traceMs * 0.001;
    // One repeat without feedback; a generous allowance with it.
    return std::min (60.0, trace * (p.feedback > 0.0f ? 8.0 : 1.0));
}

mse::HostClock MinervaSpaceEchoProcessor::readHostClock()
{
    mse::HostClock clock;
    if (auto* ph = getPlayHead())
    {
        if (const auto pos = ph->getPosition())
        {
            if (const auto bpm = pos->getBpm())
            {
                clock.hasTempo = true;
                clock.bpm = *bpm;
            }
            if (const auto ppq = pos->getPpqPosition())
                clock.ppqPosition = *ppq;
            if (const auto sig = pos->getTimeSignature())
            {
                clock.timeSigNumerator = sig->numerator;
                clock.timeSigDenominator = sig->denominator;
            }
            clock.isPlaying = pos->getIsPlaying();
        }
    }
    if (! clock.isPlaying)
        clock.ppqPosition = fallbackPpq;
    return clock;
}

void MinervaSpaceEchoProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (auto ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    const auto clock = readHostClock();
    // Keep a free-running position so the engine always has a clock, even with
    // the transport stopped (plan §6).
    fallbackPpq = clock.ppqPosition + buffer.getNumSamples() / getSampleRate() * clock.bpm / 60.0;

    engine.setParams (readParams());
    engine.process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples(), clock);

    uiClock.hasTempo.store (clock.hasTempo, std::memory_order_relaxed);
    uiClock.isPlaying.store (clock.isPlaying, std::memory_order_relaxed);
    uiClock.bpm.store (clock.bpm, std::memory_order_relaxed);
    uiClock.ppq.store (clock.ppqPosition, std::memory_order_relaxed);
    uiClock.sigNum.store (clock.timeSigNumerator, std::memory_order_relaxed);
    uiClock.sigDen.store (clock.timeSigDenominator, std::memory_order_relaxed);
}

mse::HostClock MinervaSpaceEchoProcessor::getClockForUi() const noexcept
{
    mse::HostClock c;
    c.hasTempo = uiClock.hasTempo.load (std::memory_order_relaxed);
    c.isPlaying = uiClock.isPlaying.load (std::memory_order_relaxed);
    c.bpm = uiClock.bpm.load (std::memory_order_relaxed);
    c.ppqPosition = uiClock.ppq.load (std::memory_order_relaxed);
    c.timeSigNumerator = uiClock.sigNum.load (std::memory_order_relaxed);
    c.timeSigDenominator = uiClock.sigDen.load (std::memory_order_relaxed);
    return c;
}

juce::AudioProcessorEditor* MinervaSpaceEchoProcessor::createEditor()
{
    return new MinervaSpaceEchoEditor (*this);
}

void MinervaSpaceEchoProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    state.setProperty ("stateVersion", kStateVersion, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void MinervaSpaceEchoProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // Parameters missing from older states keep their defaults.
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (parameters.state.getType()))
            parameters.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MinervaSpaceEchoProcessor();
}
