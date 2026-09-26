// Unit tests for the MINERVA pieces: features, similarity, activation, retrieval.
#include "TestHelpers.h"

#include "mse/Features.h"
#include "mse/Retrieval.h"
#include "mse/TraceStore.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace mse;

namespace {

FeatureVector analyse (const tools::AudioBuffer& b, FeatureMode mode = FeatureMode::Continuous, bool* loud = nullptr)
{
    FeatureExtractor fx;
    fx.prepare (b.sampleRate);
    fx.beginSegment (b.numSamples());
    std::vector<float> mono (static_cast<size_t> (b.numSamples()));
    for (size_t i = 0; i < mono.size(); ++i)
        mono[i] = 0.5f * (b.channels[0][i] + b.channels[1][i]);
    fx.push (mono.data(), static_cast<int> (mono.size()), 0);
    FeatureVector f;
    const bool l = fx.finalize (f, { mode, 0.5f });
    if (loud)
        *loud = l;
    return f;
}

testgen::Options oneBar()
{
    testgen::Options o;
    o.bars = 1;
    return o;
}

FeatureVector constant (float v)
{
    FeatureVector f;
    f.fill (v);
    return f;
}

} // namespace

TEST_CASE ("Features: identical audio gives identical features, different audio differs")
{
    const auto drums = testgen::drums (oneBar());
    const auto chords = testgen::chordsBass (oneBar());
    const auto f1 = analyse (drums);
    const auto f2 = analyse (drums);
    const auto f3 = analyse (chords);
    REQUIRE (f1 == f2);
    REQUIRE (similarity (f1, f2, Similarity::Hintzman) == Catch::Approx (1.0).margin (0.02));
    REQUIRE (similarity (f1, f2, Similarity::Cosine) == Catch::Approx (1.0).margin (1e-5));
    REQUIRE (similarity (f1, f3, Similarity::Cosine) < 0.8f);
}

TEST_CASE ("Features: level-independent")
{
    auto quiet = testgen::drums (oneBar());
    for (auto& ch : quiet.channels)
        for (float& x : ch)
            x *= 0.1f;
    const auto loud = analyse (testgen::drums (oneBar()));
    REQUIRE (similarity (loud, analyse (quiet), Similarity::Cosine) > 0.999f);
}

TEST_CASE ("Features: silence is flagged and all-zero; ternary values are -1/0/+1")
{
    bool isLoud = true;
    const auto f = analyse (test::silence (2, 48000), FeatureMode::Continuous, &isLoud);
    REQUIRE_FALSE (isLoud);
    REQUIRE (f == constant (0.0f));

    const auto t = analyse (testgen::fullMix (oneBar()), FeatureMode::Ternary);
    int nonZero = 0;
    for (float x : t)
    {
        REQUIRE ((x == -1.0f || x == 0.0f || x == 1.0f));
        nonZero += x != 0.0f;
    }
    REQUIRE (nonZero > kFeatureSize / 4);
}

TEST_CASE ("Features: unfilled slots stay unencoded (0)")
{
    const auto bar = testgen::fullMix (oneBar());
    FeatureExtractor fx;
    fx.prepare (48000.0);
    fx.beginSegment (bar.numSamples());
    const int half = bar.numSamples() / 2;
    fx.push (bar.channels[0].data() + half, half, half); // second half only
    FeatureVector f;
    REQUIRE (fx.finalize (f, {}));
    for (int s = 0; s < kSlots / 2; ++s)
        for (int b = 0; b < kBands; ++b)
            REQUIRE (f[static_cast<size_t> (s * kBands + b)] == 0.0f);
}

TEST_CASE ("Hintzman similarity counts features non-zero in either vector")
{
    FeatureVector p = constant (0.0f), t = constant (0.0f);
    p[0] = 1; t[0] = 1;   // match
    p[1] = 1; t[1] = -1;  // mismatch
    p[2] = 1;             // probe only
    t[3] = 1;             // trace only
    REQUIRE (similarity (p, t, Similarity::Hintzman) == Catch::Approx (0.0));
    t[1] = 1;
    REQUIRE (similarity (p, t, Similarity::Hintzman) == Catch::Approx (0.5));
    REQUIRE (similarity (constant (1), constant (-1), Similarity::Hintzman) == Catch::Approx (-1.0));
    REQUIRE (similarity (constant (0), constant (1), Similarity::Cosine) == 0.0f);
}

