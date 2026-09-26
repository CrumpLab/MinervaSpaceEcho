#pragma once

#include "PluginProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>

// Stage 0 editor: JUCE's generic parameter UI plus a status line showing the
// host clock the engine receives. A custom UI arrives in Stage 7.
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
    juce::Label status;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MinervaSpaceEchoEditor)
};
