// Stage 8: edge cases hosts produce (plan: sample-rate changes, tempo changes
// mid-segment, offline bounces with odd block sizes, transport jumps, bad input).
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

using namespace mse;
using namespace mse::test;

namespace {

constexpr double kPi = 3.14159265358979323846;

AudioBuffer tone (double hz, int samples, double sr = 48000.0, float amp = 0.3f)
{
    auto b = silence (2, samples, sr);
    for (int i = 0; i < samples; ++i)
        b.channels[0][static_cast<size_t> (i)] = b.channels[1][static_cast<size_t> (i)]
            = amp * static_cast<float> (std::sin (2.0 * kPi * hz * i / sr));
    return b;
}

// Runs with a given sequence of block sizes (cycled) and a steady clock.
AudioBuffer runBlocks (EchoEngine& engine, AudioBuffer audio, const std::vector<int>& sizes, double bpm = 120.0)
{
    const int n = audio.numSamples();
    HostClock clock;
    clock.hasTempo = true;
    clock.bpm = bpm;
    clock.isPlaying = true;
    size_t k = 0;
    for (int pos = 0; pos < n;)
    {
        const int len = std::min (sizes[k++ % sizes.size()], n - pos);
        float* ptrs[2] = { audio.channels[0].data() + pos, audio.channels[1].data() + pos };
        clock.ppqPosition = pos / audio.sampleRate * bpm / 60.0;
        engine.process (ptrs, 2, len, clock);
        pos += len;
    }
    return audio;
}

double maxStep (const AudioBuffer& b, int from = 0)
{
    double m = 0.0;
    for (size_t i = static_cast<size_t> (std::max (1, from)); i < b.channels[0].size(); ++i)
        m = std::max (m, static_cast<double> (std::abs (b.channels[0][i] - b.channels[0][i - 1])));
    return m;
}

bool allFinite (const AudioBuffer& b)
{
    for (const auto& ch : b.channels)
        for (float x : ch)
            if (! std::isfinite (x))
                return false;
    return true;
}

double frequency (const AudioBuffer& b, double sr)
{
    const auto& x = b.channels[0];
    int first = -1, last = -1, count = 0;
    for (size_t i = 1; i < x.size(); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            if (first < 0)
                first = static_cast<int> (i);
            last = static_cast<int> (i);
            ++count;
        }
    return count > 1 ? (count - 1) * sr / (last - first) : 0.0;
}

} // namespace

TEST_CASE ("Blocks larger than prepared, and odd block sizes, give the same output")
{
    const auto in = testgen::fullMix (testgen::Options { 48000.0, 120.0, 1, 6 });
    auto p = wetOnly();
    p.dryLevelDb = -6.0f;
    p.feedback = 0.3f;

    EchoEngine ref;
    prepare (ref, p, 100, 48000.0, 512);
    const auto expected = runBlocks (ref, in, { 512 });

    EchoEngine big; // prepared for 512, fed 4096 (offline bounce)
    prepare (big, p, 100, 48000.0, 512);
    const auto a = runBlocks (big, in, { 4096, 8192, 777 });

    EchoEngine odd;
    prepare (odd, p, 100, 48000.0, 512);
    const auto b = runBlocks (odd, in, { 1, 0, 13, 512, 64, 300, 0, 511 });

    REQUIRE (big.getStats().tracesStored == ref.getStats().tracesStored);
    double da = 0.0, db = 0.0;
    for (size_t i = 0; i < expected.channels[0].size(); ++i)
    {
        da = std::max (da, static_cast<double> (std::abs (a.channels[0][i] - expected.channels[0][i])));
        db = std::max (db, static_cast<double> (std::abs (b.channels[0][i] - expected.channels[0][i])));
    }
    INFO ("max difference: big blocks " << da << ", odd blocks " << db);
    REQUIRE (da < 1.0e-5);
    REQUIRE (db < 1.0e-5);
}

