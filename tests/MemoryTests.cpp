// Stage 2: memory management (write gates, full policies, clamping, forgetting,
// echo re-encoding, resizing, snapshots).
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace mse;
using namespace mse::test;

namespace {

constexpr int kBar = 96000; // 1 bar @ 120 BPM, 48 kHz

testgen::Options bars (int n)
{
    testgen::Options o;
    o.bars = n;
    return o;
}

// n bars of the full mix followed by one silent bar, so the last musical bar
// is committed within the run.
AudioBuffer music (int n)
{
    auto in = testgen::fullMix (bars (n));
    append (in, silence (2, kBar));
    return in;
}

// The same drum bar repeated n times (+ a silent bar).
AudioBuffer repeatedBar (int n)
{
    const auto bar = testgen::drums (bars (1));
    AudioBuffer in;
    for (int i = 0; i < n; ++i)
        append (in, bar);
    append (in, silence (2, kBar));
    return in;
}

// Short free-running traces over the full mix: many segments quickly.
EngineParams shortTraces()
{
    auto p = wetOnly();
    p.syncMode = SyncMode::Free;
    p.traceMs = 250.0f;
    return p;
}

struct Rig
{
    EchoEngine engine;
    Rig (const EngineParams& p, int capacity) { prepare (engine, p, capacity); }
    AudioBuffer operator() (const AudioBuffer& in) { return run (engine, in); }
    EngineStats stats() const { return engine.getStats(); }
    void command (Command c)
    {
        engine.sendCommand (c);
        run (engine, silence (2, 64));
    }
};

} // namespace

// ---- write gates -------------------------------------------------------------------

TEST_CASE ("Freeze: memory stops changing but still answers")
{
    auto p = wetOnly();
    Rig rig (p, 100);
    rig (music (2));
    REQUIRE (rig.stats().tracesStored == 2);

    p.freeze = true;
    rig.engine.setParams (p);
    const auto out = rig (music (3));
    REQUIRE (rig.stats().tracesStored == 2);
    REQUIRE (rig.stats().lastWrite == WriteOutcome::Frozen);
    REQUIRE (rms (slice (out, kBar, 2 * kBar)) > 0.01); // still echoing from memory
}

TEST_CASE ("Write gate: quiet segments are not stored")
{
    auto quiet = testgen::fullMix (bars (2));
    for (auto& ch : quiet.channels)
        for (float& x : ch)
            x *= 0.0001f; // -80 dB
    AudioBuffer in;
    append (in, testgen::fullMix (bars (2)));
    append (in, quiet);
    append (in, silence (2, kBar));

    Rig rig (wetOnly(), 100); // default write gate -70 dB
    rig (in);
    REQUIRE (rig.stats().tracesStored == 2);
}

TEST_CASE ("Novelty gate: Store Novel skips repeats, Store Familiar keeps them")
{
    auto p = wetOnly();
    p.noveltyMode = NoveltyMode::StoreNovel;
    Rig novel (p, 100);
    novel (repeatedBar (5));
    REQUIRE (novel.stats().tracesStored == 1);
    REQUIRE (novel.stats().lastWrite == WriteOutcome::Gated);

    p.noveltyMode = NoveltyMode::StoreFamiliar;
    Rig familiar (p, 100);
    AudioBuffer in;
    append (in, testgen::drums (bars (1)));
    append (in, testgen::chordsBass (bars (1))); // unfamiliar: not stored
    append (in, testgen::drums (bars (1)));      // familiar: stored
    append (in, silence (2, kBar));
    familiar (in);
    REQUIRE (familiar.stats().tracesStored == 2); // first bar (empty memory) + the repeat
}

TEST_CASE ("Write probability thins out what is stored (deterministic with a seed)")
{
    auto p = shortTraces();
    p.writeProbability = 0.5f;
    Rig rig (p, 1000);
    rig (music (4)); // 32 segments of music
    const int n = rig.stats().tracesStored;
    INFO (n);
    REQUIRE (n > 8);
    REQUIRE (n < 26);

    p.writeProbability = 0.0f;
    Rig none (p, 1000);
    none (music (2));
    REQUIRE (none.stats().tracesStored == 0);
}

