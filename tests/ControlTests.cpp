// Stage 7: memory view, per-trace commands, trigger parameters, importing
// audio, MIDI note map and presets.
#include "TestHelpers.h"

#include "mse/Import.h"
#include "mse/MidiMap.h"
#include "mse/Preset.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace mse;
using namespace mse::test;

namespace {

constexpr double kSr = 48000.0;
constexpr int kSeg = 24000; // 500 ms traces
constexpr double kPi = 3.14159265358979323846;

AudioBuffer tone (double hz, int samples = kSeg, float amp = 0.3f)
{
    auto b = silence (2, samples);
    for (int i = 0; i < samples; ++i)
        b.channels[0][static_cast<size_t> (i)] = b.channels[1][static_cast<size_t> (i)]
            = amp * static_cast<float> (std::sin (2.0 * kPi * hz * i / kSr));
    return b;
}

EngineParams freeParams()
{
    auto p = wetOnly();
    p.syncMode = SyncMode::Free;
    p.traceMs = 500.0f;
    return p;
}

// Processes a little silence so the engine publishes a fresh view.
constexpr int kViewRun = 2048; // > 1/30 s
const MemoryView& view (EchoEngine& e)
{
    run (e, silence (2, kViewRun));
    const auto* v = e.readMemoryView();
    REQUIRE (v != nullptr);
    return *v;
}

std::vector<std::vector<float>> channelsOf (const AudioBuffer& b) { return b.channels; }

} // namespace

TEST_CASE ("Memory view mirrors memory and shows which trace answers")
{
    auto p = freeParams();
    p.selfMatch = false;
    p.power = 9.0f;
    EchoEngine e;
    e.enableMemoryView();
    prepare (e, p, 10);
    run (e, tone (440));
    run (e, tone (660));
    run (e, tone (880));
    run (e, tone (660, 4800)); // segment 4 starts: the first three are stored
    const auto& v = view (e);
    REQUIRE (v.count == 3);
    REQUIRE (v.capacity == 10);
    REQUIRE (v.rows[0].serial < v.rows[1].serial);
    REQUIRE (v.rows[1].serial < v.rows[2].serial);
    REQUIRE (v.rows[0].age == 2);
    REQUIRE (v.rows[2].age == 0);
    REQUIRE (v.rows[0].seconds == Catch::Approx (0.5).margin (0.01));
    // Each row carries its trace's address.
    int nonZero = 0;
    for (int j = 0; j < kFeatureSize; ++j)
        nonZero += v.thumb (0)[j] != 0 ? 1 : 0;
    REQUIRE (nonZero > kFeatureSize / 4);

    // Now cue with 440: the 440 trace answers and plays on head 1.
    run (e, tone (440, kSeg - 4800 - kViewRun));
    run (e, silence (2, 2048)); // the 440 cue has been retrieved
    const auto& v2 = view (e);
    REQUIRE (v2.count == 4);
    int best = 0;
    for (int i = 1; i < v2.count; ++i)
        if (v2.rows[static_cast<size_t> (i)].activation > v2.rows[static_cast<size_t> (best)].activation)
            best = i;
    REQUIRE (best == 0);
    REQUIRE (v2.rows[0].play[0] > 0.0f);
    FeatureVector echo;
    REQUIRE (v2.echoContent (echo));
    // With power 9 the 440 Hz trace dominates: the echo looks like it.
    std::array<float, kFeatureSize> row0 {};
    for (int j = 0; j < kFeatureSize; ++j)
        row0[static_cast<size_t> (j)] = v2.thumb (0)[j];
    REQUIRE (similarity (echo, row0, Similarity::Cosine) > 0.9);
    REQUIRE (v2.intensity > 0.0f);
}

TEST_CASE ("Traces can be clamped, unclamped and deleted by serial")
{
    auto p = freeParams();
    p.clampBudget = 0.2f; // 2 of 10
    EchoEngine e;
    e.enableMemoryView();
    prepare (e, p, 10);
    for (double hz : { 300.0, 400.0, 500.0, 600.0 })
        run (e, tone (hz));
    run (e, silence (2, 512));
    auto v = view (e);
    REQUIRE (v.count == 4);
    const auto s0 = v.rows[0].serial, s1 = v.rows[1].serial, s2 = v.rows[2].serial;

    e.sendCommand (Command::ClampTrace, s0);
    e.sendCommand (Command::ClampTrace, s2);
    e.sendCommand (Command::ClampTrace, s1); // over the clamp budget: refused
    v = view (e);
    REQUIRE (v.rows[0].clamped);
    REQUIRE_FALSE (v.rows[1].clamped);
    REQUIRE (v.rows[2].clamped);
    REQUIRE (v.clampLimit == 2);

    e.sendCommand (Command::UnclampTrace, s0);
    e.sendCommand (Command::DeleteTrace, s1);
    e.sendCommand (Command::DeleteTrace, 999999); // unknown: ignored
    v = view (e);
    REQUIRE (v.count == 3);
    REQUIRE_FALSE (v.rows[0].clamped);
    REQUIRE (v.rows[1].serial == s2);
    REQUIRE (e.getStats().tracesStored == 3);
}

