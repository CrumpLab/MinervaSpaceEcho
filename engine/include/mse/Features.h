#pragma once

#include "mse/Params.h"

#include <array>
#include <cstdint>

namespace mse {

// A trace's address: K time slots x B frequency bands (plan §2.2).
constexpr int kSlots = 16;
constexpr int kBands = 24;
constexpr int kFeatureSize = kSlots * kBands;

using FeatureVector = std::array<float, kFeatureSize>; // index = slot * kBands + band

struct FeatureSettings
{
    FeatureMode mode = FeatureMode::Continuous;
    float ternaryThreshold = 0.5f;
};

// Incremental band-energy analyser. A bank of band-pass biquads runs on the
// mono signal; squared outputs are pooled into kSlots equal slices of the
// segment's nominal length, so traces of any length (10 ms – 20 s) produce
// comparable vectors. Real-time safe after prepare().
class FeatureExtractor
{
public:
    void prepare (double sampleRate);

    // Starts a new segment. nominalLength is the expected trace length in
    // samples; it decides which slot each sample position falls into.
    void beginSegment (double nominalLength) noexcept;

    // Adds n mono samples written at segment positions startIndex..startIndex+n-1.
    void push (const float* mono, int n, int64_t startIndex) noexcept;

    // Writes the normalised feature vector. Slots that received no audio are 0
    // (MINERVA's "unencoded"). Returns false, with an all-zero vector, if the
    // segment was effectively silent.
    bool finalize (FeatureVector& out, const FeatureSettings& settings) const noexcept;

    float bandCentreHz (int band) const noexcept { return centres[static_cast<size_t> (band)]; }

private:
    struct Biquad
    {
        float b0 = 0, b2 = 0, a1 = 0, a2 = 0; // bandpass: b1 == 0
        float z1 = 0, z2 = 0;
    };

    std::array<Biquad, kBands> filters {};
    std::array<float, kBands> centres {};
    std::array<double, kFeatureSize> energy {};
    std::array<int64_t, kSlots> counts {};
    double slotScale = 0.0; // kSlots / nominalLength
};

} // namespace mse
