// Headless checks of the plugin: parameters and (optionally embedded) memory
// survive getStateInformation -> setStateInformation into a fresh instance;
// MIDI notes, presets and audio import work (Stage 7).
#include "../src/PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <cstdio>

namespace {

void setParam (juce::AudioProcessor& p, const juce::String& id, float realValue)
{
    for (auto* param : p.getParameters())
        if (auto* withId = dynamic_cast<juce::RangedAudioParameter*> (param); withId && withId->getParameterID() == id)
        {
            withId->setValueNotifyingHost (withId->convertTo0to1 (realValue));
            return;
        }
    std::printf ("no parameter %s\n", id.toRawUTF8());
    std::exit (1);
}

float getParam (juce::AudioProcessor& p, const juce::String& id)
{
    for (auto* param : p.getParameters())
        if (auto* withId = dynamic_cast<juce::RangedAudioParameter*> (param); withId && withId->getParameterID() == id)
            return withId->convertFrom0to1 (withId->getValue());
    return NAN;
}

// Plays `seconds` of a chord through the processor; returns the output RMS
// (ignoring the first 50 ms, where parameter changes are still ramping).
double play (juce::AudioProcessor& p, double seconds, float amplitude = 0.3f, const juce::MidiBuffer* firstBlockMidi = nullptr)
{
    const int block = 512;
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    if (firstBlockMidi != nullptr)
        midi = *firstBlockMidi;
    double phase = 0.0, sum = 0.0;
    long n = 0;
    const long total = static_cast<long> (seconds * 48000.0);
    for (long done = 0; done < total; done += block)
    {
        for (int i = 0; i < block; ++i)
        {
            const float x = amplitude * static_cast<float> (std::sin (phase) + 0.5 * std::sin (1.5 * phase));
            phase += 2.0 * 3.14159265 * 220.0 / 48000.0;
            buf.setSample (0, i, x);
            buf.setSample (1, i, x);
        }
        p.processBlock (buf, midi);
        midi.clear();
        if (done < 2400)
            continue;
        for (int i = 0; i < block; ++i)
        {
            sum += buf.getSample (0, i) * buf.getSample (0, i);
            ++n;
        }
    }
    return std::sqrt (sum / std::max (1L, n));
}

int check (bool ok, const char* what)
{
    std::printf ("%s %s\n", ok ? "PASS" : "FAIL", what);
    return ok ? 0 : 1;
}

int run (bool embed)
{
    auto a = std::make_unique<MinervaSpaceEchoProcessor>();
    a->setPlayConfigDetails (2, 2, 48000.0, 512);
    a->prepareToPlay (48000.0, 512);
    setParam (*a, "sync_mode", 0.0f);   // Free
    setParam (*a, "trace_ms", 500.0f);
    setParam (*a, "power", 7.0f);
    setParam (*a, "embed_memory", embed ? 1.0f : 0.0f);
    play (*a, 3.0); // six 500 ms traces

    juce::MemoryBlock state;
    a->getStateInformation (state);

    auto b = std::make_unique<MinervaSpaceEchoProcessor>();
    b->setPlayConfigDetails (2, 2, 48000.0, 512);
    b->setStateInformation (state.getData(), static_cast<int> (state.getSize()));
    b->prepareToPlay (48000.0, 512);

    int failures = 0;
    failures += check (std::abs (getParam (*b, "power") - 7.0f) < 1e-3f, "power restored");
    failures += check (std::abs (getParam (*b, "trace_ms") - 500.0f) < 1.0f, "trace length restored");

    // With memory restored, a frozen instance still echoes; without, it is silent.
    setParam (*b, "freeze", 1.0f);
    setParam (*b, "dry_level_db", -60.0f);
    const double echo = play (*b, 1.0);
    if (embed)
        failures += check (echo > 0.05, "embedded memory restored (echo present)");
    else
        failures += check (echo < 1e-6, "memory not embedded by default (no echo)");
    std::printf ("  state %zu bytes, echo rms %.4f\n", static_cast<size_t> (state.getSize()), echo);
    return failures;
}

juce::MidiBuffer note (int number, bool on)
{
    juce::MidiBuffer m;
    m.addEvent (on ? juce::MidiMessage::noteOn (1, number, 0.8f) : juce::MidiMessage::noteOff (1, number), 0);
    return m;
}

std::unique_ptr<MinervaSpaceEchoProcessor> freshProcessor()
{
    auto p = std::make_unique<MinervaSpaceEchoProcessor>();
    p->setPlayConfigDetails (2, 2, 48000.0, 512);
    p->prepareToPlay (48000.0, 512);
    setParam (*p, "sync_mode", 0.0f);   // Free
    setParam (*p, "trace_ms", 500.0f);
    return p;
}

int runMidi()
{
    int failures = 0;
    auto p = freshProcessor();
    play (*p, 1.6);
    failures += check (p->getStats().tracesStored == 3, "three traces before MIDI");

    // Base note + 1 held: memory frozen while held.
    const auto held = note (37, true);
    play (*p, 1.0, 0.3f, &held);
    failures += check (p->getStats().tracesStored == 3 && p->getStats().lastWrite == mse::WriteOutcome::Frozen,
                       "held freeze note stops writing");
    const auto released = note (37, false);
    play (*p, 1.0, 0.3f, &released);
    failures += check (p->getStats().tracesStored > 3, "releasing it writes again");

    // Base note + 7: Clear All.
    const auto clear = note (43, true);
    play (*p, 0.1, 0.3f, &clear);
    failures += check (p->getStats().tracesStored == 0, "clear-all note clears memory");
    failures += check (p->getLastMidiNote() == 43, "last MIDI note reported");

    // MIDI control off: notes ignored.
    play (*p, 1.1);
    const int before = p->getStats().tracesStored;
    setParam (*p, "midi_control", 0.0f);
    play (*p, 0.1, 0.3f, &clear);
    failures += check (p->getStats().tracesStored == before && before > 0, "notes ignored with MIDI control off");

    // A different base note and channel filter.
    setParam (*p, "midi_control", 1.0f);
    setParam (*p, "midi_base_note", 60.0f);
    setParam (*p, "midi_channel", 2.0f);
    const auto wrongChannel = note (67, true); // channel 1
    play (*p, 0.1, 0.3f, &wrongChannel);
    failures += check (p->getStats().tracesStored == before, "other MIDI channels ignored");
    juce::MidiBuffer right;
    right.addEvent (juce::MidiMessage::noteOn (2, 67, 0.8f), 0);
    play (*p, 0.1, 0.3f, &right);
    failures += check (p->getStats().tracesStored == 0, "base note 60 + 7 on channel 2 clears");
    return failures;
}

int runPresets()
{
    int failures = 0;
    auto p = freshProcessor();
    setParam (*p, "power", 7.0f);
    setParam (*p, "embed_memory", 1.0f);
    const auto& presets = p->getPresets();
    int factory = 0, examples = 0;
    const MinervaSpaceEchoProcessor::Preset* tape = nullptr;
    for (const auto& preset : presets)
    {
        factory += preset.category == "Factory" ? 1 : 0;
        examples += preset.category == "Examples" ? 1 : 0;
        if (preset.name == "02 Tape Delay")
            tape = &preset;
    }
    std::printf ("  %d factory presets, %d examples\n", factory, examples);
    failures += check (factory >= 20 && examples >= 30 && tape != nullptr, "built-in presets present");
    if (tape == nullptr)
        return failures + 1;
    failures += check (p->applyPreset (*tape).isEmpty(), "preset applies");
    failures += check (std::abs (getParam (*p, "capacity") - 1.0f) < 1e-3f, "preset sets capacity");
    failures += check (std::abs (getParam (*p, "feedback") - 0.45f) < 1e-3f, "preset sets feedback");
    failures += check (std::abs (getParam (*p, "power") - 3.0f) < 1e-3f, "unmentioned settings return to defaults");
    failures += check (getParam (*p, "embed_memory") > 0.5f, "session settings are kept");
    failures += check (p->getCurrentPresetName() == "02 Tape Delay", "preset name remembered");

    // Every built-in preset applies cleanly.
    int bad = 0;
    for (const auto& preset : presets)
        if (preset.category != "User" && p->applyPreset (preset).isNotEmpty())
            ++bad;
    failures += check (bad == 0, "every built-in preset applies");

    // User presets round trip.
    setParam (*p, "power", 5.5f);
    const juce::String name = "mse state test preset";
    failures += check (p->saveUserPreset (name, "test").isEmpty(), "user preset saved");
    setParam (*p, "power", 2.0f);
    bool found = false;
    for (const auto& preset : p->getPresets())
        if (preset.category == "User" && preset.name == name)
        {
            found = true;
            p->applyPreset (preset);
            preset.file.deleteFile();
        }
    failures += check (found && std::abs (getParam (*p, "power") - 5.5f) < 1e-3f, "user preset loads back");
    p->rescanUserPresets();
    return failures;
}

int runImport()
{
    int failures = 0;
    // A 2.2 s stereo WAV at 44.1 kHz: two tones.
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("mse_import_test.wav");
    {
        juce::AudioBuffer<float> audio (2, 97020);
        for (int i = 0; i < audio.getNumSamples(); ++i)
        {
            const double hz = i < 44100 ? 440.0 : 660.0;
            const auto v = static_cast<float> (0.3 * std::sin (2.0 * 3.14159265 * hz * i / 44100.0));
            audio.setSample (0, i, v);
            audio.setSample (1, i, v);
        }
        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}
                                                       .withSampleRate (44100.0)
                                                       .withNumChannels (2)
                                                       .withBitsPerSample (24));
        writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    }
    mse::ImportSettings settings;
    settings.traceSeconds = 1.0;
    settings.clamp = true;
    juce::StringArray problems;
    int files = 0;
    const auto snap = MinervaSpaceEchoProcessor::decodeForImport ({ file.getFullPathName(), "/no/such/file.wav" }, settings,
                                                                  48000.0, problems, files);
    std::printf ("  imported %d traces from %d file(s), %d problem(s)\n", static_cast<int> (snap.traces.size()), files,
                 problems.size());
    failures += check (files == 1 && problems.size() == 1, "readable files decoded, unreadable reported");
    failures += check (snap.traces.size() == 2, "cut into 1 s traces (short tail dropped)");
    failures += check (snap.traces.size() == 2 && snap.traces[0].length() == 48000 && snap.traces[0].clamped,
                       "resampled to the engine rate and clamped");

    auto p = freshProcessor();
    setParam (*p, "clamp_budget", 1.0f);
    p->importTraces (snap);
    play (*p, 0.1, 0.0f);
    failures += check (p->getStats().tracesStored == 2 && p->getStats().clamped == 2, "imported traces are in memory");
    file.deleteFile();
    return failures;
}

