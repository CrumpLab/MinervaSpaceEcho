#include "PluginEditor.h"

#include "mse/MidiMap.h"

namespace {

constexpr int kHeaderH = 44, kCaptionH = 18, kButtonsH = 32, kStatusH = 24, kSideW = 250;

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

juce::String cueText (const mse::EngineStats& st)
{
    switch (static_cast<mse::CueMode> (st.cueMode))
    {
        case mse::CueMode::Progressive:
            return "  |  cue: progressive";
        case mse::CueMode::Rolling:
            return st.windowTooLong ? juce::String ("  |  rolling: traces must be longer than the window!")
                                    : "  |  rolling cue " + juce::String (st.cueLatencyMs, 0) + " ms behind";
        case mse::CueMode::Segment:
            break;
    }
    return {};
}

juce::File defaultMemoryFolder()
{
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
        .getChildFile ("MINERVA Space Echo")
        .getChildFile ("Memories");
}

} // namespace

MinervaSpaceEchoEditor::MinervaSpaceEchoEditor (MinervaSpaceEchoProcessor& p)
    : AudioProcessorEditor (p), processor (p), params (p.getParameters())
{
    setLookAndFeel (&lookAndFeel);

    addAndMakeVisible (matrix);
    addAndMakeVisible (side);
    addAndMakeVisible (params);
    matrix.onSelect = [this] (uint64_t) { refresh(); };
    matrix.onLock = [this] (uint64_t serial, bool lock) {
        processor.sendCommand (lock ? mse::Command::ClampTrace : mse::Command::UnclampTrace, serial);
    };
    side.onLock = matrix.onLock;
    side.onDelete = [this] (uint64_t serial) {
        processor.sendCommand (mse::Command::DeleteTrace, serial);
        matrix.setSelected (0);
    };

    // Presets.
    addAndMakeVisible (presetBox);
    addAndMakeVisible (prevPreset);
    addAndMakeVisible (nextPreset);
    addAndMakeVisible (savePreset);
    presetBox.setTextWhenNothingSelected ("Presets");
    presetBox.onChange = [this] {
        const int index = presetBox.getSelectedId() - 1;
        if (index >= 0 && index < menuPresets.size())
        {
            const auto error = processor.applyPreset (menuPresets[index]);
            showMessage (error.isEmpty() ? "Preset: " + menuPresets[index].name : "Preset failed: " + error);
        }
    };
    prevPreset.onClick = [this] { stepPreset (-1); };
    nextPreset.onClick = [this] { stepPreset (1); };
    savePreset.onClick = [this] { savePresetDialog(); };
    prevPreset.setTooltip ("Previous preset");
    nextPreset.setTooltip ("Next preset");
    savePreset.setTooltip ("Save the current settings as a user preset (" + MinervaSpaceEchoProcessor::userPresetFolder().getFullPathName() + ")");
    rebuildPresetMenu();

    // Memory actions.
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
    clampLastButton.setTooltip ("Clamp (lock) the newest trace so it is never replaced");
    clampAllButton.setTooltip ("Clamp the newest traces, up to the clamp budget");
    clearUnclampedButton.setTooltip ("Forget every trace that isn't clamped");
    clearButton.setColour (juce::TextButton::buttonColourId, theme::danger.withAlpha (0.35f));

    for (auto* b : { &importButton, &saveButton, &loadButton })
        addAndMakeVisible (*b);
    importButton.onClick = [this] { chooseImportFiles(); };
    saveButton.onClick = [this] { chooseSaveFolder(); };
    loadButton.onClick = [this] { chooseLoadFolder(); };
    importButton.setTooltip ("Cut audio files into traces of the current length and add them to memory (or drop files on the plugin)");
    addAndMakeVisible (lockImports);
    lockImports.setToggleState (processor.getLockImports(), juce::dontSendNotification);
    lockImports.onClick = [this] { processor.setLockImports (lockImports.getToggleState()); };
    lockImports.setTooltip ("Clamp imported traces so new playing never replaces them");

    setResizable (true, true);
    setResizeLimits (860, 620, 2000, 1600);
    setSize (1040, 760);
    refresh();
    startTimerHz (30);
}

MinervaSpaceEchoEditor::~MinervaSpaceEchoEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

