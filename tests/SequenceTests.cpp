// Stage 9: sequential context (Jamieson & Mewhort, 2009). Traces store
// [n-1 | n]; cues can predict what comes next; an echo can cue the next echo.
#include "TestHelpers.h"

#include "mse/Import.h"
#include "mse/Retrieval.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <map>

using namespace mse;
using namespace mse::test;

namespace {

constexpr double kSr = 48000.0;
constexpr int kSeg = 24000; // 500 ms traces
constexpr double kPi = 3.14159265358979323846;

// Distinct segments: two partials each, so their addresses differ clearly.
const std::map<char, std::pair<double, double>> kTones { { 'A', { 220.0, 1320.0 } }, { 'B', { 330.0, 2640.0 } },
                                                          { 'C', { 495.0, 990.0 } },  { 'D', { 660.0, 3960.0 } },
                                                          { 'E', { 880.0, 1760.0 } } };

AudioBuffer tone (char name, int samples = kSeg)
{
    const auto [f1, f2] = kTones.at (name);
    auto b = silence (2, samples);
    for (int i = 0; i < samples; ++i)
        b.channels[0][static_cast<size_t> (i)] = b.channels[1][static_cast<size_t> (i)]
            = 0.2f * static_cast<float> (std::sin (2.0 * kPi * f1 * i / kSr) + 0.5 * std::sin (2.0 * kPi * f2 * i / kSr));
    return b;
}

AudioBuffer sequence (const std::string& names)
{
    AudioBuffer out;
    for (char c : names)
        append (out, c == '-' ? silence (2, kSeg) : tone (c));
    return out;
}

EngineParams contextParams (ContextCue cue)
{
    auto p = wetOnly();
    p.syncMode = SyncMode::Free;
    p.traceMs = 500.0f;
    p.power = 9.0f;
    p.sequenceContext = true;
    p.contextCue = cue;
    return p;
}

double cosine (const FeatureVector& a, const FeatureVector& b)
{
    return similarity (a, b, Similarity::Cosine);
}

// Correlation of each 500 ms output segment (skipping edges) with a tone.
double segmentMatch (const AudioBuffer& out, int segment, char name)
{
    return correlation (slice (out, segment * kSeg + 2000, kSeg - 4000), slice (tone (name), 2000, kSeg - 4000));
}

} // namespace

TEST_CASE ("Traces store the previous segment's address as context")
{
    auto p = contextParams (ContextCue::PredictNext);
    p.sequenceContext = false; // context is recorded whether or not cues use it
    EchoEngine e;
    prepare (e, p, 10);
    run (e, sequence ("ABC"));
    run (e, silence (2, 512));
    MemorySnapshot snap;
    REQUIRE (e.takeSnapshot (snap));
    REQUIRE (snap.traces.size() == 3);
    for (float v : snap.traces[0].context)
        REQUIRE (v == 0.0f); // nothing came before the first segment
    REQUIRE (cosine (snap.traces[1].context, snap.traces[0].features) > 0.999);
    REQUIRE (cosine (snap.traces[2].context, snap.traces[1].features) > 0.999);

    // Saved memory keeps it.
    const auto back = deserializeMemory (serializeMemory (snap).data(), serializeMemory (snap).size());
    REQUIRE (back.traces[2].context == snap.traces[2].context);
    REQUIRE (back.traces[0].context == snap.traces[0].context);
}

TEST_CASE ("Predict Next plays what followed the segment just heard")
{
    auto p = contextParams (ContextCue::PredictNext);
    EchoEngine e;
    e.enableMemoryView();
    prepare (e, p, 20);
    // Learn A B C D twice, then play A and listen to the next segment.
    const auto out = run (e, sequence ("ABCDABCDA-"));
    INFO ("B match " << segmentMatch (out, 9, 'B') << ", C match " << segmentMatch (out, 9, 'C'));
    REQUIRE (segmentMatch (out, 9, 'B') > 0.8);
    // Earlier in the sequence it anticipates too: after the second A, B.
    REQUIRE (segmentMatch (out, 5, 'B') > 0.8);
    REQUIRE (segmentMatch (out, 7, 'D') > 0.8);
}

