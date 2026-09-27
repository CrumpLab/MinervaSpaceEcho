#pragma once

#include "mse/EchoEngine.h"
#include "mse/MemoryView.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

// The probe (Stage 11): a trace's address used as a cue, for inspection.
// Shows every trace ranked by how strongly it answers, with its similarity
// under the current Address and under each address set, and plays the
// probe's echo soloed (raw, or through the echo's colouring).
class ProbePanel final : public juce::Component,
                         private juce::ListBoxModel
{
public:
    ProbePanel();

    void update (const mse::MemoryView& view);
    void setAuditioning (uint64_t serial);

    std::function<void (mse::Command, uint64_t)> onCommand;
    std::function<void (uint64_t serial)> onAuditionRow; // toggles an audition of that trace

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Row
    {
        mse::TraceView trace;
        bool isProbe = false;
        int rank = 0;
    };

    int getNumRows() override { return static_cast<int> (ranked.size()); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    juce::String getTooltipForRow (int row) override;
    void layoutColumns (juce::Rectangle<int> area, std::array<juce::Rectangle<int>, 9>& cols) const;

    juce::TextButton playButton { "Play echo" }, compareButton { "n vs n" }, closeButton { "Close" };
    juce::ToggleButton loopToggle { "Loop" }, fullToggle { "Full path" }, selfToggle { "Include itself" };
    juce::ListBox list { "Probe results", this };

    std::vector<Row> ranked;
    uint64_t probeSerial = 0, auditioning = 0;
    bool playing = false, next = false, includeSelf = false, sequence = false;
    int shownSet = 0, count = 0;
    float intensity = 0.0f, totalActivation = 0.0f;
    juce::Rectangle<int> titleArea, headerArea;
};
