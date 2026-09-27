#pragma once

#include "ProbePanel.h"
#include "mse/MemoryView.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

// Draws a trace address (16 time slots x 24 bands) as a small spectrogram:
// time left to right, low bands at the bottom.
// Draws one address set (`set`: mse::AddressSet index) in its own layout.
void drawAddress (juce::Graphics& g, juce::Rectangle<float> area, const float* values, int set);
void drawAddress (juce::Graphics& g, juce::Rectangle<float> area, const int8_t* quantised, int set);
juce::String addressLayoutText (int set);

// The memory matrix (plan Stage 7): one row per memory slot, oldest at the
// top. Each row shows the trace's lock, its activation in the last retrieval,
// which heads are playing it, and its address as MINERVA stores it (the 384
// feature values, 16 slots of 24 bands, left to right).
class MemoryMatrix final : public juce::Component,
                           public juce::TooltipClient
{
public:
    MemoryMatrix();

    void update (const mse::MemoryView& view);
    void setSelected (uint64_t serial) { selected = serial; repaint(); }
    uint64_t getSelected() const noexcept { return selected; }

    std::function<void (uint64_t serial)> onSelect;
    std::function<void (uint64_t serial, bool lock)> onLock;
    std::function<void (uint64_t serial)> onAudition; // Alt-click on a row
    std::function<void (uint64_t serial)> onProbe;    // Shift-click on a row

    void setAuditioning (uint64_t serial) { if (serial != auditioning) { auditioning = serial; repaint(); } }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    juce::String getTooltip() override;

    static constexpr int kLockW = 16, kActW = 64, kHeadsW = 34, kGap = 4;

private:
    int rowAt (float y) const noexcept;
    float rowHeight() const noexcept;
    int displayRows() const noexcept; // rows the height is divided into
    juce::Rectangle<float> heatArea() const noexcept;

    std::vector<mse::TraceView> rows;
    int count = 0, capacity = 1, clampLimit = 0;
    float maxAct = 0.0f, maxPlay = 0.0f;
    juce::Image heat;                 // kSetSize (x2 with sequence context) x rows
    int shownSet = 0;                 // the address set the rows show
    std::vector<uint64_t> heatKeys;
    uint64_t selected = 0;
    int hoverRow = -1;
    bool sequence = false; // rows show [n-1 | n]
    uint64_t auditioning = 0;
    uint64_t probeSerial = 0; // probe view: activations are the probe's
};

// Beside the matrix: what cued memory, what came back, how familiar it was,
// and the selected trace.
class MemorySidePanel final : public juce::Component
{
public:
    MemorySidePanel();

    void update (const mse::MemoryView& view, uint64_t selectedSerial);

    std::function<void (uint64_t serial, bool lock)> onLock;
    std::function<void (uint64_t serial)> onDelete;
    // mode: 0 once, 1 loop, 2 [n-1 | n] pair; serial 0 = stop
    std::function<void (uint64_t serial, int mode)> onAudition;
    std::function<void (uint64_t serial)> onProbe;              // probe with this trace (0 = end the probe)
    std::function<void (mse::Command, uint64_t)> onCommand;     // probe controls

    void setAuditioning (uint64_t serial);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    uint64_t auditioning = 0;
    juce::TextButton auditionButton { "Audition" }, pairButton { "n-1 | n" };
    juce::ToggleButton loopToggle { "Loop" };
    void updateAuditionButtons();
    mse::SetVector heard {}, echo {}, heardBefore {}, echoContext {};
    int shownSet = 0;
    bool hasEcho = false, sequence = false, predicting = false;
    float intensity = 0.0f, maxActivation = 0.0f, intensityShown = 0.0f;
    bool hasSelection = false;
    mse::TraceView sel;
    std::array<int8_t, mse::kSetSize> selThumb {}, selContextThumb {};
    juce::TextButton lockButton { "Clamp" }, deleteButton { "Delete" }, probeButton { "Probe" };
    juce::Rectangle<int> heardArea, echoArea, meterArea, selArea;
    ProbePanel probePanel;
    uint64_t probeSerial = 0;
};
