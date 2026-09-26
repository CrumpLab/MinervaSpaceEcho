#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>

using namespace mse;
using namespace mse::test;

namespace {

testgen::Options bars (int n)
{
    testgen::Options o;
    o.bars = n;
    return o;
}

constexpr int kBar = 96000; // 1 bar @ 120 BPM, 48 kHz

} // namespace

TEST_CASE ("Echo off is a bit-exact pass-through")
{
    EchoEngine engine;
    auto p = EngineParams {};
    p.echoLevelDb = kLevelOffDb;
    prepare (engine, p, 100);

    const auto in = testgen::fullMix (bars (3));
    const auto out = run (engine, in, { 333 });
    REQUIRE (out.channels == in.channels);
}

TEST_CASE ("Capacity 1 is exactly a delay (free-running)")
{
    const int block = GENERATE (64, 333, 512, 4096);
    CAPTURE (block);

    EchoEngine engine;
    auto p = wetOnly();
    p.syncMode = SyncMode::Free;
    p.traceMs = 250.0f;
    prepare (engine, p, 1, 48000.0, block);

    const int L = 12000;
    const auto in = testgen::fullMix (bars (2));
    const auto out = run (engine, in, { block });

    for (int c = 0; c < 2; ++c)
    {
        const auto& x = in.channels[static_cast<size_t> (c)];
        const auto& y = out.channels[static_cast<size_t> (c)];
        for (int i = 0; i < L; ++i)
            REQUIRE (y[static_cast<size_t> (i)] == 0.0f);
        for (int i = L; i < in.numSamples(); ++i)
            REQUIRE (y[static_cast<size_t> (i)] == Catch::Approx (x[static_cast<size_t> (i - L)]).margin (1e-5));
    }
}

TEST_CASE ("Capacity 1 is exactly a delay (tempo-synced, 1 bar)")
{
    EchoEngine engine;
    prepare (engine, wetOnly(), 1);

    const auto in = testgen::fullMix (bars (4));
    const auto out = run (engine, in, { 480, 120.0 });
    for (int i = kBar; i < in.numSamples(); ++i)
        REQUIRE (out.channels[0][static_cast<size_t> (i)]
                 == Catch::Approx (in.channels[0][static_cast<size_t> (i - kBar)]).margin (1e-5));
}

TEST_CASE ("Capacity 1 with feedback repeats with geometric decay")
{
    EchoEngine engine;
    auto p = wetOnly();
    p.syncMode = SyncMode::Free;
    p.traceMs = 100.0f; // 4800 samples
    p.feedback = 0.5f;
    prepare (engine, p, 1);

    // A short noise burst (an isolated impulse is too sparse for the features).
    auto in = silence (1, 4800 * 8);
    uint32_t r = 12345;
    for (int i = 100; i < 600; ++i)
    {
        r = r * 1664525u + 1013904223u;
        in.channels[0][static_cast<size_t> (i)] = (static_cast<float> (r >> 8) / 8388608.0f - 1.0f) * 0.5f;
    }
    const auto out = run (engine, in, { 256 });

    float expectedGain = 1.0f;
    for (int k = 1; k <= 5; ++k)
    {
        for (int i = 100; i < 600; ++i)
            REQUIRE (out.channels[0][static_cast<size_t> (i + k * 4800)]
                     == Catch::Approx (in.channels[0][static_cast<size_t> (i)] * expectedGain).margin (1e-5));
        expectedGain *= 0.5f;
    }
}

TEST_CASE ("Self-match off with capacity 1 gives silence")
{
    EchoEngine engine;
    auto p = wetOnly();
    p.selfMatch = false;
    prepare (engine, p, 1);
    const auto out = run (engine, testgen::fullMix (bars (3)));
    REQUIRE (rms (out) == 0.0);
}

TEST_CASE ("Encoding failure of 1 erases every address: no echo")
{
    EchoEngine engine;
    auto p = wetOnly();
    p.encodingFailure = 1.0f;
    prepare (engine, p, 10);
    const auto out = run (engine, testgen::fullMix (bars (3)));
    REQUIRE (rms (out) == 0.0);
}

TEST_CASE ("Memory fills FIFO up to capacity")
{
    EchoEngine engine;
    auto p = wetOnly();
    p.syncMode = SyncMode::Free;
    p.traceMs = 100.0f;
    prepare (engine, p, 3);
    run (engine, testgen::fullMix (bars (1)), { 512 }); // 2 s = 20 segments
    const auto st = engine.getStats();
    REQUIRE (st.capacity == 3);
    REQUIRE (st.tracesStored == 3);
    REQUIRE (st.segments == 19); // the 20th segment ends exactly at the end of input, and is stored at the next block
}

