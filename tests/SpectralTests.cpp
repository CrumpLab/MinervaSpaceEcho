// Stage 6: spectral engine (FFT, spectral blending, time-stretch, freeze,
// grain memory).
#include "TestHelpers.h"

#include "mse/Spectral.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace mse;
using namespace mse::test;

namespace {

constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979323846;

AudioBuffer sine (double hz, int samples, float amp = 0.5f, double phase = 0.0)
{
    auto b = silence (2, samples);
    for (int i = 0; i < samples; ++i)
        b.channels[0][static_cast<size_t> (i)] = b.channels[1][static_cast<size_t> (i)]
            = amp * static_cast<float> (std::sin (2.0 * kPi * hz * i / kSr + phase));
    return b;
}

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

EngineParams spectralFree (float ms)
{
    auto p = wetOnly();
    p.syncMode = SyncMode::Free;
    p.traceMs = ms;
    p.blendDomain = BlendDomain::Spectral;
    return p;
}

} // namespace

TEST_CASE ("FFT forward + inverse is the identity")
{
    Fft fft;
    fft.prepare (1024);
    std::vector<float> re (1024), im (1024, 0.0f), orig (1024);
    for (size_t i = 0; i < re.size(); ++i)
        orig[i] = re[i] = std::sin (0.37f * static_cast<float> (i)) + 0.25f * std::cos (1.9f * static_cast<float> (i));
    fft.forward (re.data(), im.data());
    fft.inverse (re.data(), im.data());
    for (size_t i = 0; i < re.size(); ++i)
        REQUIRE (re[i] / 1024.0f == Catch::Approx (orig[i]).margin (1e-5));
}

TEST_CASE ("Spectral blending of a single memory reconstructs it (capacity 1 is still a delay)")
{
    auto p = spectralFree (500.0f);
    EchoEngine engine;
    prepare (engine, p, 1);
    const auto in = testgen::fullMix (testgen::Options { 48000.0, 120.0, 1, 2 });
    const auto out = run (engine, in);
    const int L = 24000, N = 2048;
    for (int seg = 1; seg < 7; ++seg)
        for (int i = seg * L + N; i < (seg + 1) * L - N; i += 7)
            REQUIRE (out.channels[0][static_cast<size_t> (i)]
                     == Catch::Approx (in.channels[0][static_cast<size_t> (i - L)]).margin (2e-4));
}

TEST_CASE ("Spectral blending avoids phase cancellation between unaligned memories")
{
    // Two memories of the same tone, half a period apart: in the waveform
    // domain they cancel; as spectra their magnitudes add.
    auto echoLevel = [] (BlendDomain domain) {
        auto p = spectralFree (500.0f);
        p.blendDomain = domain;
        p.selfMatch = false;
        p.power = 1.0f;
        EchoEngine engine;
        prepare (engine, p, 10);
        AudioBuffer in;
        append (in, sine (440.0, 24000));
        append (in, sine (440.0, 24000, 0.5f, kPi)); // inverted
        append (in, sine (440.0, 24000));             // cue: recalls both
        append (in, silence (2, 24000));
        const auto out = run (engine, in);
        return rms (slice (out, 3 * 24000 + 4000, 16000));
    };
    const double wave = echoLevel (BlendDomain::Waveform);
    const double spec = echoLevel (BlendDomain::Spectral);
    INFO ("waveform " << wave << ", spectral " << spec);
    REQUIRE (spec > 0.2);
    REQUIRE (spec > 10.0 * wave);
}

TEST_CASE ("Stretch fits old traces to a new length without changing pitch")
{
    auto p = wetOnly();
    p.syncMode = SyncMode::Free;
    p.traceMs = 2000.0f;
    p.lengthMismatch = LengthMismatch::Stretch;
    EchoEngine engine;
    prepare (engine, p, 10);
    run (engine, sine (440.0, 96000));
    run (engine, silence (2, 64));

    p.traceMs = 1000.0f;
    p.freeze = true;
    p.cueSource = CueSource::Frozen;
    engine.setParams (p);
    const auto out = run (engine, silence (2, 3 * 48000));
    const auto seg = slice (out, 60000, 24000); // inside a 1 s segment playing the 2 s trace
    INFO ("rms " << rms (seg) << ", frequency " << frequency (seg));
    REQUIRE (rms (seg) > 0.2);
    REQUIRE (frequency (seg) == Catch::Approx (440.0).epsilon (0.01)); // varispeed would give 880
}

TEST_CASE ("Spectral freeze holds the echo after memory is cleared")
{
    auto p = spectralFree (500.0f);
    EchoEngine engine;
    prepare (engine, p, 10);
    run (engine, sine (440.0, 48000 * 2 + 12000)); // freeze mid-segment
    p.spectralFreeze = true;
    engine.setParams (p);
    run (engine, sine (440.0, 4800));
    engine.sendCommand (Command::ClearAll);
    const auto out = run (engine, silence (2, 48000 * 3));
    const auto late = slice (out, 48000 * 2, 24000);
    INFO ("rms " << rms (late) << ", frequency " << frequency (late));
    REQUIRE (rms (late) > 0.05);
    REQUIRE (frequency (late) == Catch::Approx (440.0).epsilon (0.03));

    // Unfreezing with an empty memory lets it go.
    p.spectralFreeze = false;
    engine.setParams (p);
    REQUIRE (rms (slice (run (engine, silence (2, 48000)), 24000, 24000)) < 1.0e-4);
}

TEST_CASE ("Grain memory: thousands of 15 ms traces")
{
    auto p = wetOnly();
    p.syncMode = SyncMode::Free;
    p.traceMs = 15.0f;
    p.selfMatch = false;
    p.power = 6.0f;
    EchoEngine engine;
    prepare (engine, p, 2000);
    AudioBuffer in;
    for (int i = 0; i < 4; ++i)
        append (in, testgen::fullMix (testgen::Options { 48000.0, 120.0, 1, 4 }));
    const auto out = run (engine, in);
    REQUIRE (engine.getStats().capacity == 2000);
    REQUIRE (engine.getStats().tracesStored == 2000);
    REQUIRE (rms (slice (out, in.numSamples() - 48000, 48000)) > 0.01);
    for (float x : out.channels[0])
        REQUIRE (std::isfinite (x));
}