TEST_CASE ("Manual mode stores only captured segments; capture also overrides freeze")
{
    auto p = wetOnly();
    p.writeMode = WriteMode::Manual;
    Rig rig (p, 100);
    rig (music (2));
    REQUIRE (rig.stats().tracesStored == 0);

    rig.engine.sendCommand (Command::Capture);
    rig (music (2));
    REQUIRE (rig.stats().tracesStored == 1);

    // Capture parameter: rising edge arms a capture.
    p.writeMode = WriteMode::Auto;
    p.freeze = true;
    p.capture = true;
    rig.engine.setParams (p);
    rig (music (2));
    REQUIRE (rig.stats().tracesStored == 2);
    rig (music (2)); // still held on: no new edge, frozen
    REQUIRE (rig.stats().tracesStored == 2);
}

// ---- when memory is full ------------------------------------------------------------------

TEST_CASE ("Full policies")
{
    SECTION ("Oldest skips clamped traces")
    {
        auto p = shortTraces();
        Rig rig (p, 4);
        rig (slice (testgen::fullMix (bars (1)), 0, 12000)); // one 250 ms segment
        rig (silence (2, 512));
        rig.command (Command::ClampLast);
        REQUIRE (rig.stats().clamped == 1);
        rig (music (1));
        REQUIRE (rig.stats().tracesStored == 4);
        REQUIRE (rig.stats().clamped == 1); // the clamped first trace survived
        REQUIRE (rig.stats().evictions > 0);
    }
    SECTION ("Reject keeps the first traces")
    {
        auto p = shortTraces();
        p.fullPolicy = FullPolicy::Reject;
        Rig rig (p, 3);
        rig (music (1));
        REQUIRE (rig.stats().tracesStored == 3);
        REQUIRE (rig.stats().evictions == 0);
        REQUIRE (rig.stats().rejections == 5); // 8 music segments, 3 stored
    }
    SECTION ("Everything clamped: new traces are rejected")
    {
        auto p = shortTraces();
        p.clampIncoming = true;
        p.clampBudget = 1.0f;
        Rig rig (p, 3);
        rig (music (1));
        REQUIRE (rig.stats().clamped == 3);
        REQUIRE (rig.stats().rejections == 5);
    }
    SECTION ("Random, Least Used and Weakest all evict")
    {
        for (auto policy : { FullPolicy::Random, FullPolicy::LeastUsed, FullPolicy::Weakest })
        {
            auto p = shortTraces();
            p.fullPolicy = policy;
            p.decayFadeDb = 0.5f;
            Rig rig (p, 5);
            rig (music (2));
            REQUIRE (rig.stats().tracesStored == 5);
            REQUIRE (rig.stats().evictions > 0);
        }
    }
    SECTION ("Merge Similar consolidates instead of evicting")
    {
        auto p = wetOnly();
        p.fullPolicy = FullPolicy::MergeSimilar;
        Rig rig (p, 2);
        rig (music (5));
        REQUIRE (rig.stats().tracesStored == 2);
        REQUIRE (rig.stats().merges == 3);
        REQUIRE (rig.stats().evictions == 0);
    }
}

TEST_CASE ("Consolidate Above merges near-duplicates even when memory has room")
{
    auto p = wetOnly();
    p.mergeThreshold = 0.95f;
    Rig rig (p, 100);
    rig (repeatedBar (4));
    REQUIRE (rig.stats().tracesStored == 1);
    REQUIRE (rig.stats().merges == 3);
}

// ---- clamping ------------------------------------------------------------------------------

TEST_CASE ("Clamp commands respect the clamp budget")
{
    auto p = shortTraces();
    p.clampBudget = 0.5f;
    Rig rig (p, 10);
    rig (music (1));
    REQUIRE (rig.stats().tracesStored == 8);

    rig.command (Command::ClampAll);
    REQUIRE (rig.stats().clamped == 5); // 50 % of capacity 10
    rig.command (Command::ClearUnclamped);
    REQUIRE (rig.stats().tracesStored == 5);
    rig.command (Command::UnclampAll);
    REQUIRE (rig.stats().clamped == 0);
    rig.command (Command::ClearAll);
    REQUIRE (rig.stats().tracesStored == 0);
}

// ---- forgetting -----------------------------------------------------------------------------