namespace {

// Bars A B C A' (A' identical to A) followed by one bar of silence. Returns
// the echo heard during the silent bar, i.e. memory's answer to the cue A'.
tools::AudioBuffer answerToRepeatedBar (float power, bool selfMatch = false)
{
    const auto o = bars (1);
    const auto a = testgen::drums (o);
    const auto b = testgen::chordsBass (o);
    const auto c = testgen::melody (o);
    tools::AudioBuffer in;
    append (in, a);
    append (in, b);
    append (in, c);
    append (in, a);
    append (in, silence (2, kBar));

    EchoEngine engine;
    auto p = wetOnly();
    p.power = power;
    p.selfMatch = selfMatch;
    prepare (engine, p, 10);
    const auto out = run (engine, in);
    return slice (out, 4 * kBar, kBar);
}

} // namespace

TEST_CASE ("A repeated bar retrieves its earlier trace; higher power sharpens the choice")
{
    const auto drumsBar = testgen::drums (bars (1));
    const double sharp = correlation (answerToRepeatedBar (9.0f), drumsBar);
    const double blurry = correlation (answerToRepeatedBar (1.0f), drumsBar);
    INFO ("power 9 corr " << sharp << ", power 1 corr " << blurry);
    REQUIRE (sharp > 0.97);
    REQUIRE (blurry < sharp);
}

TEST_CASE ("Transport jumps re-sync the grid and discard short partial segments")
{
    EchoEngine engine;
    prepare (engine, wetOnly(), 10);

    auto chunk = testgen::fullMix (bars (2));
    run (engine, slice (chunk, 0, kBar + kBar / 4), { 512, 120.0, 0.0 }); // 1.25 bars
    REQUIRE (engine.getStats().segments == 1);

    // Jump back to the start: the quarter-bar partial is too short to store.
    run (engine, slice (chunk, 0, kBar), { 512, 120.0, 0.0 });
    REQUIRE (engine.getStats().segments == 1);
    // ...and the next bar line commits the bar that followed the jump.
    run (engine, slice (chunk, 0, 1024), { 512, 120.0, 4.0 });
    REQUIRE (engine.getStats().segments == 2);
}

TEST_CASE ("Starting mid-bar keeps echoes aligned to the bar grid")
{
    EchoEngine engine;
    prepare (engine, wetOnly(), 1);

    // Start at beat 3 of bar 1 (ppq 2). A noise burst at beat 4 should echo at
    // beat 4 of the next bar: exactly one bar later.
    auto in = silence (1, 3 * kBar);
    const int burst = 24000;
    uint32_t r = 7;
    for (int i = burst; i < burst + 2000; ++i)
    {
        r = r * 1664525u + 1013904223u;
        in.channels[0][static_cast<size_t> (i)] = (static_cast<float> (r >> 8) / 8388608.0f - 1.0f) * 0.5f;
    }
    const auto out = run (engine, in, { 512, 120.0, 2.0 });
    for (int i = burst; i < burst + 2000; ++i)
        REQUIRE (out.channels[0][static_cast<size_t> (i + kBar)]
                 == Catch::Approx (in.channels[0][static_cast<size_t> (i)]).margin (1e-5));
}

TEST_CASE ("Memory config changes are applied at the next block, and clear works")
{
    EchoEngine engine;
    prepare (engine, wetOnly(), 100);
    run (engine, testgen::fullMix (bars (3)));
    REQUIRE (engine.getStats().tracesStored == 2);

    MemoryConfig m;
    m.capacity = 5;
    engine.setMemoryConfig (m);
    run (engine, silence (2, 512));
    engine.collectGarbage();
    REQUIRE (engine.getStats().capacity == 5);
    REQUIRE (engine.getStats().tracesStored == 0); // Stage 1: rebuilding clears memory

    run (engine, testgen::fullMix (bars (2)));
    REQUIRE (engine.getStats().tracesStored > 0);
    engine.requestClear();
    run (engine, silence (2, 512));
    REQUIRE (engine.getStats().tracesStored == 0);
}

