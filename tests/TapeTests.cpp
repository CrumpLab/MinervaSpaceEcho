// Stage 5: tape character (length mismatch, wow/flutter, drive, hiss,
// feedback EQ, spring reverb).
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace mse;
using namespace mse::test;

namespace {

constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979323846;

AudioBuffer sine (double hz, int samples, float amp = 0.5f)
{
    auto b = silence (2, samples);
    for (int i = 0; i < samples; ++i)
        b.channels[0][static_cast<size_t> (i)] = b.channels[1][static_cast<size_t> (i)]
            = amp * static_cast<float> (std::sin (2.0 * kPi * hz * i / kSr));
    return b;
}

// Frequency from positive-going zero crossings (channel 0).
double frequency (const AudioBuffer& b)
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
    return count > 1 ? (count - 1) * kSr / (last - first) : 0.0;
}

// Goertzel power at one frequency.
double power (const AudioBuffer& b, double hz)
{
    const double w = 2.0 * kPi * hz / kSr, coeff = 2.0 * std::cos (w);
    double s1 = 0, s2 = 0;
    for (float x : b.channels[0])
    {
        const double s = x + coeff * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

EngineParams freeRunning (float ms)
{
    auto p = wetOnly();
    p.syncMode = SyncMode::Free;
    p.traceMs = ms;
    return p;
}

// Records a 2 s 440 Hz tone as one trace, then replays it from a frozen cue
// with a different trace length. Returns 5 s of echo.
AudioBuffer replayAtNewLength (LengthMismatch mode, float newMs, float recordMs = 2000.0f)
{
    auto p = freeRunning (recordMs);
    p.lengthMismatch = mode;
    EchoEngine engine;
    prepare (engine, p, 10);
    run (engine, sine (440.0, static_cast<int> (recordMs * 48.0)));
    run (engine, silence (2, 64)); // commit the boundary

    p.traceMs = newMs;
    p.freeze = true;
    p.cueSource = CueSource::Frozen;
    engine.setParams (p);
    return run (engine, silence (2, 5 * 48000));
}

} // namespace

TEST_CASE ("Length mismatch: varispeed changes pitch like tape; cut keeps it")
{
    const auto fast = replayAtNewLength (LengthMismatch::Varispeed, 1000.0f);
    const auto cut = replayAtNewLength (LengthMismatch::Cut, 1000.0f);
    // Measure inside the second echo segment (1.0 - 2.0 s after the change).
    const double fFast = frequency (slice (fast, 60000, 24000));
    const double fCut = frequency (slice (cut, 60000, 24000));
    INFO ("varispeed " << fFast << " Hz, cut " << fCut << " Hz");
    REQUIRE (fFast == Catch::Approx (880.0).epsilon (0.01)); // twice the speed: an octave up
    REQUIRE (fCut == Catch::Approx (440.0).epsilon (0.01));
}

TEST_CASE ("Length mismatch: loop repeats a short trace to fill the segment; cut leaves a gap")
{
    const auto loop = replayAtNewLength (LengthMismatch::Loop, 2000.0f, 1000.0f);
    const auto cut = replayAtNewLength (LengthMismatch::Cut, 2000.0f, 1000.0f);
    // The segment in progress when the length changed finishes on its old
    // schedule; the next one (from ~2 s) is 2 s long. Measure in its second
    // half, past the end of the 1 s trace.
    const int from = 96000 + 60000;
    REQUIRE (rms (slice (loop, from, 24000)) > 0.2);
    REQUIRE (rms (slice (cut, from, 24000)) < 1.0e-4);
    REQUIRE (frequency (slice (loop, from, 24000)) == Catch::Approx (440.0).epsilon (0.01)); // pitch unchanged
}

TEST_CASE ("Wow and flutter wobble the pitch; zero leaves it steady")
{
    auto wobble = [] (float wow, float flutter) {
        auto p = freeRunning (500.0f);
        p.wow = wow;
        p.flutter = flutter;
        EchoEngine engine;
        prepare (engine, p, 1);
        const auto out = run (engine, sine (440.0, 5 * 48000));
        double lo = 1e9, hi = 0;
        for (int w = 0; w < 120; ++w) // 25 ms windows: short enough to see 7 Hz flutter
        {
            const double f = frequency (slice (out, 48000 + w * 1200, 1200));
            lo = std::min (lo, f);
            hi = std::max (hi, f);
        }
        return hi - lo;
    };
    const double steady = wobble (0.0f, 0.0f);
    const double wow = wobble (1.0f, 0.0f);
    const double flutter = wobble (0.0f, 1.0f);
    INFO ("spread: steady " << steady << ", wow " << wow << ", flutter " << flutter << " Hz");
    REQUIRE (steady < 1.5);
    REQUIRE (wow > 3.0);
    REQUIRE (flutter > 3.0);
}

TEST_CASE ("Tape drive compresses loud echoes but leaves quiet ones alone")
{
    auto peakOfEcho = [] (float amp, float drive) {
        auto p = freeRunning (250.0f);
        p.tapeDrive = drive;
        EchoEngine engine;
        prepare (engine, p, 1);
        const auto out = run (engine, sine (220.0, 36000, amp));
        float peak = 0.0f;
        for (size_t i = 12000; i < out.channels[0].size(); ++i)
            peak = std::max (peak, std::abs (out.channels[0][i]));
        return peak;
    };
    REQUIRE (peakOfEcho (0.9f, 1.0f) < 0.3f);
    REQUIRE (peakOfEcho (0.9f, 0.0f) == Catch::Approx (0.9f).epsilon (0.01));
    REQUIRE (peakOfEcho (0.005f, 1.0f) == Catch::Approx (0.005f).epsilon (0.02));
}

TEST_CASE ("Hiss sits on the echo at the set level")
{
    auto p = wetOnly();
    p.hissDb = -40.0f;
    EchoEngine engine;
    prepare (engine, p, 10);
    const auto out = run (engine, silence (2, 48000));
    const double r = rms (out);
    INFO ("hiss rms " << r);
    REQUIRE (r > 0.005);
    REQUIRE (r < 0.015);

    p.hissDb = kGateOffDb;
    EchoEngine quiet;
    prepare (quiet, p, 10);
    REQUIRE (rms (run (quiet, silence (2, 48000))) == 0.0);
}

TEST_CASE ("Feedback bass cut thins out each repeat")
{
    auto p = freeRunning (250.0f);
    p.feedback = 0.9f;
    p.feedbackBassDb = -12.0f;
    EchoEngine engine;
    prepare (engine, p, 1);

    // A 250 ms burst of 100 Hz + 5 kHz, then silence while it repeats.
    auto in = sine (100.0, 12000, 0.3f);
    const auto hi = sine (5000.0, 12000, 0.3f);
    for (size_t c = 0; c < 2; ++c)
        for (size_t i = 0; i < in.channels[c].size(); ++i)
            in.channels[c][i] += hi.channels[c][i];
    append (in, silence (2, 12000 * 6));
    const auto out = run (engine, in);

    auto balance = [&] (int repeat) {
        const auto seg = slice (out, repeat * 12000 + 1000, 10000);
        return power (seg, 100.0) / power (seg, 5000.0);
    };
    INFO ("low/high: repeat 1 " << balance (1) << ", repeat 4 " << balance (4));
    REQUIRE (balance (4) < 0.1 * balance (1));
}

TEST_CASE ("Spring reverb adds a decaying tail; off means no tail")
{
    auto tail = [] (float levelDb) {
        auto p = wetOnly();
        p.echoLevelDb = kLevelOffDb; // hear only the spring
        p.dryLevelDb = 0.0f;
        p.springLevelDb = levelDb;
        p.springOnDry = true;
        p.springDecaySeconds = 1.0f;
        EchoEngine engine;
        prepare (engine, p, 10);
        AudioBuffer in = sine (1000.0, 2400, 0.5f); // 50 ms burst
        append (in, silence (2, 2 * 48000));
        return run (engine, in);
    };
    const auto wet = tail (0.0f);
    const double early = rms (slice (wet, 9600, 9600));   // 0.2-0.4 s
    const double late = rms (slice (wet, 72000, 9600));   // 1.5-1.7 s
    INFO ("early " << early << ", late " << late);
    REQUIRE (early > 1.0e-3);
    REQUIRE (late < 0.1 * early);
    for (float x : wet.channels[0])
        REQUIRE (std::isfinite (x));

    REQUIRE (rms (slice (tail (kLevelOffDb), 9600, 9600)) == 0.0);
}

TEST_CASE ("Snapshots keep each trace's recorded length")
{
    auto p = freeRunning (1500.0f);
    EchoEngine engine;
    prepare (engine, p, 10);
    run (engine, sine (440.0, 3 * 48000));
    run (engine, silence (2, 64));
    MemorySnapshot snap;
    REQUIRE (engine.takeSnapshot (snap));
    REQUIRE (! snap.traces.empty());
    REQUIRE (snap.traces[0].nominalLength == Catch::Approx (72000.0));
    const auto blob = serializeMemory (snap);
    REQUIRE (deserializeMemory (blob.data(), blob.size()).traces[0].nominalLength == Catch::Approx (72000.0));
}
