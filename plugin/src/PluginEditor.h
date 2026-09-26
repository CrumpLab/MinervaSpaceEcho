#pragma once

#include "PluginProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>

// Stage 1 editor: JUCE's generic parameter UI, a memory/clock status panel and
// a Clear Memory button. A custom UI arrives in Stage 7.
class MinervaSpaceEchoEditor final : public juce::AudioProcessorEditor,
                                     private juce::Timer
{
public:
    explicit MinervaSpaceEchoEditor (MinervaSpaceEchoProcessor&);
    ~MinervaSpaceEchoEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    MinervaSpaceEchoProcessor& processor;
    juce::GenericAudioProcessorEditor generic;
    juce::Label memoryStatus, clockStatus;
    juce::TextButton clearButton { "Clear Memory" };
    float phase = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MinervaSpaceEchoEditor)
};
