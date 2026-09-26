// Stage 3: live cueing (Progressive and Rolling modes, prediction, frame tracks).
#include "TestHelpers.h"

#include "mse/Retrieval.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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

// RMS envelope in 10 ms windows (channel 0): compares timing without
// requiring sample-accurate alignment.
std::vector<double> envelope (const AudioBuffer& b)
{
    std::vector<double> e;
    const int w = 480;
    for (int i = 0; i + w <= b.numSamples(); i += w)
    {
        double s = 0;
        for (int j = 0; j < w; ++j)
            s += static_cast<double> (b.channels[0][static_cast<size_t> (i + j)]) * b.channels[0][static_cast<size_t> (i + j)];
        e.push_back (std::sqrt (s / w));
    }
    return e;
}

double envelopeCorrelation (const AudioBuffer& a, const AudioBuffer& b)
{
    const auto x = envelope (a), y = envelope (b);
    const size_t n = std::min (x.size(), y.size());
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    for (size_t i = 0; i < n; ++i)
    {
        sx += x[i]; sy += y[i];
        sxx += x[i] * x[i]; syy += y[i] * y[i]; sxy += x[i] * y[i];
    }
    const double cov = sxy - sx * sy / n, vx = sxx - sx * sx / n, vy = syy - sy * sy / n;
    return (vx <= 0 || vy <= 0) ? 0.0 : cov / std::sqrt (vx * vy);
}

} // namespace

// ---- Progressive -------------------------------------------------------------------------

TEST_CASE ("Progressive cue: a returning bar is joined by its memory, in step")
{
    const auto a = testgen::drums (bars (1));
    AudioBuffer in;
    append (in, a);
    append (in, testgen::chordsBass (bars (1)));
    append (in, a);
    append (in, silence (2, kBar));

    auto p = wetOnly();
    p.cueMode = CueMode::Progressive;
    p.power = 9.0f;
    p.cueSmoothingMs = 5.0f;
    EchoEngine engine;
    prepare (engine, p, 100);
    const auto out = run (engine, in);

    // Second half of bar 3: memory of bar 1 plays at the same position.
    const double c = correlation (slice (out, 2 * kBar + kBar / 2, kBar / 2), slice (a, kBar / 2, kBar / 2));
    INFO ("correlation " << c);
    REQUIRE (c > 0.95);
    REQUIRE (engine.getStats().cueUpdates > 30); // re-cued many times per bar
}

TEST_CASE ("Progressive cue with prediction plays memory ahead of the input")
{
    const auto a = testgen::drums (bars (1));
    AudioBuffer in;
    append (in, a);
    append (in, testgen::chordsBass (bars (1)));
    append (in, a);
    append (in, silence (2, kBar));

    auto p = wetOnly();
    p.cueMode = CueMode::Progressive;
    p.power = 9.0f;
    p.cueSmoothingMs = 5.0f;
    p.lookaheadMs = 125.0f; // 6000 samples
    EchoEngine engine;
    prepare (engine, p, 100);
    const auto out = run (engine, in);

    const int start = 2 * kBar + kBar / 2;
    const double ahead = correlation (slice (out, start, kBar / 4), slice (a, kBar / 2 + 6000, kBar / 4));
    const double inStep = correlation (slice (out, start, kBar / 4), slice (a, kBar / 2, kBar / 4));
    INFO ("ahead " << ahead << ", in step " << inStep);
    REQUIRE (ahead > 0.95);
    REQUIRE (ahead > inStep);
}

TEST_CASE ("Progressive cue: early slots still play the previous bar's echo")
{
    auto p = wetOnly();
    p.cueMode = CueMode::Progressive;
    p.progressiveStart = 8; // first half of each bar: Segment-mode echo
    EchoEngine engine;
    prepare (engine, p, 1);
    const auto in = testgen::fullMix (bars (3));
    const auto out = run (engine, in);
    // First half of bar 2 is exactly the delayed bar 1.
    REQUIRE (correlation (slice (out, kBar, kBar / 2 - 1000), slice (in, 0, kBar / 2 - 1000)) > 0.999);
}

TEST_CASE ("Prefix similarity compares only the part heard so far")
{
    const auto bar = testgen::fullMix (bars (1));
    FeatureExtractor full, half;
    full.prepare (48000.0);
    half.prepare (48000.0);
    full.beginSegment (kBar);
    half.beginSegment (kBar);
    full.push (bar.channels[0].data(), kBar, 0);
    half.push (bar.channels[0].data(), kBar / 2, 0);
    FeatureVector f, h;
    full.finalize (f, {});
    half.finalize (h, {});

    REQUIRE (similarity (h, f, Similarity::Cosine) < 0.9f); // whole-bar comparison is diluted
    REQUIRE (prefixSimilarity (h, f, Similarity::Cosine, 0, kSlots / 2, true) > 0.99f);
    REQUIRE (prefixSimilarity (h, f, Similarity::Hintzman, 0, kSlots / 2, true) > 0.9f);
}

// ---- Rolling ------------------------------------------------------------------------------