TEST_CASE ("Changing the sample rate keeps memory, resampled")
{
    auto p = wetOnly();
    p.syncMode = SyncMode::Free;
    p.traceMs = 500.0f;
    EchoEngine e;
    prepare (e, p, 10, 48000.0);
    run (e, tone (440.0, 24000 * 3));
    run (e, silence (2, 512));
    REQUIRE (e.getStats().tracesStored == 3);

    e.prepare (44100.0, 512, 2);
    REQUIRE (e.getStats().tracesStored == 3);
    p.freeze = true;
    p.cueSource = CueSource::Frozen;
    e.setParams (p);
    // A frozen cue needs one live segment first; then memory answers at 44.1 kHz.
    run (e, tone (440.0, 22050, 44100.0));
    const auto out = run (e, silence (2, 44100, 44100.0));
    const auto seg = slice (out, 2000, 18000);
    INFO ("rms " << rms (seg) << ", frequency " << frequency (seg, 44100.0));
    REQUIRE (rms (seg) > 0.1);
    REQUIRE (frequency (seg, 44100.0) == Catch::Approx (440.0).epsilon (0.01));
}

TEST_CASE ("Changing the channel count keeps memory")
{
    auto p = wetOnly();
    p.syncMode = SyncMode::Free;
    p.traceMs = 500.0f;
    EchoEngine e;
    prepare (e, p, 10, 48000.0, 512, 2);
    run (e, tone (440.0, 24000 * 2));
    run (e, silence (2, 512));
    REQUIRE (e.getStats().tracesStored == 2);
    e.prepare (48000.0, 512, 1);
    REQUIRE (e.getStats().tracesStored == 2);
    e.prepare (48000.0, 512, 2);
    REQUIRE (e.getStats().tracesStored == 2);
}

TEST_CASE ("A tempo change mid-segment neither clicks nor breaks the grid")
{
    auto p = wetOnly();
    p.edgeFadeMs = 5.0f;
    EchoEngine e;
    prepare (e, p, 1); // a 1-bar tape delay
    // 2.5 bars at 120 BPM, then 3 bars at 90 BPM, with the clock continuous.
    const auto in = tone (220.0, static_cast<int> (48000 * (5.0 + 8.0)));
    auto audio = in;
    HostClock clock;
    clock.hasTempo = true;
    clock.isPlaying = true;
    double ppq = 0.0;
    for (int pos = 0; pos < audio.numSamples(); pos += 512)
    {
        clock.bpm = ppq < 10.0 ? 120.0 : 90.0;
        clock.ppqPosition = ppq;
        float* ptrs[2] = { audio.channels[0].data() + pos, audio.channels[1].data() + pos };
        const int len = std::min (512, audio.numSamples() - pos);
        e.process (ptrs, 2, len, clock);
        ppq += len / 48000.0 * clock.bpm / 60.0;
    }
    REQUIRE (allFinite (audio));
    const double step = maxStep (audio, 48000 * 2);
    INFO ("largest sample step " << step);
    REQUIRE (step < 0.06); // a 0.3 sine at 220 Hz moves up to 0.009 per sample; clicks jump far more
    REQUIRE (e.getStats().traceSeconds == Catch::Approx (60.0 / 90.0 * 4.0).epsilon (0.001));
    REQUIRE (e.getStats().tracesStored == 1);
}

TEST_CASE ("Transport jumps (looping in Live) resynchronise cleanly")
{
    auto p = wetOnly();
    EchoEngine e;
    prepare (e, p, 100);
    auto audio = testgen::fullMix (testgen::Options { 48000.0, 120.0, 1, 12 });
    HostClock clock;
    clock.hasTempo = true;
    clock.bpm = 120.0;
    clock.isPlaying = true;
    double ppq = 0.0;
    for (int pos = 0; pos < audio.numSamples(); pos += 512)
    {
        clock.ppqPosition = ppq;
        float* ptrs[2] = { audio.channels[0].data() + pos, audio.channels[1].data() + pos };
        e.process (ptrs, 2, std::min (512, audio.numSamples() - pos), clock);
        ppq += 512 / 48000.0 * 2.0;
        if (ppq >= 16.0 + 1.3) // loop back from mid-bar 5 to bar 1
            ppq -= 16.0;
    }
    REQUIRE (allFinite (audio));
    REQUIRE (e.getStats().tracesStored >= 8);
}