TEST_CASE ("Match Both tells the same segment apart by what came before it")
{
    // B follows A (then C) and D (then E). The probe [A | B] should recall the
    // B that followed A.
    auto p = contextParams (ContextCue::MatchBoth);
    p.selfMatch = false;
    EchoEngine e;
    e.enableMemoryView();
    prepare (e, p, 20);
    run (e, sequence ("ABCDBE"));
    run (e, sequence ("AB"));
    run (e, silence (2, 2048)); // the [A | B] cue has been retrieved
    run (e, silence (2, 2048));
    const auto* v = e.readMemoryView();
    REQUIRE (v != nullptr);
    REQUIRE (v->count >= 7);
    // Rows: 0 A, 1 B(after A), 2 C, 3 D, 4 B(after D), 5 E, 6 A, ...
    const float afterA = v->rows[1].activation, afterD = v->rows[4].activation;
    INFO ("B after A " << afterA << ", B after D " << afterD);
    REQUIRE (afterA > 2.0f * afterD);

    // Current Only can't tell them apart.
    auto q = p;
    q.contextCue = ContextCue::CurrentOnly;
    EchoEngine f;
    f.enableMemoryView();
    prepare (f, q, 20);
    run (f, sequence ("ABCDBE"));
    run (f, sequence ("AB"));
    run (f, silence (2, 2048));
    run (f, silence (2, 2048));
    const auto* w = f.readMemoryView();
    REQUIRE (w->rows[1].activation == Catch::Approx (w->rows[4].activation).epsilon (0.05));
}

TEST_CASE ("Echo Chain walks through a learned sequence on its own")
{
    auto p = contextParams (ContextCue::PredictNext);
    EchoEngine e;
    prepare (e, p, 20);
    run (e, sequence ("ABCDABCD"));
    p.cueSource = CueSource::EchoChain;
    p.chainInput = 0.0f;
    p.chainStep = ChainStep::Blend; // deterministic: the expected continuation every step
    e.setParams (p);
    // Seed with A, then silence: the chain should play B C D A B C ...
    const auto out = run (e, sequence ("A-------"));
    const std::string expected = "BCDABCD";
    for (int k = 0; k < 7; ++k)
    {
        INFO ("segment " << k + 1 << " expected " << expected[static_cast<size_t> (k)] << ": "
                         << segmentMatch (out, k + 1, expected[static_cast<size_t> (k)]));
        REQUIRE (segmentMatch (out, k + 1, expected[static_cast<size_t> (k)]) > 0.8);
    }
}

TEST_CASE ("Chain Input 1 follows the input exactly like Predict Next")
{
    auto render = [] (bool chain) {
        auto p = contextParams (ContextCue::PredictNext);
        EchoEngine e;
        prepare (e, p, 20);
        run (e, sequence ("ABCDABCD"));
        if (chain)
        {
            p.cueSource = CueSource::EchoChain;
            p.chainInput = 1.0f;
            e.setParams (p);
        }
        return run (e, sequence ("ACBD-"));
    };
    const auto a = render (false), b = render (true);
    double diff = 0.0;
    for (size_t i = 0; i < a.channels[0].size(); ++i)
        diff = std::max (diff, static_cast<double> (std::abs (a.channels[0][i] - b.channels[0][i])));
    REQUIRE (diff < 1.0e-6);
}

TEST_CASE ("With Sequence Context off, the context cue is ignored")
{
    auto render = [] (ContextCue cue) {
        auto p = contextParams (cue);
        p.sequenceContext = false;
        p.power = 3.0f;
        EchoEngine e;
        prepare (e, p, 20);
        return run (e, sequence ("ABCDABCDAC-"));
    };
    const auto a = render (ContextCue::PredictNext), b = render (ContextCue::CurrentOnly);
    REQUIRE (a.channels == b.channels);
}

TEST_CASE ("Imported audio gets sequential context")
{
    const auto audio = sequence ("AB-C");
    ImportSettings is;
    is.traceSeconds = 0.5;
    const auto snap = tracesFromAudio (audio.channels, kSr, kSr, is);
    REQUIRE (snap.traces.size() == 3); // the silent piece is skipped
    for (float v : snap.traces[0].context)
        REQUIRE (v == 0.0f);
    REQUIRE (cosine (snap.traces[1].context, snap.traces[0].features) > 0.999);
    for (float v : snap.traces[2].context) // C followed silence: no context
        REQUIRE (v == 0.0f);
}

