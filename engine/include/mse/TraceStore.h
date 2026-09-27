#pragma once

#include "mse/Features.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace mse {

// Memory configuration (structural: changing it rebuilds the store; traces are
// carried over, see TraceStore::adoptFrom).
struct MemoryConfig
{
    int capacity = 100;
    double budgetBytes = 1.024e9;   // upper bound on reserved audio memory
    double maxTraceSeconds = 20.0;

    bool operator== (const MemoryConfig&) const = default;
};

// One slot of memory. Audio is valid in [begin, end); anything outside reads
// as silence (begin > 0 when recording started mid-segment). Each slot owns a
// buffer of maxLen samples per channel; buffers can move between stores.
struct TraceSlot
{
    float* audio[2] = { nullptr, nullptr };
    int64_t maxLen = 0;
    int64_t begin = 0;
    int64_t end = 0;

    float* frames = nullptr;  // frame track: maxFrames x kBands band levels (dB)
    int maxFrames = 0;
    int frameBegin = 0;       // valid frames: [frameBegin, frameEnd)
    int frameEnd = 0;

    double nominalLen = 0.0;  // trace length setting when recorded (varispeed reference); 0 = unknown
    uint64_t serial = 0;      // store order; age in segments = store serial - serial
    float rms = 0.0f;         // mono RMS of the recorded audio
    float strength = 1.0f;    // decays over time; multiplies the trace's activation
    float useCount = 0.0f;    // accumulated |activation| (slowly decaying)
    int generation = 0;       // 0 = heard; n = echo of generation n-1 material
    int mergeCount = 1;       // how many segments were consolidated into this trace
    bool clamped = false;     // clamped traces are never replaced (or decayed, optionally)
    uint32_t featureVersion = 0; // bumped whenever `features` change after storing (UI thumbnails)
    FeatureVector features {};

    void resetMeta() noexcept
    {
        begin = end = 0;
        frameBegin = frameEnd = 0;
        nominalLen = 0.0;
        serial = 0;
        rms = 0.0f;
        strength = 1.0f;
        useCount = 0.0f;
        generation = 0;
        mergeCount = 1;
        clamped = false;
        featureVersion = 0;
    }
};

// The memory matrix: `capacity` traces plus kSpares recording slots (one for
// the input, one for the echo). Committing a spare is O(1): the slot simply
// joins memory and a free slot becomes the new spare. No audio is copied.
//
// Audio buffers are reserved uninitialised, so on macOS/Linux untouched pages
// cost no physical RAM.
//
// Not thread-safe: owned and used by the audio thread once installed.
class TraceStore
{
public:
    static constexpr int kSpares = 2;
    static constexpr int kInputSpare = 0;
    static constexpr int kEchoSpare = 1;

    TraceStore (const MemoryConfig& config, double sampleRate, int numChannels);

    int capacity() const noexcept { return cap; }
    int numChannels() const noexcept { return channels; }
    double sampleRate() const noexcept { return rate; }
    int64_t slotSamples() const noexcept { return slotLen; }
    int framesPerSlot() const noexcept { return slotFrames; }
    const MemoryConfig& config() const noexcept { return cfg; }

    int size() const noexcept { return static_cast<int> (order.size()); }
    bool full() const noexcept { return size() >= cap; }
    // i-th stored trace, oldest first (0 <= i < size()); returns its slot index.
    int storedSlot (int i) const noexcept { return order[static_cast<size_t> (i)]; }
    int positionOf (int slotIndex) const noexcept; // -1 if not stored
    int clampedCount() const noexcept;

    TraceSlot& slot (int index) noexcept { return slots[static_cast<size_t> (index)]; }
    const TraceSlot& slot (int index) const noexcept { return slots[static_cast<size_t> (index)]; }

    int spareIndex (int which = kInputSpare) const noexcept { return spares[which]; }
    TraceSlot& spareSlot (int which = kInputSpare) noexcept { return slot (spares[which]); }

    uint64_t currentSerial() const noexcept { return nextSerial; }
    int positionOfSerial (uint64_t serial) const noexcept; // -1 if not stored

    // Moves a spare into memory and returns the slot index it now occupies.
    // Requires !full().
    int commitSpare (int which = kInputSpare) noexcept;

    // Removes the trace at storage position `position`; its slot becomes free.
    void removeAt (int position) noexcept;

    void clear() noexcept;
    void clearUnclamped() noexcept;

    // Audio thread. Takes over `old`'s traces (if keepTraces) and its spare
    // recordings by swapping buffers, so nothing is copied. If this store is
    // smaller, clamped traces are kept first, then the newest. Traces already
    // in this store (imported audio) count as the newest: they keep their
    // place, follow old's traces in storage order and get fresh serials. `old` is left
    // holding the unused buffers and must outlive any playback that still
    // reads evicted traces.
    void adoptFrom (TraceStore& old, bool keepTraces) noexcept;

    // Message thread (before installing): appends a trace with the given audio.
    // Returns the slot index, or -1 when full.
    int appendTrace (const TraceSlot& meta, const float* const* audio, int numChannels, int64_t length);

private:
    void swapBuffers (int mine, TraceStore& other, int theirs) noexcept;
    // Copies metadata/features from src into slot `to`, keeping to's buffers.
    void copyMeta (int to, const TraceSlot& src) noexcept;

    MemoryConfig cfg;
    double rate;
    int cap;
    int channels;
    int64_t slotLen;
    int slotFrames;
    std::vector<std::unique_ptr<float[]>> buffers;      // audio, one per slot
    std::vector<std::unique_ptr<float[]>> frameBuffers; // frame tracks, one per slot
    std::vector<TraceSlot> slots;
    std::vector<int> order;      // stored slot indices, oldest first
    std::vector<int> freeSlots;  // stack
    int spares[kSpares] {};
    uint64_t nextSerial = 1;
};

} // namespace mse
