// Stage 11: the probe. A trace's address used as a cue, for inspection: the
// activations it produces, its similarity to every trace under every address
// set, and its echo (raw, or through the echo's colouring), soloed.
#include "TestHelpers.h"

#include "mse/Retrieval.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace mse;
using namespace mse::test;

namespace {

constexpr double kSr = 48000.0;
constexpr int kSeg = 24000; // 500 ms traces
constexpr double kPi = 3.14159265358979323846;

AudioBuffer note (int midi, int samples = kSeg)
{
    auto b = silence (2, samples);
    const double f = 440.0 * std::pow (2.0, (midi - 69) / 12.0);
    for (int i = 0; i < samples; ++i)
    {
        const double t = i / kSr;
        double x = 0.0;
        for (int k = 1; k <= 12 && f * k < 20000.0; ++k)
            x += std::sin (2.0 * kPi * f * k * t) / k;
        const double env = std::min (1.0, t / 0.01) * std::exp (-t * 1.5) * std::min (1.0, (samples - i) / 480.0);
        b.channels[0][static_cast<size_t> (i)] = b.channels[1][static_cast<size_t> (i)] = static_cast<float> (0.2 * x * env);
    }
    return b;
}

EngineParams probeParams()
{
    auto p = wetOnly();
    p.syncMode = SyncMode::Free;
    p.traceMs = 500.0f;
    p.power = 6.0f;
    p.sequenceContext = true;
    return p;
}

// Plays notes into memory, then pauses: memory stays as it is.
void learn (EchoEngine& e, EngineParams& p, const std::vector<int>& notes)
{
    p.running = true;
    e.setParams (p);
    AudioBuffer in;
    for (int m : notes)
        append (in, note (m));
    append (in, silence (2, 2048)); // the last note is stored at the boundary
    run (e, in);
    p.running = false;
    e.setParams (p);
    run (e, silence (2, 2048));
}

const MemoryView& settle (EchoEngine& e, int samples = 4096)
{
    run (e, silence (2, samples)); // commands run, the view is published
    const auto* v = e.readMemoryView();
    REQUIRE (v != nullptr);
    return *v;
}

int rowOfSerial (const MemoryView& v, uint64_t serial)
{
    for (int i = 0; i < v.count; ++i)
        if (v.rows[static_cast<size_t> (i)].serial == serial)
            return i;
    return -1;
}

int strongestRow (const MemoryView& v)
{
    int best = 0;
    for (int i = 1; i < v.count; ++i)
        if (std::abs (v.rows[static_cast<size_t> (i)].probe) > std::abs (v.rows[static_cast<size_t> (best)].probe))
            best = i;
    return best;
}

double share (const MemoryView& v, int row)
{
    double total = 0.0;
    for (int i = 0; i < v.count; ++i)
        total += std::abs (v.rows[static_cast<size_t> (i)].probe);
    return total > 0.0 ? std::abs (v.rows[static_cast<size_t> (row)].probe) / total : 0.0;
}

} // namespace

TEST_CASE ("A probe gives the activations a cue with that trace's address would")
{
    auto p = probeParams();
    EchoEngine e;
    e.enableMemoryView();
    prepare (e, p, 20);
    learn (e, p, { 60, 62, 64, 65, 67 });
    MemorySnapshot snap;
    REQUIRE (e.takeSnapshot (snap));
    REQUIRE (snap.traces.size() == 5);

    const uint64_t probeSerial = snap.traces[2].serial; // E
    e.sendCommand (Command::ProbeTrace, probeSerial);
    const auto& v = settle (e);
    REQUIRE (v.probeSerial == probeSerial);
    REQUIRE (e.getStats().probeSerial == probeSerial);

    const auto& cue = snap.traces[2].features;
    for (int i = 0; i < 5; ++i)
    {
        const auto& r = v.rows[static_cast<size_t> (i)];
        const auto& t = snap.traces[static_cast<size_t> (i)];
        INFO ("row " << i);
        // Similarity under every set, whatever the Address.
        for (int s = 0; s < kNumSets; ++s)
            REQUIRE (r.probeSims[static_cast<size_t> (s)]
                     == Catch::Approx (setSimilarity (cue.data() + setOffset (s), t.features.data() + setOffset (s), Similarity::Hintzman))
                            .margin (1e-6));
        // Activation: MINERVA's A = S^p (x strength), with the probe itself left out.
        const float expected = i == 2 ? 0.0f : activation (similarity (cue, t.features, Similarity::Hintzman), p.power) * t.strength;
        REQUIRE (r.probe == Catch::Approx (expected).margin (1e-6));
    }
    REQUIRE (v.probeCount == 4);

    // Include itself: the probe answers itself most strongly (S = 1).
    e.sendCommand (Command::ProbeIncludeSelf, 1);
    const auto& v2 = settle (e);
    REQUIRE (v2.probeIncludeSelf);
    REQUIRE (strongestRow (v2) == 2);
    REQUIRE (v2.rows[2].probe == Catch::Approx (1.0f).margin (1e-4));

    e.sendCommand (Command::ClearProbe);
    const auto& v3 = settle (e);
    REQUIRE (v3.probeSerial == 0);
    REQUIRE (v3.rows[0].probe == 0.0f);
}

