// Stage 4: heads, chorus voices, sampling, cue sources, recency, feature focus,
// echo tone and familiarity modulation.
#include "TestHelpers.h"

#include "mse/Retrieval.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

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

AudioBuffer channel (const AudioBuffer& b, int c)
{
    AudioBuffer out;
    out.sampleRate = b.sampleRate;
    out.channels = { b.channels[static_cast<size_t> (c)] };
    return out;
}

// A bar of random noise bursts: shares nothing with the musical bars.
AudioBuffer noiseBursts()
{
    auto b = silence (2, kBar);
    uint32_t r = 4242;
    for (int burst = 0; burst < 12; ++burst)
    {
        r = r * 1664525u + 1013904223u;
        const int start = static_cast<int> (r % static_cast<uint32_t> (kBar - 4000));
        for (int i = 0; i < 3000; ++i)
        {
            r = r * 1664525u + 1013904223u;
            const float x = (static_cast<float> (r >> 8) / 8388608.0f - 1.0f) * 0.4f;
            b.channels[0][static_cast<size_t> (start + i)] += x;
            b.channels[1][static_cast<size_t> (start + i)] += x;
        }
    }
    return b;
}

// Six unrelated bars (drums, chords, melody, noise, style B, clicks), so each
// echo can be traced back to exactly one input bar.
AudioBuffer distinctBars()
{
    const auto o = bars (1);
    auto styleB = testgen::styleChange (bars (2));
    AudioBuffer in;
    append (in, testgen::drums (o));
    append (in, testgen::chordsBass (o));
    append (in, testgen::melody (o));
    append (in, noiseBursts());
    append (in, slice (styleB, kBar, kBar));
    append (in, testgen::impulses (o));
    return in;
}

} // namespace

// ---- heads -------------------------------------------------------------------------------

TEST_CASE ("Mode selector 2: head 2 alone is a two-segment delay")
{
    auto p = wetOnly();
    p.modeSelector = ModeSelector::H2;
    p.headLevelDb[1] = 0.0f;
    p.power = 9.0f;
    EchoEngine engine;
    prepare (engine, p, 10);
    const auto in = distinctBars();
    const auto out = run (engine, in);
    for (int bar = 3; bar < 6; ++bar)
    {
        INFO ("bar " << bar);
        REQUIRE (correlation (slice (out, bar * kBar, kBar), slice (in, (bar - 2) * kBar, kBar)) > 0.95);
    }
}

TEST_CASE ("Mode selector 1+2+3: three taps at their own levels and pans")
{
    auto p = wetOnly();
    p.modeSelector = ModeSelector::H1H2H3;
    p.power = 9.0f;
    p.headLevelDb[0] = 0.0f;
    p.headLevelDb[1] = -6.0f;
    p.headLevelDb[2] = -12.0f;
    p.headPan[0] = 0.0f;
    p.headPan[1] = -1.0f; // head 2 hard left
    p.headPan[2] = 1.0f;  // head 3 hard right
    EchoEngine engine;
    prepare (engine, p, 10);
    const auto in = distinctBars();
    const auto out = run (engine, in);

    // Bar 5: left = head 1 (bar 4) + head 2 (bar 3); right = head 1 + head 3 (bar 2).
    const auto left = channel (slice (out, 4 * kBar, kBar), 0);
    const auto right = channel (slice (out, 4 * kBar, kBar), 1);
    REQUIRE (correlation (left, channel (slice (in, 2 * kBar, kBar), 0)) > 0.2);
    REQUIRE (std::abs (correlation (left, channel (slice (in, 1 * kBar, kBar), 0))) < 0.1);
    REQUIRE (correlation (right, channel (slice (in, 1 * kBar, kBar), 1)) > 0.1);
    REQUIRE (std::abs (correlation (right, channel (slice (in, 2 * kBar, kBar), 1))) < 0.1);
}

TEST_CASE ("Iterative heads cue memory with the previous echo")
{
    auto p = wetOnly();
    p.modeSelector = ModeSelector::Custom;
    p.headLevelDb[0] = kLevelOffDb; // hear only head 2
    p.headMode[1] = HeadMode::Iterative;
    p.headLevelDb[1] = 0.0f;
    p.headPan[1] = 0.0f;
    p.power = 9.0f;
    EchoEngine engine;
    prepare (engine, p, 10);
    const auto in = distinctBars();
    const auto out = run (engine, in);
    // Head 1's echo of bar k is bar k itself (self-match); the echo of that
    // echo is again the closest memory: the same bar.
    REQUIRE (correlation (slice (out, 3 * kBar, kBar), slice (in, 2 * kBar, kBar)) > 0.9);
}

// ---- playback ------------------------------------------------------------------------------

