#pragma once

#include "MemoryMatrix.h"
#include "ParamPanel.h"
#include "PluginProcessor.h"
#include "Theme.h"

#include <juce_audio_processors/juce_audio_processors.h>

// Stage 7 editor: the memory matrix (traces as rows, live activations, which
// heads play what, locks), what cued memory and what came back, memory
// actions, presets, drag-and-drop import and tabbed parameter pages.
class MinervaSpaceEchoEditor final : public juce::AudioProcessorEditor,
                                     public juce::FileDragAndDropTarget,
                                     private juce::Timer
{
public:
    explicit MinervaSpaceEchoEditor (MinervaSpaceEchoProcessor&);
    ~MinervaSpaceEchoEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray& files, int, int) override;

    // Pulls the latest memory view and stats (called by the timer; public so
    // the headless snapshot tool can drive it).
    void refresh();
    void showMessage (const juce::String& text);
    void selectRow (int row); // for the snapshot tool

private:
    void timerCallback() override { refresh(); }
    void chooseSaveFolder();
    void chooseLoadFolder();
    void chooseImportFiles();
    void importFiles (const juce::StringArray& files);
    void rebuildPresetMenu();
    void stepPreset (int delta);
    void savePresetDialog();
    juce::String midiNoteText() const;

    MinervaSpaceEchoProcessor& processor;
    theme::LookAndFeel lookAndFeel;
    juce::TooltipWindow tooltips { this, 600 };

    MemoryMatrix matrix;
    MemorySidePanel side;
    ParamPanel params;

    juce::ComboBox presetBox;
    juce::TextButton prevPreset { "<" }, nextPreset { ">" }, savePreset { "Save..." };
    juce::Array<MinervaSpaceEchoProcessor::Preset> menuPresets;

    juce::TextButton captureButton { "Capture" }, clampLastButton { "Clamp Last" }, clampAllButton { "Clamp All" },
        unclampButton { "Unclamp All" }, clearUnclampedButton { "Clear Unclamped" }, clearButton { "Clear All" },
        importButton { "Import Audio..." }, saveButton { "Save Memory..." }, loadButton { "Load Memory..." };
    juce::ToggleButton lockImports { "Clamp imports" };
    juce::TextButton runButton { "Running" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> runAttachment;

    juce::Rectangle<int> header, matrixCaption, matrixArea, statusArea;
    juce::String memoryText, clockText, message;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::AlertWindow> dialog;
    float phase = 0.0f;
    int messageTicks = 0;
    bool dragging = false;
    uint32_t lastMidiCount = 0;
    int midiFlash = 0;
    int shownBaseNote = -1;
    bool sequenceView = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MinervaSpaceEchoEditor)
};