namespace {

// Plays `steps` chain steps after seeding with A and returns, for each step,
// which tone the echo sounds most like ('?' if none clearly).
std::string walk (EngineParams p, const std::string& learn, int steps, uint64_t seed)
{
    EchoEngine e;
    e.setSeed (seed);
    prepare (e, p, 40);
    run (e, sequence (learn));
    p.cueSource = CueSource::EchoChain;
    p.chainInput = 0.0f;
    e.setParams (p);
    const auto out = run (e, sequence ("A" + std::string (static_cast<size_t> (steps), '-')));
    std::string heard;
    for (int k = 1; k <= steps; ++k)
    {
        char best = '?';
        double bestMatch = 0.6;
        for (const auto& [name, freqs] : kTones)
            if (const double m = segmentMatch (out, k, name); m > bestMatch)
            {
                bestMatch = m;
                best = name;
            }
        heard += best;
    }
    return heard;
}

} // namespace

TEST_CASE ("Chain Step Sample makes a random walk that follows learned transitions")
{
    // After A comes B or C equally often; B and C are always followed by A.
    auto p = contextParams (ContextCue::PredictNext);
    p.playback = Playback::Sample; // hear exactly the trace that steers
    p.chainStep = ChainStep::Sample;
    const std::string learn = "ABACABACABAC";
    const auto heard = walk (p, learn, 24, 3);
    INFO ("walk: " << heard);
    int b = 0, c = 0, ok = 0;
    for (size_t k = 0; k < heard.size(); ++k)
    {
        b += heard[k] == 'B';
        c += heard[k] == 'C';
        // Every step is a transition memory has seen: A -> B|C, B|C -> A.
        const char prev = k == 0 ? 'A' : heard[k - 1];
        const bool seen = (prev == 'A' && (heard[k] == 'B' || heard[k] == 'C')) || ((prev == 'B' || prev == 'C') && heard[k] == 'A');
        ok += seen ? 1 : 0;
    }
    REQUIRE (b >= 3);  // both branches get visited...
    REQUIRE (c >= 3);
    REQUIRE (ok >= 20); // ...along transitions memory learned

    // Blend never branches: after A it plays the same mixture of B and C
    // every time, so the walk is periodic and the seed makes no difference.
    p.chainStep = ChainStep::Blend;
    p.playback = Playback::Blend;
    const auto blended = walk (p, learn, 12, 3);
    INFO ("blend walk: " << blended);
    REQUIRE (walk (p, learn, 12, 99) == blended);
    for (size_t k = 2; k + 2 < blended.size(); ++k)
        REQUIRE (blended[k] == blended[k + 2]);
}

TEST_CASE ("Habituation moves the walk on from a trace that keeps answering")
{
    // A repeats; with Blend the chain sits on A forever. Habituation tires the
    // A traces so the rare B gets its turn.
    auto p = contextParams (ContextCue::PredictNext);
    p.chainStep = ChainStep::Blend;
    const std::string learn = "AAAAAAAB";
    REQUIRE (walk (p, learn, 10, 1).find ('B') == std::string::npos);
    p.habituation = 0.9f;
    const auto heard = walk (p, learn, 10, 1);
    INFO ("with habituation: " << heard);
    REQUIRE (heard.find ('B') != std::string::npos);
}

TEST_CASE ("Cue Noise changes retrieval and is reproducible")
{
    auto p = contextParams (ContextCue::PredictNext);
    p.chainStep = ChainStep::Sample;
    p.playback = Playback::Sample;
    const std::string learn = "ABACABAC";
    const auto clean = walk (p, learn, 12, 5);
    p.cueNoise = 0.8f;
    const auto noisy1 = walk (p, learn, 12, 5);
    const auto noisy2 = walk (p, learn, 12, 5);
    INFO ("clean " << clean << ", noisy " << noisy1);
    REQUIRE (noisy1 == noisy2);          // deterministic for a seed
    REQUIRE (noisy1 != clean);           // and it does change the walk
}
