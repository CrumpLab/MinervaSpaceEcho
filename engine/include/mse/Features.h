#pragma once

#include "mse/Params.h"

#include <array>
#include <cstdint>
#include <vector>

namespace mse {

// A trace's address: K time slots x B frequency bands (plan §2.2).
constexpr int kSlots = 16;
constexpr int kBands = 24;
constexpr int kFeatureSize = kSlots * kBands;

using FeatureVector = std::array<float, kFeatureSize>; // index = slot * kBands + band

// Frame track (plan §3 Mode C): band levels (dB) every ~20 ms, used for the
// unclocked rolling search. Frame f covers segment samples [f*hop, (f+1)*hop).
constexpr double kFrameSeconds = 0.02;
constexpr int kFrameRing = 128;          // live frames kept for the rolling cue (~2.5 s)
int frameHop (double sampleRate) noexcept;

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
    // samples; it decides which slot each sample position falls into. Complete
    // frames are written to `frameSink` (capacity `sinkFrames` frames of kBands).
    void beginSegment (double nominalLength, float* frameSink = nullptr, int sinkFrames = 0) noexcept;

    // Adds n mono samples written at segment positions startIndex..startIndex+n-1.
    void push (const float* mono, int n, int64_t startIndex) noexcept;

    // Writes the normalised feature vector. Slots that received no audio are 0
    // (MINERVA's "unencoded"). Returns false, with an all-zero vector, if the
    // segment was effectively silent.
    bool finalize (FeatureVector& out, const FeatureSettings& settings) const noexcept;

    float bandCentreHz (int band) const noexcept { return centres[static_cast<size_t> (band)]; }
    int hop() const noexcept { return frameHopSamples; }

    // Frames written to the sink during this segment: [sinkBegin, sinkEnd).
    int sinkBegin() const noexcept { return sinkFirst; }
    int sinkEnd() const noexcept { return sinkLast; }

    // Live frame ring, continuous across segments. Frame i (0 <= i < produced,
    // and produced - i <= kFrameRing) ended at stream sample ringEnd(i).
    int64_t framesProduced() const noexcept { return produced; }
    const float* ringFrame (int64_t i) const noexcept { return ring.data() + static_cast<size_t> (i % kFrameRing) * kBands; }
    float ringEnergy (int64_t i) const noexcept { return ringPower[static_cast<size_t> (i % kFrameRing)]; }
    int64_t ringEnd (int64_t i) const noexcept { return ringEndSample[static_cast<size_t> (i % kFrameRing)]; }
    int64_t streamPosition() const noexcept { return streamSamples; }

    // Computes the frame track of existing audio (e.g. a loaded trace).
    static void computeFrames (const float* mono, int64_t begin, int64_t end, double sampleRate,
                               float* sink, int sinkFrames, int& framesBegin, int& framesEnd);

private:
    void emitFrame (bool toSink) noexcept;

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

    // frames
    int frameHopSamples = 960;
    std::array<double, kBands> frameAcc {};
    double framePower = 0.0;
    int frameCount = 0;
    int64_t frameStartPos = 0;
    float* sink = nullptr;
    int sinkCapacity = 0, sinkFirst = 0, sinkLast = 0;
    std::array<float, static_cast<size_t> (kFrameRing) * kBands> ring {};
    std::array<float, kFrameRing> ringPower {};
    std::array<int64_t, kFrameRing> ringEndSample {};
    int64_t produced = 0;
    int64_t streamSamples = 0;
};

} // namespace mse
