#pragma once

#include "mse/EchoEngine.h"
#include "mse/Import.h"
#include "mse/MidiMap.h"

#include <juce_audio_processors/juce_audio_processors.h>

// JUCE wrapper around mse::EchoEngine. All DSP lives in the engine; this class
// only maps host parameters, MIDI, transport and state onto it, and offers the
// editor presets, memory import/export and the memory view.
class MinervaSpaceEchoProcessor final : public juce::AudioProcessor,
                                        private juce::Timer
{
public:
    MinervaSpaceEchoProcessor();
    ~MinervaSpaceEchoProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }   // MIDI note control
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getParameters() noexcept { return parameters; }

    // ---- editor (message thread) ----
    mse::HostClock getClockForUi() const noexcept;
    mse::EngineStats getStats() const noexcept { return engine.getStats(); }
    const mse::MemoryView* readMemoryView() noexcept { return engine.readMemoryView(); }
    void sendCommand (mse::Command c, uint64_t traceSerial = 0) noexcept { engine.sendCommand (c, traceSerial); }

    // Save writes on a background thread; `done` is called on the message
    // thread with an empty string on success, or an error.
    void saveMemory (const juce::File& folder, std::function<void (juce::String)> done);
    juce::String loadMemory (const juce::File& folderOrManifest); // empty on success

    // Cuts audio files into traces of the current length and adds them to
    // memory (decoding runs in the background). `done(message)` on the
    // message thread.
    void importAudioFiles (const juce::StringArray& paths, std::function<void (juce::String)> done);
    static bool isImportableFile (const juce::String& path);
    // The decoding half of an import (any thread): files -> traces.
    static mse::MemorySnapshot decodeForImport (const juce::StringArray& paths, const mse::ImportSettings& settings,
                                                double targetRate, juce::StringArray& problems, int& files);
    // Adds traces to memory (message thread).
    void importTraces (const mse::MemorySnapshot& traces) { engine.importTraces (traces); }
    bool getLockImports() const;
    void setLockImports (bool);

    // Presets: factory (built in), examples (the listening examples) and the
    // user's own, all in the `key = value` format mse-render reads.
    struct Preset
    {
        juce::String name, category, text;
        juce::File file;   // user presets
    };
    const juce::Array<Preset>& getPresets();   // factory, examples, user
    void rescanUserPresets();
    juce::String applyPreset (const Preset& preset);   // empty on success
    juce::String saveUserPreset (const juce::String& name, const juce::String& description); // empty on success
    juce::String getCurrentPresetName() const;
    static juce::File userPresetFolder();

    // MIDI activity for the editor: last note received (or -1) and a counter.
    int getLastMidiNote() const noexcept { return lastMidiNote.load(); }
    uint32_t getMidiCount() const noexcept { return midiCount.load(); }

    static constexpr int kStateVersion = 4;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    mse::HostClock readHostClock();
    mse::EngineParams readParams() const noexcept;
    void applyMemoryConfig();
    void handleMidi (const juce::MidiBuffer& midi, const mse::EngineParams& p) noexcept;
    void timerCallback() override;
    void setParamValue (int index, float realValue);

    juce::AudioProcessorValueTreeState parameters;
    std::array<std::atomic<float>*, mse::kNumParams> rawParams {};

    mse::EchoEngine engine;

    struct UiClock
    {
        std::atomic<bool> hasTempo { false }, isPlaying { false };
        std::atomic<double> bpm { 120.0 }, ppq { 0.0 };
        std::atomic<int> sigNum { 4 }, sigDen { 4 };
    } uiClock;

    double fallbackPpq = 0.0; // free-running position when the host gives none

    // MIDI (audio thread): held freezes and a Mode Selector choice that
    // applies at once and is written to the parameter by the timer.
    bool midiFreeze = false, midiSpectralFreeze = false, midiChain = false;
    int modeOverride = -1;
    double modeOverrideSamples = 0.0;
    std::atomic<int> pendingModeParam { -1 };
    std::atomic<int> lastMidiNote { -1 };
    std::atomic<uint32_t> midiCount { 0 };

    std::shared_ptr<std::atomic<bool>> aliveFlag = std::make_shared<std::atomic<bool>> (true);
    juce::Array<Preset> presets;
    bool presetsScanned = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MinervaSpaceEchoProcessor)
};
