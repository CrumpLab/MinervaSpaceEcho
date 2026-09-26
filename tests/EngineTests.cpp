#include "mse/EchoEngine.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

namespace {

struct Stereo
{
    std::vector<float> l, r;
    explicit Stereo (int n) : l (static_cast<size_t> (n)), r (static_cast<size_t> (n))
    {
        for (int i = 0; i < n; ++i)
        {
            l[static_cast<size_t> (i)] = std::sin (0.01f * static_cast<float> (i));
            r[static_cast<size_t> (i)] = std::cos (0.013f * static_cast<float> (i));
        }
    }
};

void runBlocks (mse::EchoEngine& engine, Stereo& s, int block)
{
    const int n = static_cast<int> (s.l.size());
    for (int pos = 0; pos < n; pos += block)
    {
        float* ptrs[2] = { s.l.data() + pos, s.r.data() + pos };
        engine.process (ptrs, 2, std::min (block, n - pos), {});
    }
}

} // namespace

TEST_CASE ("Default engine is a bit-exact pass-through")
{
    mse::EchoEngine engine;
    engine.prepare (48000.0, 256, 2);

    Stereo s (10000), ref (10000);
    runBlocks (engine, s, 256);
    REQUIRE (s.l == ref.l);
    REQUIRE (s.r == ref.r);
}

TEST_CASE ("Output gain is applied and settles to the target")
{
    mse::EchoEngine engine;
    mse::EngineParams p;
    p.outputGainDb = -6.0f;
    engine.setParams (p);
    engine.prepare (48000.0, 512, 2); // prepare() snaps gain to the target

    Stereo s (4800), ref (4800);
    runBlocks (engine, s, 512);
    const float g = std::pow (10.0f, -6.0f / 20.0f);
    for (size_t i = 0; i < s.l.size(); ++i)
        REQUIRE (s.l[i] == Catch::Approx (ref.l[i] * g).margin (1e-6));
}

TEST_CASE ("Gain changes are smoothed (no instantaneous jump)")
{
    mse::EchoEngine engine;
    engine.prepare (48000.0, 64, 1);

    mse::EngineParams p;
    p.outputGainDb = -60.0f;
    engine.setParams (p);

    std::vector<float> ones (64, 1.0f);
    float* ptr = ones.data();
    engine.process (&ptr, 1, 64, {});
    REQUIRE (ones.front() > 0.99f);  // first sample barely moved
    REQUIRE (ones.back() < ones.front());
}

TEST_CASE ("Engine reports the host clock it was given")
{
    mse::EchoEngine engine;
    engine.prepare (44100.0, 128, 2);

    mse::HostClock clock;
    clock.hasTempo = true;
    clock.bpm = 97.0;
    clock.ppqPosition = 12.5;

    std::vector<float> buf (128, 0.0f);
    float* ptrs[2] = { buf.data(), buf.data() };
    engine.process (ptrs, 2, 128, clock);
    REQUIRE (engine.getLastClock().bpm == 97.0);
    REQUIRE (engine.getLastClock().ppqPosition == 12.5);
}
