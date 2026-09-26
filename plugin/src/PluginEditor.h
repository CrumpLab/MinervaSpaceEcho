#pragma once

#include "PluginProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>

// Stage 2 editor: JUCE's generic parameter UI, a memory/clock status panel and
// memory actions (capture, clamping, clearing, save/load). A custom UI arrives
// in Stage 7.
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
    void chooseSaveFolder();
    void chooseLoadFolder();
    void showMessage (const juce::String& text);

    MinervaSpaceEchoProcessor& processor;
    juce::GenericAudioProcessorEditor generic;
    juce::Label memoryStatus, clockStatus, message;

    juce::TextButton captureButton { "Capture" }, clampLastButton { "Clamp Last" }, clampAllButton { "Clamp All" },
        unclampButton { "Unclamp All" }, clearUnclampedButton { "Clear Unclamped" }, clearButton { "Clear All" },
        saveButton { "Save Memory..." }, loadButton { "Load Memory..." };

    std::unique_ptr<juce::FileChooser> chooser;
    float phase = 0.0f;
    int messageTicks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MinervaSpaceEchoEditor)
};