TEST_CASE ("Voices with no spread, detune or delay reproduce the blend")
{
    auto blend = wetOnly();
    blend.power = 2.0f;
    auto voices = blend;
    voices.playback = Playback::Voices;
    voices.voices = 8;
    voices.voiceSpread = voices.voiceDetuneCents = voices.voiceDelayMs = 0.0f;

    EchoEngine a, b;
    prepare (a, blend, 6);
    prepare (b, voices, 6);
    const auto in = distinctBars();
    const auto outA = run (a, in);
    const auto outB = run (b, in);
    for (size_t i = static_cast<size_t> (kBar); i < outA.channels[0].size(); i += 97)
        REQUIRE (outB.channels[0][i] == Catch::Approx (outA.channels[0][i]).margin (1e-5));
}

TEST_CASE ("Voices spread across the stereo field and detune")
{
    auto p = wetOnly();
    p.power = 1.0f;
    p.playback = Playback::Voices;
    p.voices = 3;
    p.voiceSpread = 1.0f;
    p.voiceDetuneCents = 20.0f;
    p.voiceDelayMs = 10.0f;
    EchoEngine engine;
    prepare (engine, p, 6);
    const auto out = run (engine, distinctBars());
    const auto last = slice (out, 5 * kBar, kBar);
    REQUIRE (correlation (channel (last, 0), channel (last, 1)) < 0.9); // left and right differ
    REQUIRE (engine.getStats().activeTraces == 3);
    for (const auto& ch : out.channels)
        for (float x : ch)
            REQUIRE (std::isfinite (x));
}

TEST_CASE ("Sample playback plays exactly one memory at a time")
{
    auto p = wetOnly();
    p.power = 2.0f;
    p.playback = Playback::Sample;
    EchoEngine engine;
    prepare (engine, p, 10);
    const auto in = distinctBars();
    const auto out = run (engine, in);
    REQUIRE (engine.getStats().activeTraces == 1);
    // Each echo bar is one whole input bar (whichever was drawn).
    for (int bar = 2; bar < 6; ++bar)
    {
        double best = 0.0;
        for (int src = 0; src < bar; ++src)
            best = std::max (best, correlation (slice (out, bar * kBar, kBar), slice (in, src * kBar, kBar)));
        INFO ("bar " << bar);
        REQUIRE (best > 0.99);
    }
}

// ---- cue sources -------------------------------------------------------------------------------

TEST_CASE ("Sidechain cue: another signal chooses what memory plays")
{
    const auto o = bars (1);
    const auto drums = testgen::drums (o);
    const auto chords = testgen::chordsBass (o);
    AudioBuffer main, side;
    append (main, drums);
    append (main, chords);
    append (main, silence (2, 2 * kBar));
    append (side, silence (2, 2 * kBar));
    append (side, drums); // bar 3: the sidechain plays the drum bar
    append (side, silence (2, kBar));

    auto p = wetOnly();
    p.cueSource = CueSource::Sidechain;
    p.power = 9.0f;
    EchoEngine engine;
    prepare (engine, p, 10);

    const int n = main.numSamples();
    HostClock clock;
    clock.hasTempo = true;
    clock.isPlaying = true;
    for (int pos = 0; pos < n; pos += 512)
    {
        float* io[2] = { main.channels[0].data() + pos, main.channels[1].data() + pos };
        const float* sc[2] = { side.channels[0].data() + pos, side.channels[1].data() + pos };
        clock.ppqPosition = pos / 48000.0 * 2.0;
        engine.process (io, 2, std::min (512, n - pos), clock, sc, 2);
    }
    // Bar 4 echoes memory's answer to the sidechain's bar 3: the drums.
    REQUIRE (correlation (slice (main, 3 * kBar, kBar), drums) > 0.95);
}

TEST_CASE ("Random cue: memory dreams on after the input stops")
{
    auto p = wetOnly();
    p.cueSource = CueSource::Random;
    p.power = 3.0f;
    EchoEngine engine;
    prepare (engine, p, 10);
    AudioBuffer in;
    append (in, testgen::fullMix (bars (3)));
    append (in, silence (2, 3 * kBar));
    const auto out = run (engine, in);
    REQUIRE (rms (slice (out, 4 * kBar, 2 * kBar)) > 0.01);
}

TEST_CASE ("Frozen cue: the same memory answers every bar")
{
    auto p = wetOnly();
    p.power = 9.0f;
    EchoEngine engine;
    prepare (engine, p, 10);
    const auto in = distinctBars();
    run (engine, slice (in, 0, 2 * kBar)); // cue bar 2 (chords)...
    run (engine, silence (2, 64));        // ...its boundary falls on the block edge: let it happen
    p.cueSource = CueSource::Frozen;
    engine.setParams (p);
    const auto out = run (engine, slice (in, 2 * kBar, 4 * kBar));
    const auto chords = slice (in, kBar, kBar);
    for (int bar = 1; bar < 4; ++bar)
        REQUIRE (correlation (slice (out, bar * kBar, kBar), chords) > 0.95);
}