juce::String MinervaSpaceEchoEditor::midiNoteText() const
{
    const int base = static_cast<int> (processor.getParameters().getRawParameterValue ("midi_base_note")->load());
    juce::String text = "MIDI notes (VST3 and the standalone app; in Live, route a MIDI track to this plugin). "
                        "Every action is also a parameter you can MIDI-map or automate.\n";
    char name[16];
    for (int k = 0; k < mse::kMidiModeOffset + mse::kMidiModeCount; ++k)
    {
        const auto m = mse::midiMappingFor (base + k, base);
        if (m.action == mse::MidiAction::None)
            continue;
        mse::midiNoteName (base + k, name, sizeof (name));
        juce::String what = mse::midiActionName (m.action);
        if (m.action == mse::MidiAction::ModeSelector)
            what = "Mode " + juce::String (mse::paramSpecs()[mse::kModeSelector].choices[m.value]);
        text << name << " " << what << (k == 8 || k == mse::kMidiModeOffset + mse::kMidiModeCount - 1 ? "\n" : "   ");
    }
    return text;
}

void MinervaSpaceEchoEditor::rebuildPresetMenu()
{
    menuPresets = processor.getPresets();
    presetBox.clear (juce::dontSendNotification);
    auto* menu = presetBox.getRootMenu();
    juce::String category;
    const auto current = processor.getCurrentPresetName();
    int currentId = 0;
    for (int i = 0; i < menuPresets.size(); ++i)
    {
        const auto& p = menuPresets.getReference (i);
        if (p.category != category)
        {
            category = p.category;
            menu->addSectionHeader (category);
        }
        presetBox.addItem (p.name, i + 1);
        if (p.name == current)
            currentId = i + 1;
    }
    if (currentId > 0)
        presetBox.setSelectedId (currentId, juce::dontSendNotification);
    else if (current.isNotEmpty())
        presetBox.setText (current, juce::dontSendNotification);
}

void MinervaSpaceEchoEditor::stepPreset (int delta)
{
    if (menuPresets.isEmpty())
        return;
    const int n = menuPresets.size();
    int index = presetBox.getSelectedId() - 1;
    index = index < 0 ? (delta > 0 ? 0 : n - 1) : (index + delta + n) % n;
    presetBox.setSelectedId (index + 1, juce::sendNotificationSync);
}

void MinervaSpaceEchoEditor::savePresetDialog()
{
    dialog = std::make_unique<juce::AlertWindow> ("Save preset", "Saved as a text file in " + MinervaSpaceEchoProcessor::userPresetFolder().getFullPathName()
                                                                     + " (mse-render reads the same format).",
                                                  juce::MessageBoxIconType::NoIcon, this);
    dialog->setLookAndFeel (&lookAndFeel);
    dialog->addTextEditor ("name", processor.getCurrentPresetName(), "Name");
    dialog->addTextEditor ("description", "", "Description");
    dialog->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MinervaSpaceEchoEditor> safe (this);
    dialog->enterModalState (true, juce::ModalCallbackFunction::create ([safe] (int result) {
        if (safe == nullptr || safe->dialog == nullptr)
            return;
        const auto name = safe->dialog->getTextEditorContents ("name");
        const auto description = safe->dialog->getTextEditorContents ("description");
        safe->dialog.reset();
        if (result != 1)
            return;
        const auto error = safe->processor.saveUserPreset (name, description);
        safe->rebuildPresetMenu();
        safe->showMessage (error.isEmpty() ? "Saved preset " + name : error);
    }), false);
}

void MinervaSpaceEchoEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    // Header.
    g.setColour (theme::panel);
    g.fillRect (header);
    auto h = header.reduced (14, 0);
    g.setColour (theme::accent);
    g.setFont (juce::FontOptions (19.0f, juce::Font::bold));
    const juce::String title ("MINERVA SPACE ECHO");
    g.drawText (title, h, juce::Justification::centredLeft);
    const int titleW = juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (19.0f, juce::Font::bold), title);
    g.setColour (theme::textDim);
    g.setFont (juce::FontOptions (12.0f));
    const auto subtitle = h.withTrimmedLeft (titleW + 12).withRight (prevPreset.getX() - 8);
    if (subtitle.getWidth() > 170)
        g.drawText ("multiple-trace memory echo", subtitle, juce::Justification::centredLeft);

    // MIDI activity and version, far right.
    auto right = header.reduced (14, 0).removeFromRight (120);
    g.setFont (juce::FontOptions (11.0f));
    g.setColour (theme::textDim);
    g.drawText (juce::String ("v") + mse::versionString(), right, juce::Justification::centredRight);
    const auto dot = juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ static_cast<float> (right.getX() + 30), static_cast<float> (right.getCentreY()) });
    g.setColour (midiFlash > 0 ? theme::head2 : theme::panelLight);
    g.fillEllipse (dot);
    g.setColour (theme::textDim);
    g.drawText ("MIDI", dot.toNearestInt().withWidth (40).translated (12, -4).withHeight (16), juce::Justification::centredLeft);

    // Matrix captions.
    auto c = matrixCaption;
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.setColour (theme::textDim);
    auto cols = c.withWidth (matrix.getWidth());
    cols.removeFromLeft (MemoryMatrix::kLockW + MemoryMatrix::kGap);
    g.drawText ("ACTIVATION", cols.removeFromLeft (MemoryMatrix::kActW), juce::Justification::centredLeft);
    cols.removeFromLeft (MemoryMatrix::kGap);
    {
        auto heads = cols.removeFromLeft (MemoryMatrix::kHeadsW);
        for (int hd = 0; hd < mse::kNumHeads; ++hd)
        {
            g.setColour (theme::headColour (hd));
            g.drawText (juce::String (hd + 1), heads.getX() + 2 + hd * 10, heads.getY(), 10, heads.getHeight(),
                        juce::Justification::centred);
        }
        g.setColour (theme::textDim);
    }
    cols.removeFromLeft (MemoryMatrix::kGap);
    const juce::FontOptions statsFont (12.0f);
    const int statsW = juce::GlyphArrangement::getStringWidthInt (statsFont, memoryText) + 12;
    cols.setRight (c.getRight() - statsW);
    const juce::String address (sequenceView ? "ADDRESS  [n-1 | n]  (each: 16 time slots x 24 bands)"
                                             : "ADDRESS  (16 time slots x 24 bands)");
    g.drawText (juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (11.0f, juce::Font::bold), address) < cols.getWidth()
                    ? address : juce::String ("ADDRESS"),
                cols, juce::Justification::centredLeft);
    g.setFont (statsFont);
    g.setColour (theme::text);
    g.drawText (memoryText, c, juce::Justification::centredRight);

    // Status bar with the segment's progress.
    g.setColour (theme::panel);
    g.fillRect (statusArea);
    const auto bar = statusArea.toFloat().removeFromTop (3.0f);
    g.setColour (theme::panelLight);
    g.fillRect (bar);
    g.setColour (theme::accent);
    g.fillRect (bar.withWidth (bar.getWidth() * phase));
    auto s = statusArea.reduced (10, 0).withTrimmedTop (3);
    g.setFont (juce::FontOptions (12.0f));
    g.setColour (message.isNotEmpty() ? theme::accent : theme::textDim);
    g.drawText (message.isNotEmpty() ? message : clockText, s, juce::Justification::centredLeft);
}

void MinervaSpaceEchoEditor::paintOverChildren (juce::Graphics& g)
{
    if (! dragging)
        return;
    const auto r = matrixArea.toFloat().reduced (4.0f);
    g.setColour (theme::background.withAlpha (0.8f));
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (theme::accent);
    g.drawRoundedRectangle (r, 8.0f, 2.0f);
    const auto st = processor.getStats();
    g.setFont (juce::FontOptions (16.0f));
    g.drawFittedText ("Drop to add the audio to memory\ncut into traces of " + juce::String (st.traceSeconds, 2) + " s"
                          + (lockImports.getToggleState() ? ", clamped" : ""),
                      r.toNearestInt(), juce::Justification::centred, 3);
}

void MinervaSpaceEchoEditor::resized()
{
    auto area = getLocalBounds();
    header = area.removeFromTop (kHeaderH);
    statusArea = area.removeFromBottom (kStatusH);

    // Presets in the header, centred-right.
    auto presetRow = header.reduced (0, 8).withTrimmedRight (150);
    presetRow = presetRow.removeFromRight (juce::jmin (430, presetRow.getWidth() / 2));
    savePreset.setBounds (presetRow.removeFromRight (70));
    presetRow.removeFromRight (6);
    nextPreset.setBounds (presetRow.removeFromRight (28));
    prevPreset.setBounds (presetRow.removeFromLeft (28));
    presetBox.setBounds (presetRow.reduced (4, 0));

    area.reduce (8, 6);
    const int paramsH = juce::jlimit (230, 330, area.getHeight() * 2 / 5);
    params.setBounds (area.removeFromBottom (paramsH));
    area.removeFromBottom (6);

    auto buttons = area.removeFromBottom (kButtonsH);
    area.removeFromBottom (6);
    const int bw = juce::jmax (80, (buttons.getWidth() - 110) / 9);
    for (auto* b : { &captureButton, &clampLastButton, &clampAllButton, &unclampButton, &clearUnclampedButton, &clearButton })
        b->setBounds (buttons.removeFromLeft (bw).reduced (2, 2));
    buttons.removeFromLeft (10);
    lockImports.setBounds (buttons.removeFromRight (110).reduced (4, 2));
    const int fw = juce::jmin (bw + 16, buttons.getWidth() / 3);
    for (auto* b : { &loadButton, &saveButton, &importButton })
        b->setBounds (buttons.removeFromRight (fw).reduced (2, 2));

    matrixArea = area;
    side.setBounds (area.removeFromRight (kSideW));
    area.removeFromRight (8);
    matrixCaption = area.removeFromTop (kCaptionH);
    matrix.setBounds (area);
}

