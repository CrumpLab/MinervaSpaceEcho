#pragma once

#include "mse/Features.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace mse {

// Memory configuration (structural: changing it rebuilds the store).
struct MemoryConfig
{
    int capacity = 100;
    double budgetBytes = 1.0e9;     // upper bound on reserved audio memory
    double maxTraceSeconds = 20.0;

    bool operator== (const MemoryConfig&) const = default;
};

// One slot of memory. Audio is valid in [begin, end); anything outside reads
// as silence (begin > 0 when recording started mid-segment).
struct TraceSlot
{
    float* audio[2] = { nullptr, nullptr };
    int64_t begin = 0;
    int64_t end = 0;
    uint64_t serial = 0;      // monotonically increasing store order
    float rms = 0.0f;         // mono RMS of the recorded audio
    FeatureVector features {};
};

// The memory matrix. Holds capacity + 1 preallocated slots: `capacity` traces
// plus one spare that is always the recording target. Committing the spare is
// O(1) (no copying), and a trace evicted to make room becomes the new spare.
//
// Audio storage is reserved uninitialised, so on macOS/Linux untouched pages
// cost no physical RAM: 100 slots of 20 s reserve ~768 MB of address space but
// 1-bar traces only use ~77 MB.
//
// Not thread-safe: owned and used by the audio thread once installed.
class TraceStore
{
public:
    TraceStore (const MemoryConfig& config, double sampleRate, int numChannels);

    int capacity() const noexcept { return cap; }
    int numChannels() const noexcept { return channels; }
    int64_t slotSamples() const noexcept { return slotLen; }
    const MemoryConfig& config() const noexcept { return cfg; }

    int size() const noexcept { return count; }
    // i-th stored trace, oldest first (0 <= i < size()); returns its slot index.
    int storedSlot (int i) const noexcept { return fifo[static_cast<size_t> ((head + i) % cap)]; }

    TraceSlot& slot (int index) noexcept { return slots[static_cast<size_t> (index)]; }
    const TraceSlot& slot (int index) const noexcept { return slots[static_cast<size_t> (index)]; }

    int spareIndex() const noexcept { return spare; }
    TraceSlot& spareSlot() noexcept { return slot (spare); }

    // Moves the spare into memory (evicting the oldest trace when full, FIFO)
    // and returns the slot index it now occupies.
    int commitSpare() noexcept;

    void clear() noexcept;

private:
    MemoryConfig cfg;
    int cap;
    int channels;
    int64_t slotLen;
    std::unique_ptr<float[]> storage;
    std::vector<TraceSlot> slots;
    std::vector<int> fifo;       // ring of stored slot indices
    std::vector<int> freeSlots;  // stack
    int head = 0, count = 0;
    int spare = 0;
    uint64_t nextSerial = 1;
};

} // namespace mse
