#pragma once

#include "mse/Features.h"
#include "mse/MemorySnapshot.h"

#include <vector>

namespace mse {

// Seeding memory from audio files (plan Stage 7): the audio is cut into
// traces of the current trace length and each gets its address, exactly as
// if it had been played in. Not real-time safe (use on a background thread).
struct ImportSettings
{
    double traceSeconds = 2.0;       // current trace length
    FeatureSettings features {};     // current feature mode
    bool clamp = false;              // lock the imported traces
    int maxTraces = 4000;
    double minFraction = 0.25;       // a shorter tail than this fraction of a trace is dropped
};

// `audio` is [channel][sample] at `sampleRate`; the result is at `targetSampleRate`.
// Silent pieces are skipped.
MemorySnapshot tracesFromAudio (const std::vector<std::vector<float>>& audio, double sampleRate,
                                double targetSampleRate, const ImportSettings& settings);

} // namespace mse
