#pragma once

#include <string>
#include <vector>

namespace mse {

// Planar audio buffer: channels[c][i].
struct AudioBuffer
{
    double sampleRate = 48000.0;
    std::vector<std::vector<float>> channels;

    int numChannels() const { return static_cast<int> (channels.size()); }
    int numSamples() const { return channels.empty() ? 0 : static_cast<int> (channels[0].size()); }
    void resize (int numCh, int numSmp)
    {
        channels.assign (static_cast<size_t> (numCh), std::vector<float> (static_cast<size_t> (numSmp), 0.0f));
    }
};

enum class WavFormat { Float32, Pcm24, Pcm16 };

// Reads PCM 16/24/32-bit and IEEE float 32/64-bit WAV (incl. WAVE_FORMAT_EXTENSIBLE).
// Throws std::runtime_error on failure.
AudioBuffer readWav (const std::string& path);

// Throws std::runtime_error on failure.
void writeWav (const std::string& path, const AudioBuffer& audio, WavFormat format = WavFormat::Float32);

} // namespace mse