TEST_CASE ("Activation preserves sign and applies power")
{
    REQUIRE (activation (0.5f, 3.0f) == Catch::Approx (0.125));
    REQUIRE (activation (-0.5f, 3.0f) == Catch::Approx (-0.125));
    REQUIRE (activation (-0.5f, 2.0f) == Catch::Approx (-0.25)); // sign kept even for even powers
    REQUIRE (activation (1.0f, 9.0f) == 1.0f);
}

namespace {

// A store with three traces whose features are +1, +0.5-ish and -1.
struct ThreeTraces
{
    MemoryConfig cfg { 4, 1.0e8, 0.01 };
    TraceStore store { cfg, 48000.0, 1 };
    ThreeTraces()
    {
        FeatureVector half = constant (1.0f);
        for (size_t i = 0; i < half.size(); i += 2)
            half[i] = 0.0f; // Hintzman S = 0.5 against all-ones
        for (const auto& f : { constant (1.0f), half, constant (-1.0f) })
        {
            store.spareSlot().features = f;
            store.spareSlot().end = 10;
            store.commitSpare();
        }
    }
};

} // namespace

TEST_CASE ("Retrieval normalisation modes")
{
    ThreeTraces t;
    std::vector<EchoWeight> w (4);
    RetrievalSettings rs;
    rs.power = 1.0f;

    SECTION ("Sum: |weights| add to 1; negative trace subtracts")
    {
        const auto r = retrieve (constant (1.0f), t.store, rs, w.data());
        REQUIRE (r.numWeights == 3);
        REQUIRE (r.sumAbs == Catch::Approx (2.5));
        REQUIRE (r.intensity == Catch::Approx (0.5));
        float s = 0;
        for (int i = 0; i < 3; ++i)
            s += std::abs (w[static_cast<size_t> (i)].weight);
        REQUIRE (s == Catch::Approx (1.0));
        REQUIRE (w[2].weight < 0.0f);
    }
    SECTION ("Max: strongest weight is 1")
    {
        rs.normalization = Normalization::Max;
        retrieve (constant (1.0f), t.store, rs, w.data());
        REQUIRE (w[0].weight == Catch::Approx (1.0));
        REQUIRE (w[1].weight == Catch::Approx (0.5));
    }
    SECTION ("Familiarity: weak total activation stays quiet")
    {
        rs.normalization = Normalization::Familiarity;
        rs.excludeSlot = t.store.storedSlot (0);
        rs.negativeMode = NegativeMode::Ignore;
        const auto r = retrieve (constant (1.0f), t.store, rs, w.data());
        REQUIRE (r.numWeights == 1); // the -1 trace is ignored (activation 0)
        REQUIRE (w[0].weight == Catch::Approx (0.5));
    }
    SECTION ("Absolute: opposite traces add instead of subtracting")
    {
        rs.negativeMode = NegativeMode::Absolute;
        const auto r = retrieve (constant (1.0f), t.store, rs, w.data());
        REQUIRE (r.intensity == Catch::Approx (2.5));
        REQUIRE (w[2].weight > 0.0f);
    }
    SECTION ("High power suppresses weaker matches")
    {
        rs.power = 9.0f;
        rs.normalization = Normalization::Max;
        rs.negativeMode = NegativeMode::Ignore;
        retrieve (constant (1.0f), t.store, rs, w.data());
        REQUIRE (w[1].weight == Catch::Approx (std::pow (0.5, 9.0)));
    }
}

TEST_CASE ("TraceStore evicts FIFO and recycles the evicted slot as the spare")
{
    MemoryConfig cfg { 2, 1.0e8, 0.01 };
    TraceStore store (cfg, 48000.0, 2);
    const int s0 = store.commitSpare();
    const int s1 = store.commitSpare();
    REQUIRE (store.size() == 2);
    const int s2 = store.commitSpare();
    REQUIRE (store.size() == 2);
    REQUIRE (store.storedSlot (0) == s1);
    REQUIRE (store.storedSlot (1) == s2);
    REQUIRE (store.spareIndex() == s0);
    store.clear();
    REQUIRE (store.size() == 0);
}

TEST_CASE ("TraceStore slot length respects the memory budget")
{
    MemoryConfig cfg { 1000, 1.0e9, 20.0 };
    TraceStore store (cfg, 48000.0, 2);
    // 1 GB / (1001 slots * 2 ch * 4 bytes) = ~124875 samples = ~2.6 s
    REQUIRE (store.slotSamples() == 124875);
}
