#include "PluginProcessor.h"
#include "PluginEditor.h"

MinervaSpaceEchoProcessor::MinervaSpaceEchoProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "MinervaSpaceEcho", createLayout())
{
    outputGainDb = parameters.getRawParameterValue (kOutputGainId);
}

juce::AudioProcessorValueTreeState::ParameterLayout MinervaSpaceEchoProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { kOutputGainId, 1 }, "Output Gain",
        juce::NormalisableRange<float> (-60.0f, 12.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));
    return layout;
}

bool MinervaSpaceEchoProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void MinervaSpaceEchoProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.setParams ({ outputGainDb->load() });
    engine.prepare (sampleRate, samplesPerBlock, getTotalNumOutputChannels());
    fallbackPpq = 0.0;
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

    engine.setParams ({ outputGainDb->load() });
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
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (parameters.state.getType()))
            parameters.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MinervaSpaceEchoProcessor();
}
