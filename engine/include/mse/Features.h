#pragma once

#include "mse/Params.h"
#include "mse/Spectral.h"

#include <array>
#include <cstdint>
#include <vector>

namespace mse {

// A trace's address is made of address sets (Stage 10), each kSetSize values
// laid out time slot by time slot. The Spectrum set is the original address
// (plan §2.2): K time slots x B frequency bands.
constexpr int kSlots = 16;
constexpr int kBands = 24;
constexpr int kSetSize = kSlots * kBands;

// What each set describes. Every set is normalised (z-scored) on its own.
//  Spectrum    24 log-spaced bands (60 Hz - 16 kHz) x 16 slots: brightness and envelope
//  PitchClass  12 pitch classes (chroma) x 32 slots: harmony and melody, octave-blind
//  Pitch       48 semitones (C2 - B5; lower partials folded up, higher ones left out) x 8 slots:
//              melody with register
//  Timbre      spectral shape (cepstrum, aligned to the fundamental) and brightness (place-coded
//              centroid) x 16 slots: the sound, not the note
//  Rhythm      onset strength in 4 frequency regions x 96 steps: attack pattern
enum class AddressSet { Spectrum, PitchClass, Pitch, Timbre, Rhythm };
constexpr int kNumSets = kNumAddressSets; // see Params.h
constexpr int kAddressSize = kNumSets * kSetSize;

struct SetLayout
{
    int slots; // time slots across the segment
    int width; // values per slot (slot-major: index = slot * width + value)
};
constexpr std::array<SetLayout, kNumSets> kSetLayouts { { { 16, 24 }, { 32, 12 }, { 8, 48 }, { 16, 24 }, { 96, 4 } } };
constexpr int setOffset (int set) noexcept { return set * kSetSize; }
constexpr int setCells (int set) noexcept { return kSetLayouts[static_cast<size_t> (set)].slots * kSetLayouts[static_cast<size_t> (set)].width; }
using SetVector = std::array<float, kSetSize>; // one set's values
const char* addressSetName (int set) noexcept;

constexpr int kPitchLow = 36;      // MIDI note of the Pitch set's first semitone (C2)
constexpr int kPitchCount = 48;
constexpr int kTimbreCoeffs = 12;
constexpr int kRhythmSteps = 96;
constexpr int kRhythmGroups = 4;

using FeatureVector = std::array<float, kAddressSize>; // set s occupies [setOffset (s), setOffset (s) + kSetSize)

// How much each set counts in a comparison (retrieval similarity is the
// weighted mean of the sets' similarities; see Retrieval.h).
using AddressWeights = std::array<float, kNumSets>;
constexpr AddressWeights kSpectrumOnly { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };

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

// Incremental analyser. A bank of band-pass biquads runs on the mono signal;
// squared outputs are pooled into equal slices of the segment's nominal
// length, so traces of any length (10 ms – 20 s) produce comparable vectors.
// A short-time FFT (~85 ms frames, 75 % overlap, restarted at each segment)
// feeds the pitch sets; each frame counts toward the slot its centre falls
// in, so segments shorter than about half a frame have no pitch sets (they
// read as unencoded). Real-time
// safe after prepare().
class FeatureExtractor
{
public:
    // addressSets = false: only the Spectrum set and frames (cheaper).
    void prepare (double sampleRate, bool addressSets = true);

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

    void analyseFrame (int64_t endPosition) noexcept;

    std::array<Biquad, kBands> filters {};
    std::array<float, kBands> centres {};
    std::array<double, kSetSize> energy {};
    std::array<int64_t, kSlots> counts {};
    double slotScale = 0.0;   // kSlots / nominalLength
    double nominal = 1.0;

    // address sets (Stage 10)
    bool sets = false;
    std::array<double, static_cast<size_t> (kRhythmSteps) * kRhythmGroups> rhythmEnergy {};
    std::array<int64_t, kRhythmSteps> rhythmCounts {};
    double rhythmScale = 0.0;
    Fft fft;
    std::vector<float> window, fftRing, fftRe, fftIm;
    double sampleRateHz = 48000.0;
    int fftSize = 0, fftHop = 0, fftFill = 0, fftPos = 0, binLow = 0, binHigh = 0;
    std::array<double, 12 * 32> chromaEnergy {};
    std::array<int, 32> chromaCounts {};
    std::array<double, static_cast<size_t> (kPitchCount) * 8> pitchEnergy {};
    std::array<int, 8> pitchCounts {};

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
