#include "PluginEditor.h"

namespace {

constexpr int kPanelHeight = 132;

const char* outcomeText (mse::WriteOutcome o)
{
    switch (o)
    {
        case mse::WriteOutcome::Stored:   return "stored";
        case mse::WriteOutcome::Merged:   return "merged";
        case mse::WriteOutcome::Gated:    return "gated";
        case mse::WriteOutcome::Rejected: return "rejected (full)";
        case mse::WriteOutcome::Frozen:   return "frozen";
        case mse::WriteOutcome::None:     break;
    }
    return "-";
}

juce::File defaultMemoryFolder()
{
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
        .getChildFile ("MINERVA Space Echo")
        .getChildFile ("Memories");
}

} // namespace

MinervaSpaceEchoEditor::MinervaSpaceEchoEditor (MinervaSpaceEchoProcessor& p)
    : AudioProcessorEditor (p), processor (p), generic (p)
{
    addAndMakeVisible (generic);
    for (auto* l : { &memoryStatus, &clockStatus, &message })
    {
        addAndMakeVisible (*l);
        l->setJustificationType (juce::Justification::centredLeft);
        l->setFont (juce::FontOptions (13.0f));
    }

    const std::pair<juce::TextButton*, mse::Command> commands[] = {
        { &captureButton, mse::Command::Capture },
        { &clampLastButton, mse::Command::ClampLast },
        { &clampAllButton, mse::Command::ClampAll },
        { &unclampButton, mse::Command::UnclampAll },
        { &clearUnclampedButton, mse::Command::ClearUnclamped },
        { &clearButton, mse::Command::ClearAll },
    };
    for (auto [button, command] : commands)
    {
        addAndMakeVisible (*button);
        button->onClick = [this, c = command] { processor.sendCommand (c); };
    }
    captureButton.setTooltip ("Store the segment in progress, bypassing write gates and freeze");
    clampLastButton.setTooltip ("Protect the newest trace from being replaced");
    clampAllButton.setTooltip ("Clamp the newest traces, up to the clamp budget");

    addAndMakeVisible (saveButton);
    addAndMakeVisible (loadButton);
    saveButton.onClick = [this] { chooseSaveFolder(); };
    loadButton.onClick = [this] { chooseLoadFolder(); };

    setResizable (true, true);
    setResizeLimits (480, 420, 1400, 1600);
    setSize (620, 720);
    startTimerHz (15);
}

MinervaSpaceEchoEditor::~MinervaSpaceEchoEditor()
{
    stopTimer();
}

void MinervaSpaceEchoEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    // Segment progress bar along the top of the panel.
    auto bar = getLocalBounds().removeFromBottom (kPanelHeight).removeFromTop (4).toFloat();
    g.setColour (juce::Colours::grey.withAlpha (0.3f));
    g.fillRect (bar);
    g.setColour (juce::Colours::orange);
    g.fillRect (bar.withWidth (bar.getWidth() * phase));
}

void MinervaSpaceEchoEditor::resized()
{
    auto area = getLocalBounds();
    auto panel = area.removeFromBottom (kPanelHeight).reduced (8, 6);
    panel.removeFromTop (4);
    generic.setBounds (area);

    memoryStatus.setBounds (panel.removeFromTop (20));
    clockStatus.setBounds (panel.removeFromTop (20));

    auto layoutRow = [] (juce::Rectangle<int> row, std::initializer_list<juce::Component*> items) {
        const int w = row.getWidth() / static_cast<int> (items.size());
        for (auto* c : items)
            c->setBounds (row.removeFromLeft (w).reduced (2, 2));
    };
    layoutRow (panel.removeFromTop (28), { &captureButton, &clampLastButton, &clampAllButton, &unclampButton });
    layoutRow (panel.removeFromTop (28), { &clearUnclampedButton, &clearButton, &saveButton, &loadButton });
    message.setBounds (panel);
}

void MinervaSpaceEchoEditor::showMessage (const juce::String& text)
{
    message.setText (text, juce::dontSendNotification);
    messageTicks = 15 * 8; // ~8 s
}

void MinervaSpaceEchoEditor::chooseSaveFolder()
{
    auto dir = defaultMemoryFolder();
    dir.createDirectory();
    const auto name = "Memory " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S");
    chooser = std::make_unique<juce::FileChooser> ("Save memory as a folder", dir.getChildFile (name));
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc) {
                              const auto target = fc.getResult();
                              if (target == juce::File())
                                  return;
                              showMessage ("Saving memory...");
                              juce::Component::SafePointer<MinervaSpaceEchoEditor> safe (this);
                              processor.saveMemory (target, [safe, target] (juce::String error) {
                                  if (safe != nullptr)
                                      safe->showMessage (error.isEmpty() ? "Saved memory to " + target.getFullPathName()
                                                                         : "Save failed: " + error);
                              });
                          });
}

void MinervaSpaceEchoEditor::chooseLoadFolder()
{
    chooser = std::make_unique<juce::FileChooser> ("Load a saved memory folder (or its manifest.json)",
                                                   defaultMemoryFolder(), "manifest.json");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories
                              | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc) {
                              const auto target = fc.getResult();
                              if (target == juce::File())
                                  return;
                              const auto error = processor.loadMemory (target);
                              showMessage (error.isEmpty() ? "Loaded memory from " + target.getFullPathName()
                                                           : "Load failed: " + error);
                          });
}

void MinervaSpaceEchoEditor::timerCallback()
{
    const auto st = processor.getStats();
    memoryStatus.setText (juce::String::formatted ("Memory %d / %d (%d clamped, slot max %.1f s)   last bar: %s%s   "
                                                   "echo: %d traces, intensity %.2f",
                                                   st.tracesStored, st.capacity, st.clamped, st.slotSeconds,
                                                   outcomeText (st.lastWrite), st.captureArmed ? "  [capture armed]" : "",
                                                   st.activeTraces, static_cast<double> (st.intensity)),
                          juce::dontSendNotification);

    const auto clock = processor.getClockForUi();
    const auto quartersPerBar = 4.0 * clock.timeSigNumerator / juce::jmax (1, clock.timeSigDenominator);
    const auto bar = static_cast<int> (std::floor (clock.ppqPosition / juce::jmax (0.25, quartersPerBar))) + 1;
    clockStatus.setText (juce::String ("v") + mse::versionString() + "   trace " + juce::String (st.traceSeconds, 3) + " s   "
                             + (clock.hasTempo ? juce::String (clock.bpm, 1) + " BPM" : juce::String ("no host tempo"))
                             + "   " + juce::String (clock.timeSigNumerator) + "/" + juce::String (clock.timeSigDenominator)
                             + "   bar " + juce::String (bar) + (clock.isPlaying ? "  (playing)" : "  (stopped)")
                             + "   evicted " + juce::String (static_cast<juce::int64> (st.evictions))
                             + ", merged " + juce::String (static_cast<juce::int64> (st.merges)),
                         juce::dontSendNotification);

    if (messageTicks > 0 && --messageTicks == 0)
        message.setText ({}, juce::dontSendNotification);

    if (std::abs (st.segmentPhase - phase) > 0.001f)
    {
        phase = st.segmentPhase;
        repaint (getLocalBounds().removeFromBottom (kPanelHeight).removeFromTop (4));
    }
}