TEST_CASE ("Deleting the trace that is playing silences its echo")
{
    auto p = freeParams();
    EchoEngine e;
    e.enableMemoryView();
    prepare (e, p, 1); // a delay
    run (e, tone (440));
    const auto early = run (e, silence (2, 4800));
    REQUIRE (rms (early) > 0.1);
    const auto serial = e.readMemoryView() ? view (e).rows[0].serial : 0;
    REQUIRE (serial > 0);
    e.sendCommand (Command::DeleteTrace, serial);
    const auto late = run (e, silence (2, 12000));
    REQUIRE (rms (slice (late, 2000, 10000)) < 1.0e-6);
}

TEST_CASE ("Trigger parameters fire memory actions on their rising edge")
{
    auto p = freeParams();
    EchoEngine e;
    prepare (e, p, 10);
    run (e, tone (440));
    run (e, tone (550));
    run (e, silence (2, 512));
    REQUIRE (e.getStats().tracesStored == 2);

    p.trigger[4] = true; // Clear All
    e.setParams (p);
    run (e, silence (2, 512));
    REQUIRE (e.getStats().tracesStored == 0);

    // Held on: no further clearing.
    run (e, tone (440, kSeg - 512));
    run (e, tone (660));
    run (e, silence (2, 512));
    REQUIRE (e.getStats().tracesStored == 2);

    p.trigger[1] = true; // Clamp All
    e.setParams (p);
    run (e, silence (2, 512));
    REQUIRE (e.getStats().clamped == 2);

    p.trigger[4] = false;
    e.setParams (p);
    run (e, silence (2, 512));
    p.trigger[4] = true; // a new edge
    e.setParams (p);
    run (e, silence (2, 512));
    REQUIRE (e.getStats().tracesStored == 0);
}

TEST_CASE ("Audio is cut into traces with the same addresses as live recording")
{
    AudioBuffer a;
    append (a, tone (440));
    append (a, tone (660));
    append (a, tone (880));
    append (a, tone (990, kSeg / 2)); // a half trace: kept
    append (a, tone (220, kSeg / 10)); // with the half trace: one 0.6-trace piece
    ImportSettings is;
    is.traceSeconds = 0.5;
    const auto snap = tracesFromAudio (channelsOf (a), kSr, kSr, is);
    REQUIRE (snap.traces.size() == 4); // 3 full, then 0.5 + 0.1 of a trace = one 0.6 piece

    // Compare with what the engine stores when the same audio is played in.
    auto p = freeParams();
    EchoEngine e;
    prepare (e, p, 10);
    run (e, slice (a, 0, 3 * kSeg));
    run (e, silence (2, 512));
    MemorySnapshot live;
    REQUIRE (e.takeSnapshot (live));
    REQUIRE (live.traces.size() == 3);
    for (size_t i = 0; i < 3; ++i)
    {
        REQUIRE (similarity (snap.traces[i].features, live.traces[i].features, Similarity::Cosine) > 0.95);
        REQUIRE (snap.traces[i].rms == Catch::Approx (live.traces[i].rms).epsilon (0.02));
    }

    // Resampled imports keep their length in seconds.
    const auto at44 = tracesFromAudio (channelsOf (a), kSr, 44100.0, is);
    REQUIRE (at44.sampleRate == 44100.0);
    REQUIRE (at44.traces[0].length() == 22050);

    // Silence is skipped.
    const auto quiet = tracesFromAudio (channelsOf (silence (2, 3 * kSeg)), kSr, kSr, is);
    REQUIRE (quiet.traces.empty());
}

TEST_CASE ("Imported traces join memory as the newest and can answer live cues")
{
    auto p = freeParams();
    p.selfMatch = false;
    p.power = 9.0f;
    EchoEngine e;
    e.enableMemoryView();
    prepare (e, p, 10);
    run (e, tone (300));
    run (e, tone (350));
    run (e, silence (2, 512));
    REQUIRE (view (e).count == 2);

    AudioBuffer a;
    append (a, tone (440));
    append (a, tone (660));
    append (a, tone (880));
    ImportSettings is;
    is.traceSeconds = 0.5;
    e.importTraces (tracesFromAudio (channelsOf (a), kSr, kSr, is));
    const auto& v = view (e);
    REQUIRE (v.count == 5);
    for (int i = 1; i < 5; ++i)
        REQUIRE (v.rows[static_cast<size_t> (i)].serial > v.rows[static_cast<size_t> (i - 1)].serial);

    // Play 660 Hz: the imported 660 trace (row 3) answers.
    run (e, silence (2, kSeg - 512 - 2 * kViewRun));
    run (e, tone (660));
    const auto out = run (e, silence (2, kSeg - kViewRun));
    const auto& v2 = view (e);
    int best = 0;
    for (int i = 1; i < v2.count; ++i)
        if (v2.rows[static_cast<size_t> (i)].activation > v2.rows[static_cast<size_t> (best)].activation)
            best = i;
    REQUIRE (best == 3);
    REQUIRE (correlation (slice (out, 2000, 20000), slice (tone (660), 2000, 20000)) > 0.5);
}