TEST_CASE ("Cue gate: near-silent segments evoke no echo")
{
    const auto o = bars (2);
    auto quiet = testgen::fullMix (o);
    for (auto& ch : quiet.channels)
        for (float& x : ch)
            x *= 0.0005f; // ~ -70 dBFS
    tools::AudioBuffer in;
    append (in, testgen::fullMix (o));
    append (in, quiet);
    append (in, silence (2, kBar));

    for (float gate : { -60.0f, kCueGateOffDb })
    {
        EchoEngine engine;
        auto p = wetOnly();
        p.cueGateDb = gate;
        p.levelTracking = 0.0f; // memories at their own level, so the ungated case is audible
        prepare (engine, p, 100);
        const auto out = run (engine, in);
        const double echoOfQuiet = rms (slice (out, 3 * kBar, 2 * kBar)); // cued by the quiet bars
        if (gate > kCueGateOffDb)
            REQUIRE (echoOfQuiet == 0.0);
        else
            REQUIRE (echoOfQuiet > 0.01); // ungated: level-independent features recall loud bars
    }
}

TEST_CASE ("Level tracking: echoes follow the cue's level, so feedback decays with a full memory")
{
    tools::AudioBuffer in;
    append (in, testgen::fullMix (bars (4)));
    append (in, silence (2, 12 * kBar));

    auto tailLevel = [&] (float tracking) {
        EchoEngine engine;
        auto p = wetOnly();
        p.feedback = 0.5f;
        p.levelTracking = tracking;
        prepare (engine, p, 100);
        const auto out = run (engine, in);
        return rms (slice (out, 14 * kBar, 2 * kBar)); // 10 bars after the input stopped
    };
    const double tracked = tailLevel (1.0f);
    const double untracked = tailLevel (0.0f);
    INFO ("tracked " << tracked << ", untracked " << untracked);
    REQUIRE (tracked < 0.01 * untracked); // ~0.5^10 decay vs. sustained memories
    REQUIRE (untracked > 0.01);
}

TEST_CASE ("Level tracking: a quiet cue gives a proportionally quiet echo")
{
    const auto o = bars (1);
    const auto loud = testgen::drums (o);
    auto quiet = loud;
    for (auto& ch : quiet.channels)
        for (float& x : ch)
            x *= 0.1f;
    tools::AudioBuffer in;
    append (in, loud);
    append (in, quiet);
    append (in, silence (2, kBar));

    EchoEngine engine;
    auto p = wetOnly();
    p.selfMatch = false; // the quiet bar can only recall the loud one
    prepare (engine, p, 10);
    const auto out = run (engine, in);
    // Echo of the quiet bar (heard in bar 3) recalls the loud bar at the quiet bar's level.
    REQUIRE (rms (slice (out, 2 * kBar, kBar)) == Catch::Approx (rms (quiet)).epsilon (0.01));
}

TEST_CASE ("Familiarity normalisation: an unfamiliar cue gives a faint echo (with level tracking)")
{
    // 8 bars of style A, then style B. With self-match off, the first B bar
    // is unfamiliar (quiet echo); the second B bar can recall the first (loud).
    EchoEngine engine;
    auto p = wetOnly();
    p.selfMatch = false;
    p.normalization = Normalization::Familiarity;
    p.featureMode = FeatureMode::Ternary;
    prepare (engine, p, 100);
    const auto out = run (engine, testgen::styleChange (bars (16)));
    const double unfamiliar = rms (slice (out, 9 * kBar, kBar));  // cued by bar 9 (first B)
    const double familiar = rms (slice (out, 10 * kBar, kBar));   // cued by bar 10
    INFO ("unfamiliar " << unfamiliar << ", familiar " << familiar);
    REQUIRE (unfamiliar < 0.25 * familiar);
}

TEST_CASE ("Heavy feedback stays finite and bounded")
{
    EchoEngine engine;
    auto p = EngineParams {};
    p.feedback = 1.2f;
    p.syncMode = SyncMode::Free;
    p.traceMs = 300.0f;
    prepare (engine, p, 50);

    auto in = testgen::fullMix (bars (2));
    append (in, silence (2, 8 * kBar));
    const auto out = run (engine, in);
    for (const auto& ch : out.channels)
        for (float x : ch)
        {
            REQUIRE (std::isfinite (x));
            REQUIRE (std::abs (x) < 4.0f);
        }
}

TEST_CASE ("Stats report memory size and trace length")
{
    EchoEngine engine;
    prepare (engine, EngineParams {}, 100);
    run (engine, silence (2, 1024));
    const auto st = engine.getStats();
    REQUIRE (st.capacity == 100);
    REQUIRE (st.traceSeconds == Catch::Approx (2.0));
    REQUIRE (st.slotSeconds == Catch::Approx (20.0));
}