// Offline bounces: hosts may send blocks larger than prepareToPlay announced,
// and may change the sample rate between renders.
int runOffline()
{
    int failures = 0;
    auto p = freshProcessor(); // prepared for 512-sample blocks
    p->setNonRealtime (true);
    juce::AudioBuffer<float> buf (2, 8192);
    juce::MidiBuffer midi;
    bool finite = true;
    for (int b = 0; b < 12; ++b)
    {
        for (int i = 0; i < buf.getNumSamples(); ++i)
            for (int c = 0; c < 2; ++c)
                buf.setSample (c, i, 0.3f * static_cast<float> (std::sin (0.05 * (b * 8192 + i))));
        p->processBlock (buf, midi);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < buf.getNumSamples(); ++i)
                finite = finite && std::isfinite (buf.getSample (c, i));
    }
    failures += check (finite, "8192-sample blocks with a 512-sample prepare");
    const int stored = p->getStats().tracesStored;
    failures += check (stored >= 3, "memory filled during the bounce");
    p->prepareToPlay (96000.0, 1024);
    failures += check (p->getStats().tracesStored == stored, "memory kept across a sample-rate change");
    return failures;
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    int failures = run (false) + run (true) + runMidi() + runPresets() + runImport() + runOffline();
    std::printf (failures == 0 ? "ALL PASSED\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