void MinervaSpaceEchoEditor::showMessage (const juce::String& text)
{
    message = text;
    messageTicks = 30 * 8; // ~8 s
    repaint (statusArea);
}

bool MinervaSpaceEchoEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (MinervaSpaceEchoProcessor::isImportableFile (f))
            return true;
    return false;
}

void MinervaSpaceEchoEditor::fileDragEnter (const juce::StringArray&, int, int)
{
    dragging = true;
    repaint();
}

void MinervaSpaceEchoEditor::fileDragExit (const juce::StringArray&)
{
    dragging = false;
    repaint();
}

void MinervaSpaceEchoEditor::filesDropped (const juce::StringArray& files, int, int)
{
    dragging = false;
    repaint();
    importFiles (files);
}

void MinervaSpaceEchoEditor::importFiles (const juce::StringArray& files)
{
    juce::StringArray audio;
    for (const auto& f : files)
        if (MinervaSpaceEchoProcessor::isImportableFile (f))
            audio.add (f);
    if (audio.isEmpty())
        return;
    showMessage ("Importing " + juce::String (audio.size()) + (audio.size() == 1 ? " file..." : " files..."));
    juce::Component::SafePointer<MinervaSpaceEchoEditor> safe (this);
    processor.importAudioFiles (audio, [safe] (juce::String msg) {
        if (safe != nullptr)
            safe->showMessage (msg);
    });
}

void MinervaSpaceEchoEditor::chooseImportFiles()
{
    chooser = std::make_unique<juce::FileChooser> ("Add audio to memory", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                   "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.m4a;*.caf");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this] (const juce::FileChooser& fc) {
                              juce::StringArray paths;
                              for (const auto& f : fc.getResults())
                                  paths.add (f.getFullPathName());
                              importFiles (paths);
                          });
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

void MinervaSpaceEchoEditor::refresh()
{
    if (const auto* view = processor.readMemoryView())
    {
        sequenceView = view->sequence;
        matrix.update (*view);
        side.update (*view, matrix.getSelected());
    }

    const auto st = processor.getStats();
    memoryText = juce::String (st.tracesStored) + " / " + juce::String (st.capacity) + " traces, "
                 + juce::String (st.clamped) + " clamped   |   last: " + outcomeText (st.lastWrite)
                 + (st.captureArmed ? " [capture armed]" : "") + "   |   echo: " + juce::String (st.activeTraces)
                 + (st.activeTraces == 1 ? " trace" : " traces");

    const auto clock = processor.getClockForUi();
    const auto quartersPerBar = 4.0 * clock.timeSigNumerator / juce::jmax (1, clock.timeSigDenominator);
    const auto bar = static_cast<int> (std::floor (clock.ppqPosition / juce::jmax (0.25, quartersPerBar))) + 1;
    clockText = "trace " + juce::String (st.traceSeconds, 3) + " s   |   "
                + (clock.hasTempo ? juce::String (clock.bpm, 1) + " BPM" : juce::String ("no host tempo")) + "  "
                + juce::String (clock.timeSigNumerator) + "/" + juce::String (clock.timeSigDenominator) + "  bar "
                + juce::String (bar) + (clock.isPlaying ? " (playing)" : " (stopped)") + "   |   slot max "
                + juce::String (st.slotSeconds, 1) + " s   |   evicted " + juce::String (static_cast<juce::int64> (st.evictions))
                + ", merged " + juce::String (static_cast<juce::int64> (st.merges)) + cueText (st);

    if (messageTicks > 0 && --messageTicks == 0)
        message.clear();

    const auto count = processor.getMidiCount();
    if (count != lastMidiCount)
    {
        lastMidiCount = count;
        midiFlash = 6;
    }
    else if (midiFlash > 0)
    {
        --midiFlash;
    }

    const int base = static_cast<int> (processor.getParameters().getRawParameterValue ("midi_base_note")->load());
    if (base != shownBaseNote)
    {
        shownBaseNote = base;
        params.setPageNote ("Control", midiNoteText());
    }

    params.updateRelevance();
    phase = st.segmentPhase;
    repaint (header);
    repaint (matrixCaption);
    repaint (statusArea);
}

void MinervaSpaceEchoEditor::selectRow (int row)
{
    if (const auto* view = processor.readMemoryView(); view != nullptr && row >= 0 && row < view->count)
        matrix.setSelected (view->rows[static_cast<size_t> (row)].serial);
}