TEST_CASE ("Changing the Address re-ranks a probe straight away")
{
    // A scale: C D E F G A B C' C. Probing the last C and comparing with the
    // traces' n-1 halves asks "what followed C?" (answer: D).
    auto p = probeParams();
    EchoEngine e;
    e.enableMemoryView();
    prepare (e, p, 20);
    learn (e, p, { 60, 62, 64, 65, 67, 69, 71, 72, 60 });
    const auto& v0 = settle (e);
    REQUIRE (v0.count == 9);
    const uint64_t lastC = v0.rows[8].serial, d = v0.rows[1].serial;

    e.sendCommand (Command::ProbeTrace, lastC);
    e.sendCommand (Command::ProbeCompare, 1);
    p.address = AddressMode::Pitch;
    e.setParams (p);
    const auto& vp = settle (e);
    REQUIRE (vp.probeNext);
    const int dRow = rowOfSerial (vp, d);
    INFO ("pitch: share of [C | D] " << share (vp, dRow));
    REQUIRE (strongestRow (vp) == dRow);
    REQUIRE (share (vp, dRow) > 0.9);

    p.address = AddressMode::Spectrum;
    e.setParams (p);
    const auto& vs = settle (e);
    INFO ("spectrum: share of [C | D] " << share (vs, dRow));
    REQUIRE (share (vs, dRow) < 0.5); // neighbouring notes look alike: many traces answer
}

TEST_CASE ("Probing changes nothing: memory, randomness and the output are untouched")
{
    auto render = [] (bool probing) {
        auto p = probeParams();
        p.dryLevelDb = 0.0f;
        p.cueNoise = 0.2f;       // random streams in use
        p.habituation = 0.5f;
        p.playback = Playback::Sample;
        EchoEngine e;
        e.enableMemoryView();
        prepare (e, p, 20);
        AudioBuffer in;
        for (int m : { 60, 62, 64, 65, 67, 69, 71, 72 })
            append (in, note (m));
        AudioBuffer out;
        for (int k = 0; k < 4; ++k)
        {
            if (probing && k > 0)
            {
                e.sendCommand (Command::ProbeTrace, static_cast<uint64_t> (k));
                e.sendCommand (Command::ProbeCompare, static_cast<uint64_t> (k % 2));
                e.sendCommand (Command::ProbeIncludeSelf, 1);
            }
            append (out, run (e, slice (in, k * 2 * kSeg, 2 * kSeg)));
        }
        MemorySnapshot snap;
        REQUIRE (e.takeSnapshot (snap));
        return std::make_pair (out, snap);
    };
    const auto a = render (false), b = render (true);
    REQUIRE (a.first.channels == b.first.channels);
    REQUIRE (a.second.traces.size() == b.second.traces.size());
    for (size_t i = 0; i < a.second.traces.size(); ++i)
    {
        REQUIRE (a.second.traces[i].features == b.second.traces[i].features);
        REQUIRE (a.second.traces[i].useCount == b.second.traces[i].useCount);
    }
}