// ---- retrieval shaping --------------------------------------------------------------------------

TEST_CASE ("Recency favours the newest of equally similar traces")
{
    MemoryConfig cfg { 4, 1.0e8, 0.01 };
    TraceStore store (cfg, 48000.0, 1);
    FeatureVector f;
    f.fill (1.0f);
    for (int i = 0; i < 3; ++i)
    {
        store.spareSlot().features = f;
        store.spareSlot().end = 10;
        store.commitSpare();
    }
    std::vector<EchoWeight> w (4);
    RetrievalSettings rs;
    rs.recency = 1.0f;
    const auto r = retrieve (f, store, rs, w.data());
    REQUIRE (r.numWeights == 3);
    REQUIRE (w[2].weight > w[1].weight); // newest > middle > oldest
    REQUIRE (w[1].weight > w[0].weight);
    REQUIRE (w[2].weight / w[1].weight == Catch::Approx (std::exp (0.25)).epsilon (1e-4));
}

TEST_CASE ("Feature focus: rhythm-only and timbre-only matching")
{
    // Probe: loud in the first half of the bar, bright spectrum.
    auto make = [] (bool earlyLoud, bool bright) {
        FeatureVector f {};
        for (int s = 0; s < kSlots; ++s)
            for (int b = 0; b < kBands; ++b)
            {
                const float time = ((s < kSlots / 2) == earlyLoud) ? 1.0f : -1.0f;
                const float tone = ((b >= kBands / 2) == bright) ? 1.0f : -1.0f;
                f[static_cast<size_t> (s * kBands + b)] = 0.5f * time + 0.5f * tone;
            }
        return f;
    };
    const auto probe = make (true, true);
    const auto sameRhythm = make (true, false);
    const auto sameTimbre = make (false, true);

    auto sim = [&] (const FeatureVector& t, FeatureFocus focus) {
        return focusedSimilarity (probe, t, Similarity::Cosine, focus, 0, kSlots, false);
    };
    REQUIRE (sim (sameRhythm, FeatureFocus::Rhythm) > 0.99f);
    REQUIRE (sim (sameTimbre, FeatureFocus::Rhythm) < -0.99f);
    REQUIRE (sim (sameTimbre, FeatureFocus::Timbre) > 0.99f);
    REQUIRE (sim (sameRhythm, FeatureFocus::Timbre) < -0.99f);
}

TEST_CASE ("Echo address of a single trace is that trace's (normalised) address")
{
    MemoryConfig cfg { 2, 1.0e8, 0.01 };
    TraceStore store (cfg, 48000.0, 1);
    FeatureVector f {};
    for (size_t j = 0; j < f.size(); ++j)
        f[j] = std::sin (0.1f * static_cast<float> (j));
    store.spareSlot().features = f;
    const int slot = store.commitSpare();
    EchoWeight w { slot, 1.0f, 1.0f, 0 };
    FeatureVector out;
    echoAddress (&w, 1, store, FeatureMode::Continuous, 0.5f, out);
    REQUIRE (similarity (out, f, Similarity::Cosine) > 0.999f);
}

// ---- tone and modulation ------------------------------------------------------------------------

TEST_CASE ("Echo tone darkens the echo; familiarity can steer it")
{
    auto hf = [] (const AudioBuffer& b) {
        double s = 0;
        for (size_t i = 1; i < b.channels[0].size(); ++i)
        {
            const double d = b.channels[0][i] - b.channels[0][i - 1];
            s += d * d;
        }
        return s;
    };
    const auto in = testgen::fullMix (bars (3));
    auto p = wetOnly();
    EchoEngine open, dark;
    prepare (open, p, 1);
    p.echoToneHz = 800.0f;
    prepare (dark, p, 1);
    REQUIRE (hf (slice (run (dark, in), kBar, kBar)) < 0.2 * hf (slice (run (open, in), kBar, kBar)));
}

TEST_CASE ("Familiarity can drive feedback")
{
    // With self-match on, every echo is fully familiar: +1 routing adds feedback.
    auto p = wetOnly();
    p.feedback = 0.0f;
    p.intensityToFeedback = 1.0f;
    p.syncMode = SyncMode::Free;
    p.traceMs = 250.0f;
    EchoEngine engine;
    prepare (engine, p, 1);
    AudioBuffer in;
    append (in, slice (testgen::fullMix (bars (1)), 0, 12000));
    append (in, silence (2, 12000 * 4));
    const auto out = run (engine, in);
    // Without feedback the second repeat would be silent.
    REQUIRE (rms (slice (out, 24000, 12000)) > 0.01);
}