TEST_CASE ("Fading removes traces that fade out; clamped traces are protected")
{
    auto p = shortTraces();
    p.decayFadeDb = 6.0f; // -60 dB after 10 segments
    Rig rig (p, 100);
    rig (slice (testgen::fullMix (bars (1)), 0, 12000));
    rig (silence (2, 512));
    rig.command (Command::ClampLast);
    rig (music (2)); // 72 more segments (incl. silence)
    const auto st = rig.stats();
    INFO ("stored " << st.tracesStored);
    REQUIRE (st.tracesStored <= 11);   // only the last ~10 survive...
    REQUIRE (st.clamped == 1);         // ...plus the clamped first trace
}

TEST_CASE ("Forgetting features weakens recall until the echo disappears")
{
    auto p = wetOnly();
    p.decayForget = 0.2f;
    p.selfMatch = false;
    Rig rig (p, 100);
    AudioBuffer in;
    append (in, testgen::drums (bars (1)));
    for (int i = 0; i < 30; ++i)
        append (in, silence (2, kBar)); // time passes
    append (in, testgen::drums (bars (1)));
    append (in, silence (2, kBar));
    const auto out = rig (in);
    // After 30 segments with 20 % loss each, ~0.1 % of the address is left.
    REQUIRE (rms (slice (out, 32 * kBar, kBar)) < 0.02 * rms (testgen::drums (bars (1))));
}

TEST_CASE ("Tape dropouts silence parts of the stored audio")
{
    auto p = wetOnly();
    p.contentDropout = 1.0f;
    Rig all (p, 1);
    REQUIRE (rms (slice (all (music (2)), kBar, kBar)) < 1.0e-4);

    p.contentDropout = 0.5f;
    Rig half (p, 1);
    const double r = rms (slice (half (music (2)), kBar, kBar));
    const double full = rms (slice (testgen::fullMix (bars (1)), 0, kBar));
    INFO (r / full);
    REQUIRE (r > 0.3 * full);
    REQUIRE (r < 0.9 * full);
}

TEST_CASE ("Wear tone: zero is exact, higher values darken older traces")
{
    auto p = wetOnly();
    p.wearTone = 0.0f;
    Rig exact (p, 1);
    const auto in = testgen::fullMix (bars (2));
    const auto out = exact (in);
    REQUIRE (out.channels[0][static_cast<size_t> (kBar + 1000)] == Catch::Approx (in.channels[0][1000]).margin (1e-5));

    // Recall an old bar through heavy wear: less high-frequency energy (first differences).
    auto hf = [] (const AudioBuffer& b) {
        double s = 0;
        for (size_t i = 1; i < b.channels[0].size(); ++i)
        {
            const double d = b.channels[0][i] - b.channels[0][i - 1];
            s += d * d;
        }
        return s;
    };
    p.selfMatch = false;
    p.power = 9.0f;
    AudioBuffer seq;
    append (seq, testgen::drums (bars (1)));
    for (int i = 0; i < 6; ++i)
        append (seq, testgen::chordsBass (bars (1)));
    append (seq, testgen::drums (bars (1)));
    append (seq, silence (2, kBar));
    Rig clean (p, 100);
    p.wearTone = 1.0f;
    Rig worn (p, 100);
    const double hfClean = hf (slice (clean (seq), 8 * kBar, kBar));
    const double hfWorn = hf (slice (worn (seq), 8 * kBar, kBar));
    REQUIRE (hfWorn < 0.5 * hfClean);
}

// ---- echo re-encoding -------------------------------------------------------------------------

TEST_CASE ("Record Source = Echo: memory feeds on its own echo")
{
    auto p = wetOnly();
    p.recordSource = RecordSource::Echo;
    Rig rig (p, 1);
    const auto a = testgen::drums (bars (1));
    const auto b = testgen::chordsBass (bars (1));
    AudioBuffer in;
    append (in, a);
    append (in, b);
    append (in, b);
    append (in, silence (2, kBar));
    const auto out = rig (in);
    // Bar 1 (A) is stored from the input (no echo yet). Bars 2-3 record the
    // echo (A) instead of B, so memory keeps answering with A.
    REQUIRE (correlation (slice (out, 3 * kBar, kBar), a) > 0.99);
}

TEST_CASE ("Record Source = Input + Echo stores both")
{
    auto p = wetOnly();
    p.recordSource = RecordSource::InputAndEcho;
    Rig rig (p, 100);
    rig (music (4));
    // 4 music bars + 1 silent bar: inputs 1..4 stored, plus the echoes heard
    // during bars 2..4. (The echo heard in bar 5 would be stored at the
    // boundary after the input ends.)
    REQUIRE (rig.stats().tracesStored == 7);
}