TEST_CASE ("Non-finite input never reaches memory or the output")
{
    auto p = wetOnly();
    p.dryLevelDb = 0.0f;
    p.feedback = 0.5f;
    p.syncMode = SyncMode::Free;
    p.traceMs = 500.0f;
    EchoEngine e;
    prepare (e, p, 10);
    auto bad = tone (440.0, 24000);
    bad.channels[0][100] = std::numeric_limits<float>::quiet_NaN();
    bad.channels[1][2000] = std::numeric_limits<float>::infinity();
    bad.channels[0][3000] = -std::numeric_limits<float>::infinity();
    auto out = run (e, bad);
    for (int k = 0; k < 4; ++k)
    {
        const auto more = run (e, tone (440.0, 24000));
        REQUIRE (allFinite (more));
    }
    MemorySnapshot snap;
    REQUIRE (e.takeSnapshot (snap));
    for (const auto& t : snap.traces)
    {
        for (const auto& ch : t.audio)
            for (float x : ch)
                REQUIRE (std::isfinite (x));
        for (float f : t.features)
            REQUIRE (std::isfinite (f));
    }
    // Memory still works: the echo is there.
    REQUIRE (rms (run (e, silence (2, 24000))) > 0.05);
}

TEST_CASE ("Offline renders are deterministic")
{
    const auto in = testgen::styleChange (testgen::Options { 48000.0, 120.0, 3, 8 });
    auto p = wetOnly();
    p.fullPolicy = FullPolicy::Random;
    p.playback = Playback::Sample;
    p.contentDropout = 0.2f;
    p.hissDb = -60.0f;
    p.wow = 0.5f;
    auto render = [&] {
        EchoEngine e;
        e.setSeed (7);
        prepare (e, p, 4);
        return run (e, in);
    };
    const auto a = render();
    const auto b = render();
    REQUIRE (a.channels == b.channels);
}

namespace {

// Largest sample-to-sample step after the first 2 s of a steady 110 Hz tone,
// with parameter `index` held at `a`, held at `b`, or jumping between them
// every 100 ms.
double largestStep (int config, int index, float a, float b, bool jump)
{
    auto v = defaultParamValues();
    v[kSyncMode] = 0;
    v[kTraceMs] = 500;
    v[kFeedback] = 0.4f;
    v[kEchoLevelDb] = -3;
    v[kDryLevelDb] = -6;
    v[kEchoToneHz] = 8000;
    v[kTapeDrive] = 0.3f;
    v[kSpringLevelDb] = -20;
    int capacity = 1;
    if (config == 1) // three heads, chorus voices, a blended memory
    {
        capacity = 10;
        v[kModeSelector] = 7;
        v[kPlayback] = 1;
        v[kPower] = 1;
    }
    auto va = v, vb = v;
    va[static_cast<size_t> (index)] = a;
    vb[static_cast<size_t> (index)] = b;
    EchoEngine e;
    prepare (e, paramsFromValues (va), capacity);
    auto in = tone (110.0, 48000 * 4);
    HostClock clock;
    clock.hasTempo = true;
    clock.bpm = 120;
    clock.isPlaying = true;
    double worst = 0.0;
    float last = 0.0f;
    for (int pos = 0; pos < in.numSamples(); pos += 480)
    {
        const bool useB = jump ? (pos / 4800) % 2 == 1 : a != b;
        e.setParams (paramsFromValues (useB ? vb : va));
        float* ptrs[2] = { in.channels[0].data() + pos, in.channels[1].data() + pos };
        e.process (ptrs, 2, 480, clock);
        for (int i = 0; i < 480; ++i)
        {
            const float x = in.channels[0][static_cast<size_t> (pos + i)];
            if (pos + i > 48000 * 2)
                worst = std::max (worst, static_cast<double> (std::abs (x - last)));
            last = x;
        }
    }
    return worst;
}

} // namespace

TEST_CASE ("Moving a continuous parameter never clicks")
{
    for (int config = 0; config < 2; ++config)
        for (int i = 0; i < kNumParams; ++i)
        {
            const auto& s = paramSpecs()[static_cast<size_t> (i)];
            // Write Probability and Encoding Failure change what memory holds, not the sound directly.
            if (s.type != ParamType::Float || i == kWriteProbability || i == kEncodingFailure)
                continue;
            const float lo = s.min + 0.25f * (s.max - s.min), hi = s.min + 0.75f * (s.max - s.min);
            const double steady = std::max (largestStep (config, i, lo, lo, false), largestStep (config, i, hi, hi, false));
            const double moving = largestStep (config, i, lo, hi, true);
            INFO ("config " << config << ", " << s.id << ": " << moving << " moving vs " << steady << " steady");
            CHECK (moving < 1.5 * steady);
        }
}
