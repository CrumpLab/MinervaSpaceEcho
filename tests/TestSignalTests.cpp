#include "TestSignals.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

namespace {

float peak (const mse::tools::AudioBuffer& b)
{
    float p = 0.0f;
    for (const auto& ch : b.channels)
        for (float x : ch)
            p = std::max (p, std::abs (x));
    return p;
}

mse::testgen::Options shortOpts()
{
    mse::testgen::Options o;
    o.bars = 4;
    return o;
}

} // namespace

TEST_CASE ("Test signals are deterministic for a given seed")
{
    const auto o = shortOpts();
    REQUIRE (mse::testgen::fullMix (o).channels == mse::testgen::fullMix (o).channels);

    auto o2 = o;
    o2.seed = 2;
    REQUIRE (mse::testgen::drums (o).channels != mse::testgen::drums (o2).channels);
}

TEST_CASE ("Every clip has the right length, is audible and does not clip")
{
    const auto o = shortOpts();
    const int expected = mse::testgen::samplesPerBar (o) * o.bars;
    REQUIRE (expected == 4 * 96000); // 1 bar @120 BPM, 48 kHz = 2 s

    for (const auto& clip : mse::testgen::generateAll (o))
    {
        INFO (clip.name);
        REQUIRE (clip.audio.numChannels() == 2);
        REQUIRE (clip.audio.numSamples() == expected);
        const float p = peak (clip.audio);
        REQUIRE (p > 0.1f);
        REQUIRE (p <= 1.0f);
    }
}

TEST_CASE ("Impulses land exactly on beats")
{
    const auto o = shortOpts();
    const auto b = mse::testgen::impulses (o);
    const int beat = mse::testgen::samplesPerBar (o) / 4;
    int count = 0;
    for (int i = 0; i < b.numSamples(); ++i)
    {
        const float v = b.channels[0][static_cast<size_t> (i)];
        if (v != 0.0f)
        {
            ++count;
            REQUIRE (i % beat == 0);
            REQUIRE (v == ((i / beat) % 4 == 0 ? 1.0f : 0.5f));
        }
    }
    REQUIRE (count == 16);
}
