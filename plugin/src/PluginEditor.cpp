#include "PluginEditor.h"

MinervaSpaceEchoEditor::MinervaSpaceEchoEditor (MinervaSpaceEchoProcessor& p)
    : AudioProcessorEditor (p), processor (p), generic (p)
{
    addAndMakeVisible (generic);
    addAndMakeVisible (status);
    status.setJustificationType (juce::Justification::centredLeft);
    status.setFont (juce::FontOptions (13.0f));

    setSize (420, 160);
    startTimerHz (10);
}

MinervaSpaceEchoEditor::~MinervaSpaceEchoEditor()
{
    stopTimer();
}

void MinervaSpaceEchoEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void MinervaSpaceEchoEditor::resized()
{
    auto area = getLocalBounds();
    status.setBounds (area.removeFromBottom (28).reduced (8, 4));
    generic.setBounds (area);
}

void MinervaSpaceEchoEditor::timerCallback()
{
    const auto clock = processor.getClockForUi();
    const auto quartersPerBar = 4.0 * clock.timeSigNumerator / juce::jmax (1, clock.timeSigDenominator);
    const auto bar = static_cast<int> (std::floor (clock.ppqPosition / juce::jmax (0.25, quartersPerBar))) + 1;
    status.setText (juce::String ("MINERVA Space Echo ") + mse::versionString() + "  |  "
                        + (clock.hasTempo ? juce::String (clock.bpm, 1) + " BPM" : juce::String ("no host tempo"))
                        + "  |  " + juce::String (clock.timeSigNumerator) + "/" + juce::String (clock.timeSigDenominator)
                        + "  |  bar " + juce::String (bar) + (clock.isPlaying ? "  (playing)" : "  (stopped)"),
                    juce::dontSendNotification);
}
