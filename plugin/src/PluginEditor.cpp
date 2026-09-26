#include "PluginEditor.h"

namespace {
constexpr int kStatusHeight = 64;
}

MinervaSpaceEchoEditor::MinervaSpaceEchoEditor (MinervaSpaceEchoProcessor& p)
    : AudioProcessorEditor (p), processor (p), generic (p)
{
    addAndMakeVisible (generic);
    for (auto* l : { &memoryStatus, &clockStatus })
    {
        addAndMakeVisible (*l);
        l->setJustificationType (juce::Justification::centredLeft);
        l->setFont (juce::FontOptions (13.0f));
    }
    addAndMakeVisible (clearButton);
    clearButton.onClick = [this] { processor.clearMemory(); };

    setResizable (true, true);
    setResizeLimits (420, 360, 1200, 1400);
    setSize (560, 640);
    startTimerHz (15);
}

MinervaSpaceEchoEditor::~MinervaSpaceEchoEditor()
{
    stopTimer();
}

void MinervaSpaceEchoEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    // Segment progress bar along the top of the status panel.
    auto bar = getLocalBounds().removeFromBottom (kStatusHeight).removeFromTop (4).toFloat();
    g.setColour (juce::Colours::grey.withAlpha (0.3f));
    g.fillRect (bar);
    g.setColour (juce::Colours::orange);
    g.fillRect (bar.withWidth (bar.getWidth() * phase));
}

void MinervaSpaceEchoEditor::resized()
{
    auto area = getLocalBounds();
    auto status = area.removeFromBottom (kStatusHeight).reduced (8, 6);
    status.removeFromTop (4);
    clearButton.setBounds (status.removeFromRight (120).reduced (0, 10));
    memoryStatus.setBounds (status.removeFromTop (status.getHeight() / 2));
    clockStatus.setBounds (status);
    generic.setBounds (area);
}

void MinervaSpaceEchoEditor::timerCallback()
{
    const auto st = processor.getStats();
    memoryStatus.setText (juce::String::formatted ("Memory %d / %d traces   (slot max %.1f s)   echo: %d traces, intensity %.2f",
                                                   st.tracesStored, st.capacity, st.slotSeconds, st.activeTraces,
                                                   static_cast<double> (st.intensity)),
                          juce::dontSendNotification);

    const auto clock = processor.getClockForUi();
    const auto quartersPerBar = 4.0 * clock.timeSigNumerator / juce::jmax (1, clock.timeSigDenominator);
    const auto bar = static_cast<int> (std::floor (clock.ppqPosition / juce::jmax (0.25, quartersPerBar))) + 1;
    clockStatus.setText (juce::String ("v") + mse::versionString() + "   trace " + juce::String (st.traceSeconds, 3) + " s   "
                             + (clock.hasTempo ? juce::String (clock.bpm, 1) + " BPM" : juce::String ("no host tempo"))
                             + "   " + juce::String (clock.timeSigNumerator) + "/" + juce::String (clock.timeSigDenominator)
                             + "   bar " + juce::String (bar) + (clock.isPlaying ? "  (playing)" : "  (stopped)"),
                         juce::dontSendNotification);

    if (std::abs (st.segmentPhase - phase) > 0.001f)
    {
        phase = st.segmentPhase;
        repaint (getLocalBounds().removeFromBottom (kStatusHeight).removeFromTop (4));
    }
}
