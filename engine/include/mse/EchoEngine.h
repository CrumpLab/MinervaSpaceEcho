#pragma once

#include "mse/Features.h"
#include "mse/HostClock.h"
#include "mse/Params.h"
#include "mse/Retrieval.h"
#include "mse/TraceStore.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace mse {

const char* versionString() noexcept;

struct EngineStats
{
    int tracesStored = 0;
    int capacity = 0;
    double slotSeconds = 0.0;     // longest trace a memory slot can hold
    double traceSeconds = 0.0;    // current nominal trace length
    float segmentPhase = 0.0f;    // 0..1 through the current segment
    float intensity = 0.0f;       // last retrieval: sum of activations
    float maxActivation = 0.0f;
    int activeTraces = 0;         // traces contributing to the current echo
    uint64_t segments = 0;        // traces committed since last reset
};

// Stage 1 MINERVA echo (plan §3 Mode A):
//  - the input is cut into segments of the trace length (free ms or tempo-synced);
//  - each finished segment is stored as a trace (audio + feature vector);
//  - the same segment's features cue the memory, and the resulting echo (an
//    activation-weighted blend of trace audio) plays during the next segment.
// With capacity 1, self-match on and Lf = 0 this is exactly a delay.
//
// Threading: prepare()/setMemoryConfig()/collectGarbage() are non-real-time.
// process()/setParams() are audio-thread only and never allocate or block.
// requestClear()/getStats() may be called from any thread.
class EchoEngine
{
public:
    static constexpr int kMaxChannels = 2;
    static constexpr int kMaxCapacity = 1000;

    EchoEngine();
    ~EchoEngine();

    // ---- non-real-time ----
    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    // Rebuilds memory with a new capacity/budget; applied at the start of the
    // next process() call. Memory is cleared (Stage 2 will preserve traces).
    void setMemoryConfig (const MemoryConfig& config);
    MemoryConfig getMemoryConfig() const;
    void collectGarbage();
    void setSeed (uint64_t seed) noexcept { seed_ = seed; }

    // ---- any thread ----
    void requestClear() noexcept { clearRequested.store (true); }
    EngineStats getStats() const noexcept;

    // ---- audio thread ----
    void setParams (const EngineParams& p) noexcept { params = p; }
    const EngineParams& getParams() const noexcept { return params; }
    void reset() noexcept;
    void process (float* const* channels, int numChannels, int numSamples, const HostClock& clock) noexcept;

    double getSampleRate() const noexcept { return sampleRate; }
    const HostClock& getLastClock() const noexcept { return lastClock; }

private:
    struct PlayEntry
    {
        const float* audio[2];
        int64_t begin, end;
        float weight;
    };
    struct Playlist
    {
        std::vector<PlayEntry> entries;
        int count = 0;
        int64_t pos = 0;      // read position within traces
        int64_t rampPos = 0;
        int64_t rampLen = 0;  // 0 = no ramp
        bool fadingOut = false;
    };

    void adoptPendingStore() noexcept;
    double nominalLength (const HostClock& clock) const noexcept;
    // Ends the current segment (storing and cueing it) and starts a new one at
    // position newBegin with the given nominal length.
    void boundary (int64_t newBegin, double newNominal) noexcept;
    void processChunk (float* const* io, int ioChannels, int offset, int len) noexcept;
    void accumulate (Playlist& pl, int ioChannels, int len) noexcept;
    void publishStats() noexcept;

    EngineParams params;
    HostClock lastClock;
    double sampleRate = 48000.0;
    int maxBlockSize = 0;
    int numChannels = 2;
    uint64_t seed_ = 1;
    uint64_t rngState = 1;

    std::unique_ptr<TraceStore> store;
    mutable std::mutex handoffMutex;
    std::unique_ptr<TraceStore> pendingStore;
    std::vector<std::unique_ptr<TraceStore>> retired;
    std::atomic<bool> hasPending { false };
    MemoryConfig requestedConfig;
    bool prepared = false;

    FeatureExtractor features;
    FeatureVector probe {};
    std::vector<EchoWeight> weights;
    Playlist current, previous;
    std::vector<float> echoBuf[kMaxChannels];
    std::vector<float> monoBuf;

    // segment state
    int64_t writeIdx = 0;
    double segEnergy = 0.0;       // sum of squares of the recorded (mono) segment
    int64_t segBegin = 0;
    double segNominal = 0.0;
    double expectedPpq = 0.0;
    bool needResync = true;
    bool boundaryPending = false;
    bool lastTempoMode = true;
    int64_t fadeSamples = 0;
    uint64_t segmentCount = 0;

    // smoothed gains
    float dryGain = 1.0f, echoGain = 1.0f, dryInc = 0.0f, echoInc = 0.0f;
    float outGain = 1.0f, outSmoothing = 0.0f;

    std::atomic<bool> clearRequested { false };

    struct AtomicStats
    {
        std::atomic<int> tracesStored { 0 }, capacity { 0 }, activeTraces { 0 };
        std::atomic<double> slotSeconds { 0.0 }, traceSeconds { 0.0 };
        std::atomic<float> segmentPhase { 0.0f }, intensity { 0.0f }, maxActivation { 0.0f };
        std::atomic<uint64_t> segments { 0 };
    } stats;
};

} // namespace mse
