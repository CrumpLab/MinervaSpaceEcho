#pragma once

#include "mse/Features.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mse {

// A self-contained copy of memory: every trace's audio, address and metadata,
// plus the parameters in use when it was taken. Used for Save/Load Memory and
// for embedding memory in a Live set.
struct TraceRecord
{
    int64_t begin = 0;                       // position of audio[.][0] within the segment
    double nominalLength = 0.0;              // trace length setting when recorded (samples; 0 = unknown)
    std::vector<std::vector<float>> audio;   // [channel][sample], the valid [begin, end) region
    uint64_t serial = 0;
    float rms = 0.0f;
    float strength = 1.0f;
    float useCount = 0.0f;
    int generation = 0;
    int mergeCount = 1;
    bool clamped = false;
    FeatureVector features {};
    FeatureVector context {};                // previous segment's address (all 0 = none)

    int64_t length() const { return audio.empty() ? 0 : static_cast<int64_t> (audio[0].size()); }
};

struct MemorySnapshot
{
    static constexpr int kFormatVersion = 1;

    double sampleRate = 48000.0;
    int channels = 2;
    int capacity = 100;
    std::vector<std::pair<std::string, float>> params;  // parameter id -> value
    std::vector<TraceRecord> traces;                     // oldest first
};

// Folder format: <dir>/manifest.json + <dir>/trace_0001.wav ... (32-bit float).
// Throws std::runtime_error on failure.
void writeMemoryFolder (const MemorySnapshot& snapshot, const std::string& dir);
MemorySnapshot readMemoryFolder (const std::string& dir);

// Compact single-blob form (for plugin state). Throws on malformed input.
std::vector<uint8_t> serializeMemory (const MemorySnapshot& snapshot);
MemorySnapshot deserializeMemory (const uint8_t* data, size_t size);

// Linear-interpolation resample of every trace to a new sample rate.
void resampleSnapshot (MemorySnapshot& snapshot, double newSampleRate);

} // namespace mse