TEST_CASE ("Importing into full memory keeps clamped traces, then the imports")
{
    auto p = freeParams();
    EchoEngine e;
    e.enableMemoryView();
    prepare (e, p, 4);
    for (double hz : { 300.0, 400.0, 500.0 })
        run (e, tone (hz));
    run (e, silence (2, 512));
    auto v = view (e);
    REQUIRE (v.count == 3);
    const auto firstSerial = v.rows[0].serial;
    e.sendCommand (Command::ClampTrace, firstSerial);
    view (e);

    AudioBuffer a;
    for (double hz : { 600.0, 700.0, 800.0 })
        append (a, tone (hz));
    ImportSettings is;
    is.traceSeconds = 0.5;
    is.clamp = false;
    e.importTraces (tracesFromAudio (channelsOf (a), kSr, kSr, is));
    v = view (e);
    REQUIRE (v.count == 4);
    REQUIRE (v.rows[0].serial == firstSerial);
    REQUIRE (v.rows[0].clamped);
    for (int i = 1; i < 4; ++i)
        REQUIRE (v.rows[static_cast<size_t> (i)].serial > firstSerial + 2);
}

TEST_CASE ("Imports before prepare() are kept")
{
    AudioBuffer a;
    append (a, tone (440));
    append (a, tone (660));
    ImportSettings is;
    is.traceSeconds = 0.5;
    is.clamp = true;
    EchoEngine e;
    e.importTraces (tracesFromAudio (channelsOf (a), kSr, kSr, is));
    e.importTraces (tracesFromAudio (channelsOf (tone (880)), kSr, kSr, is));
    e.enableMemoryView();
    auto p = freeParams();
    p.clampBudget = 1.0f;
    prepare (e, p, 10);
    const auto& v = view (e);
    REQUIRE (v.count == 3);
    REQUIRE (v.rows[2].clamped);
    REQUIRE (v.rows[2].serial > v.rows[1].serial);
}

TEST_CASE ("MIDI note map")
{
    REQUIRE (midiMappingFor (36, 36).action == MidiAction::Capture);
    REQUIRE (midiMappingFor (37, 36).action == MidiAction::Freeze);
    REQUIRE (midiMappingFor (43, 36).action == MidiAction::ClearAll);
    REQUIRE (midiMappingFor (44, 36).action == MidiAction::EchoChain);
    REQUIRE (midiMappingFor (45, 36).action == MidiAction::None);
    REQUIRE (midiMappingFor (48, 36).action == MidiAction::ModeSelector);
    REQUIRE (midiMappingFor (48, 36).value == 0);
    REQUIRE (midiMappingFor (56, 36).value == 8);
    REQUIRE (midiMappingFor (57, 36).action == MidiAction::None);
    REQUIRE (midiMappingFor (35, 36).action == MidiAction::None);
    char name[8];
    midiNoteName (36, name, sizeof (name));
    REQUIRE (std::string (name) == "C1");
    midiNoteName (61, name, sizeof (name));
    REQUIRE (std::string (name) == "C#3");
}

TEST_CASE ("Presets written by the plugin read back identically")
{
    auto v = defaultParamValues();
    v[kPower] = 6.5f;
    v[kCapacity] = 250;
    v[kBlendDomain] = 1;
    v[kSelfMatch] = 0;
    v[kEchoLevelDb] = -3.25f;
    v[kEmbedMemory] = 1;       // session setting: not written
    v[kTriggerClearAll] = 1;   // action: not written
    const auto text = writePreset (v, "My preset\nsecond line");
    REQUIRE (text.find ("embed_memory") == std::string::npos);
    REQUIRE (text.find ("trigger_") == std::string::npos);
    REQUIRE (presetDescription (text) == "My preset second line");

    auto back = defaultParamValues();
    applyPresetText (back, text);
    for (int i = 0; i < kNumParams; ++i)
        if (! isSessionParam (i))
            REQUIRE (back[static_cast<size_t> (i)] == v[static_cast<size_t> (i)]);
}

TEST_CASE ("Every example and factory preset parses and has a description")
{
    const std::string dir = MSE_PRESET_DIR;
    for (const auto* sub : { "/examples", "/factory" })
    for (const auto& entry : std::filesystem::directory_iterator (dir + sub))
    {
        auto v = defaultParamValues();
        std::vector<TimedAssignment> timed;
        REQUIRE_NOTHROW (applyPresetFile (v, entry.path().string(), &timed));
        std::ifstream in (entry.path());
        std::stringstream s;
        s << in.rdbuf();
        INFO (entry.path());
        REQUIRE_FALSE (presetDescription (s.str()).empty());
    }
}