TEST_CASE ("The probe's echo plays soloed, raw or through the echo path")
{
    auto p = probeParams();
    p.power = 9.0f;
    p.dryLevelDb = 0.0f; // paused: the dry input passes...
    EchoEngine e;
    e.enableMemoryView();
    prepare (e, p, 20);
    learn (e, p, { 48, 64, 71 });
    const auto& v = settle (e);
    const uint64_t probeSerial = v.rows[1].serial; // E4
    e.sendCommand (Command::ProbeTrace, probeSerial);
    e.sendCommand (Command::ProbeIncludeSelf, 1); // power 9: E answers itself almost alone
    settle (e);

    // ...until the probe's echo plays over it.
    const auto input = note (79, 3 * kSeg);
    e.sendCommand (Command::ProbePlay, 0); // raw, once
    const auto out = run (e, input);
    REQUIRE (e.getStats().probePlaying == false); // finished within the run
    const auto during = slice (out, 1200, kSeg - 2400);
    INFO ("raw echo vs E: " << correlation (during, slice (note (64), 1200, kSeg - 2400)));
    REQUIRE (correlation (during, slice (note (64), 1200, kSeg - 2400)) > 0.95);
    REQUIRE (std::abs (correlation (during, slice (input, 1200, kSeg - 2400))) < 0.1);
    // Afterwards the dry input is back.
    REQUIRE (correlation (slice (out, 2 * kSeg, kSeg / 2), slice (input, 2 * kSeg, kSeg / 2)) > 0.99);

    // Full path: through a dark echo tone and a spring, at the echo level.
    p.echoToneHz = 400.0f;
    p.springLevelDb = -6.0f;
    p.dryLevelDb = kLevelOffDb;
    e.setParams (p);
    run (e, silence (2, 1024));
    e.sendCommand (Command::ProbePlay, 2);
    const auto full = run (e, silence (2, 3 * kSeg));
    auto brightness = [] (const AudioBuffer& b) {
        double d = 0.0, x = 0.0;
        for (size_t i = 1; i < b.channels[0].size(); ++i)
        {
            const double v1 = b.channels[0][i], dv = v1 - b.channels[0][i - 1];
            d += dv * dv;
            x += v1 * v1;
        }
        return x > 0.0 ? d / x : 0.0;
    };
    const auto rawPart = slice (out, 1200, kSeg - 2400), fullPart = slice (full, 1200, kSeg - 2400);
    INFO ("brightness raw " << brightness (rawPart) << ", full " << brightness (fullPart));
    REQUIRE (brightness (fullPart) < 0.5 * brightness (rawPart));
    // The spring rings on after the blend ends.
    REQUIRE (rms (slice (full, kSeg + 2400, kSeg / 2)) > 1.0e-4);
}

TEST_CASE ("Clearing memory ends the probe and its echo")
{
    auto p = probeParams();
    EchoEngine e;
    e.enableMemoryView();
    prepare (e, p, 20);
    learn (e, p, { 60, 64, 67 });
    const auto& v = settle (e);
    e.sendCommand (Command::ProbeTrace, v.rows[0].serial);
    e.sendCommand (Command::ProbePlay, 1); // loop
    settle (e, 2048);
    REQUIRE (e.getStats().probePlaying);
    e.sendCommand (Command::ClearAll);
    const auto& v2 = settle (e);
    REQUIRE (v2.probeSerial == 0);
    REQUIRE_FALSE (e.getStats().probePlaying);
    // Audition and probe echo exclude each other.
    learn (e, p, { 60, 64 });
    const auto& v3 = settle (e);
    e.sendCommand (Command::ProbeTrace, v3.rows[0].serial);
    e.sendCommand (Command::ProbeIncludeSelf, 1);
    e.sendCommand (Command::ProbePlay, 1);
    settle (e, 1024);
    REQUIRE (e.getStats().probePlaying);
    e.sendCommand (Command::AuditionTrace, v3.rows[1].serial);
    settle (e, 1024);
    REQUIRE_FALSE (e.getStats().probePlaying);
    REQUIRE (e.getStats().auditionSerial == v3.rows[1].serial);
}
