#pragma once

#include "mse/HostClock.h"

namespace mse {

const char* versionString() noexcept;

// Parameters the engine reads each block. The plugin and the offline renderer
// both fill this struct, so they always drive the engine identically.
struct EngineParams
{
    float outputGainDb = 0.0f;
};

// Stage 0: a pass-through processor with output gain. Later stages add the
// memory matrix, retrieval and playback heads behind this same interface.
//
// Real-time contract: prepare() may allocate; process() and setParams() never
// allocate, lock or block.
class EchoEngine
{
public:
    static constexpr int kMaxChannels = 2;

    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();

    void setParams (const EngineParams& p) noexcept { params = p; }
    const EngineParams& getParams() const noexcept { return params; }

    // In-place processing. `channels` holds numChannels pointers to
    // numSamples floats each.
    void process (float* const* channels, int numChannels, int numSamples,
                  const HostClock& clock) noexcept;

    double getSampleRate() const noexcept { return sampleRate; }
    const HostClock& getLastClock() const noexcept { return lastClock; }

private:
    EngineParams params;
    HostClock lastClock;
    double sampleRate = 48000.0;
    int maxBlockSize = 0;
    int numChannels = 2;
    float currentGain = 1.0f;   // smoothed linear gain
    float smoothingCoeff = 0.0f;
};

} // namespace mse
