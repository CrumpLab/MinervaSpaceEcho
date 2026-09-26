#pragma once

#include "mse/EchoEngine.h"

#include <juce_audio_processors/juce_audio_processors.h>

// JUCE wrapper around mse::EchoEngine. All DSP lives in the engine; this class
// only maps host parameters, transport and state onto it.
class MinervaSpaceEchoProcessor final : public juce::AudioProcessor,
                                        private juce::Timer
{
public:
    MinervaSpaceEchoProcessor();
    ~MinervaSpaceEchoProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getParameters() noexcept { return parameters; }

    // Thread-safe accessors for the editor.
    mse::HostClock getClockForUi() const noexcept;
    mse::EngineStats getStats() const noexcept { return engine.getStats(); }
    void clearMemory() noexcept { engine.requestClear(); }

    static constexpr int kStateVersion = 2;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    mse::HostClock readHostClock();
    mse::EngineParams readParams() const noexcept;
    void applyMemoryConfig();
    void timerCallback() override;

    juce::AudioProcessorValueTreeState parameters;
    std::array<std::atomic<float>*, mse::kNumParams> rawParams {};

    mse::EchoEngine engine;

    struct UiClock
    {
        std::atomic<bool> hasTempo { false }, isPlaying { false };
        std::atomic<double> bpm { 120.0 }, ppq { 0.0 };
        std::atomic<int> sigNum { 4 }, sigDen { 4 };
    } uiClock;

    double fallbackPpq = 0.0; // free-running position when the host gives none

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MinervaSpaceEchoProcessor)
};
