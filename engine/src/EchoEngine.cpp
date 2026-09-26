#include "mse/EchoEngine.h"

#include <algorithm>
#include <cmath>

namespace mse {

const char* versionString() noexcept { return MSE_VERSION_STRING; }

static float dbToGain (float db) noexcept { return std::pow (10.0f, db / 20.0f); }

void EchoEngine::prepare (double sr, int maxBlock, int channels)
{
    sampleRate = sr > 0.0 ? sr : 48000.0;
    maxBlockSize = std::max (1, maxBlock);
    numChannels = std::clamp (channels, 1, kMaxChannels);

    // ~20 ms one-pole smoothing for gain changes.
    smoothingCoeff = std::exp (-1.0f / (0.02f * static_cast<float> (sampleRate)));
    reset();
}

void EchoEngine::reset()
{
    currentGain = dbToGain (params.outputGainDb);
    lastClock = {};
}

void EchoEngine::process (float* const* channels, int numCh, int numSamples,
                          const HostClock& clock) noexcept
{
    lastClock = clock;
    numCh = std::min (numCh, kMaxChannels);

    const float target = dbToGain (params.outputGainDb);

    // Fast path: unity gain and settled => exact pass-through.
    if (target == 1.0f && currentGain == 1.0f)
        return;

    float g = currentGain;
    for (int i = 0; i < numSamples; ++i)
    {
        g = target + (g - target) * smoothingCoeff;
        for (int c = 0; c < numCh; ++c)
            channels[c][i] *= g;
    }

    // Snap once close enough so the fast path can resume.
    currentGain = std::abs (g - target) < 1.0e-6f ? target : g;
}

} // namespace mse
