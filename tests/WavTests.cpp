#include "Wav.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>

using mse::tools::AudioBuffer;
using mse::tools::WavFormat;

namespace {

AudioBuffer makeTestBuffer()
{
    AudioBuffer b;
    b.sampleRate = 44100.0;
    b.resize (2, 1001); // odd length
    for (int i = 0; i < b.numSamples(); ++i)
    {
        b.channels[0][static_cast<size_t> (i)] = 0.9f * std::sin (0.05f * static_cast<float> (i));
        b.channels[1][static_cast<size_t> (i)] = -0.5f + static_cast<float> (i) / 1001.0f;
    }
    return b;
}

std::string tempPath (const char* name)
{
    return (std::filesystem::temp_directory_path() / name).string();
}

} // namespace

TEST_CASE ("WAV float32 round-trip is exact")
{
    const auto in = makeTestBuffer();
    const auto path = tempPath ("mse_wav_f32.wav");
    mse::tools::writeWav (path, in, WavFormat::Float32);
    const auto out = mse::tools::readWav (path);

    REQUIRE (out.sampleRate == 44100.0);
    REQUIRE (out.numChannels() == 2);
    REQUIRE (out.channels == in.channels);
    std::filesystem::remove (path);
}

TEST_CASE ("WAV PCM round-trips within quantisation error")
{
    const auto in = makeTestBuffer();
    for (auto [fmt, tol] : { std::pair { WavFormat::Pcm24, 1.0e-7 }, std::pair { WavFormat::Pcm16, 2.0e-5 } })
    {
        const auto path = tempPath ("mse_wav_pcm.wav");
        mse::tools::writeWav (path, in, fmt);
        const auto out = mse::tools::readWav (path);
        REQUIRE (out.numSamples() == in.numSamples());
        for (size_t c = 0; c < 2; ++c)
            for (size_t i = 0; i < in.channels[c].size(); ++i)
                REQUIRE (out.channels[c][i] == Catch::Approx (in.channels[c][i]).margin (tol));
        std::filesystem::remove (path);
    }
}

TEST_CASE ("Reading a missing or invalid file throws")
{
    REQUIRE_THROWS (mse::tools::readWav (tempPath ("does_not_exist_mse.wav")));
}