// ---- resizing ----------------------------------------------------------------------------------

TEST_CASE ("Resizing keeps traces and the echo continues without a gap")
{
    EchoEngine engine;
    prepare (engine, wetOnly(), 100);
    run (engine, testgen::fullMix (bars (4)));
    run (engine, silence (2, 64)); // commit bar 4
    REQUIRE (engine.getStats().tracesStored == 4);

    MemoryConfig m;
    m.capacity = 2;
    m.budgetBytes = 256.0e6;
    engine.setMemoryConfig (m);
    const auto out = run (engine, slice (testgen::fullMix (bars (8)), 4 * kBar, kBar));
    engine.collectGarbage();
    REQUIRE (engine.getStats().capacity == 2);
    REQUIRE (engine.getStats().tracesStored == 2);
    // The echo of bar 4 plays straight through the resize.
    REQUIRE (rms (slice (out, 0, kBar / 2)) > 0.01);
}

// ---- snapshots ----------------------------------------------------------------------------------

TEST_CASE ("Snapshots round-trip through a folder and a blob, and reload identically")
{
    auto p = wetOnly();
    p.selfMatch = false;
    EchoEngine a;
    prepare (a, p, 100);
    run (a, music (4));
    a.sendCommand (Command::ClampLast);
    run (a, silence (2, 64));

    MemorySnapshot snap;
    REQUIRE (a.takeSnapshot (snap));
    REQUIRE (snap.traces.size() == 4);
    REQUIRE (snap.traces.back().clamped);
    snap.params = { { "power", 9.0f }, { "capacity", 100.0f } };

    const auto dir = (std::filesystem::temp_directory_path() / "mse_memory_test").string();
    std::filesystem::remove_all (dir);
    writeMemoryFolder (snap, dir);
    const auto fromFolder = readMemoryFolder (dir);
    const auto blob = serializeMemory (snap);
    const auto fromBlob = deserializeMemory (blob.data(), blob.size());
    std::filesystem::remove_all (dir);

    for (const auto* s : { &fromFolder, &fromBlob })
    {
        REQUIRE (s->traces.size() == snap.traces.size());
        for (size_t i = 0; i < snap.traces.size(); ++i)
        {
            REQUIRE (s->traces[i].audio == snap.traces[i].audio);
            REQUIRE (s->traces[i].features == snap.traces[i].features);
            REQUIRE (s->traces[i].clamped == snap.traces[i].clamped);
            REQUIRE (s->traces[i].serial == snap.traces[i].serial);
        }
        REQUIRE (s->params.size() == 2);
    }

    // A fresh engine loaded with the snapshot answers a cue like the original.
    const auto cue = slice (testgen::fullMix (bars (4)), 2 * kBar, kBar);
    AudioBuffer probeRun;
    append (probeRun, cue);
    append (probeRun, silence (2, kBar));

    EchoEngine b;
    b.loadSnapshot (fromBlob); // before prepare: applied at prepare
    prepare (b, p, 100);
    REQUIRE (b.getStats().tracesStored == 4);
    p.freeze = true;
    a.setParams (p);
    b.setParams (p);
    const auto outA = run (a, probeRun);
    const auto outB = run (b, probeRun);
    REQUIRE (correlation (slice (outA, kBar, kBar), slice (outB, kBar, kBar)) > 0.9999);
}

TEST_CASE ("Loading a snapshot into a running engine replaces memory at the next block")
{
    EchoEngine a;
    prepare (a, wetOnly(), 100);
    run (a, music (3));
    MemorySnapshot snap;
    REQUIRE (a.takeSnapshot (snap));

    EchoEngine b;
    prepare (b, wetOnly(), 100);
    run (b, music (1));
    REQUIRE (b.getStats().tracesStored == 1);
    b.loadSnapshot (snap);
    run (b, silence (2, 64));
    REQUIRE (b.getStats().tracesStored == 3);
}

TEST_CASE ("Snapshots resample to the engine's sample rate")
{
    EchoEngine a;
    prepare (a, wetOnly(), 100);
    run (a, music (2));
    MemorySnapshot snap;
    REQUIRE (a.takeSnapshot (snap));
    resampleSnapshot (snap, 96000.0);
    REQUIRE (snap.sampleRate == 96000.0);
    REQUIRE (snap.traces[0].length() == 2 * kBar);
}