TEST_CASE ("Frame tracks are recorded with every trace")
{
    auto p = wetOnly();
    EchoEngine engine;
    prepare (engine, p, 10);
    run (engine, testgen::fullMix (bars (2)));
    run (engine, silence (2, 64));
    MemorySnapshot snap;
    REQUIRE (engine.takeSnapshot (snap));
    REQUIRE (snap.traces.size() == 2);

    // Reloaded traces get their frame tracks recomputed from the audio, so a
    // rolling search works on loaded memory too.
    EchoEngine loaded;
    loaded.loadSnapshot (snap);
    auto r = wetOnly();
    r.cueMode = CueMode::Rolling;
    r.writeGateDb = 0.0f; // don't add the cue itself to memory
    prepare (loaded, r, 10);
    const auto out = run (loaded, slice (testgen::fullMix (bars (2)), kBar / 3, kBar));
    REQUIRE (rms (slice (out, kBar / 2, kBar / 2)) > 0.01);
}

TEST_CASE ("Best offset finds a pattern hidden in a frame track")
{
    MemoryConfig cfg { 2, 1.0e8, 2.0 };
    TraceStore store (cfg, 48000.0, 1);
    auto& t = store.spareSlot();
    uint64_t r = 99;
    auto noise = [&r] {
        r = r * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<float> ((r >> 40) % 1000) * 0.02f - 60.0f;
    };
    for (int f = 0; f < 80; ++f)
        for (int b = 0; b < kBands; ++b)
            t.frames[f * kBands + b] = noise();
    t.frameBegin = 0;
    t.frameEnd = 80;

    const float* frames[10];
    for (int i = 0; i < 10; ++i)
        frames[i] = t.frames + static_cast<size_t> (37 + i) * kBands;
    WindowProbe probe;
    REQUIRE (prepareWindowProbe (frames, 10, probe));
    const auto m = bestOffset (probe, t, 1);
    REQUIRE (m.offset == 37);
    REQUIRE (m.similarity == Catch::Approx (1.0).margin (1e-4));

    // Not enough continuation left after a match at the very end.
    for (int i = 0; i < 10; ++i)
        frames[i] = t.frames + static_cast<size_t> (70 + i) * kBands;
    REQUIRE (prepareWindowProbe (frames, 10, probe));
    REQUIRE (bestOffset (probe, t, 1).offset != 70);
}

TEST_CASE ("Rolling cue: unclocked, memory continues what you play, in time")
{
    // Store 8 s of music as one free-running trace, then replay a stretch of it
    // starting at an arbitrary, un-aligned point.
    const auto music = testgen::fullMix (bars (4));
    const int start = 3 * 48000 + 17777;
    const int length = 3 * 48000;
    AudioBuffer in;
    append (in, music);
    append (in, slice (music, start, length));

    auto p = wetOnly();
    p.syncMode = SyncMode::Free;
    p.traceMs = 8000.0f;
    p.cueMode = CueMode::Rolling;
    p.power = 9.0f;
    p.cueSmoothingMs = 10.0f;
    EchoEngine engine;
    prepare (engine, p, 10);
    const auto out = run (engine, in);

    // After the window + one interval, the echo follows the replay closely.
    const int from = music.numSamples() + 24000;
    const auto echo = slice (out, from, length - 24000);
    const auto played = slice (in, from, length - 24000);
    const double c = envelopeCorrelation (echo, played);
    INFO ("envelope correlation " << c << ", cue latency " << engine.getStats().cueLatencyMs << " ms");
    REQUIRE (c > 0.8);
    REQUIRE (engine.getStats().cueLatencyMs < 30.0f); // small memory: search finishes within a block or two
}

TEST_CASE ("Rolling cue: silence evokes nothing")
{
    auto p = wetOnly();
    p.cueMode = CueMode::Rolling;
    EchoEngine engine;
    prepare (engine, p, 10);
    AudioBuffer in;
    append (in, testgen::fullMix (bars (2)));
    append (in, silence (2, 2 * kBar));
    const auto out = run (engine, in);
    REQUIRE (rms (slice (out, 3 * kBar, kBar)) < 1.0e-4);
}

TEST_CASE ("Rolling search stays within its per-block budget on a large memory")
{
    // 100 traces of 1 s: a full search takes several blocks (and spans trace
    // boundaries, where memory changes), but each block's work is bounded and
    // the echo still arrives.
    auto p = wetOnly();
    p.syncMode = SyncMode::Free;
    p.traceMs = 1000.0f;
    p.cueMode = CueMode::Rolling;
    EchoEngine engine;
    prepare (engine, p, 100);
    AudioBuffer in;
    for (int i = 0; i < 14; ++i)
        append (in, testgen::fullMix (bars (4)));
    const auto out = run (engine, in, { 256 });
    REQUIRE (engine.getStats().tracesStored == 100);
    REQUIRE (rms (slice (out, in.numSamples() - kBar, kBar)) > 0.01);
    REQUIRE (engine.getStats().cueLatencyMs < 300.0f);
}
