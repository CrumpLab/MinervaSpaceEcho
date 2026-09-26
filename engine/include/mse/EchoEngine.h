#pragma once

#include "mse/Features.h"
#include "mse/HostClock.h"
#include "mse/MemorySnapshot.h"
#include "mse/Params.h"
#include "mse/Retrieval.h"
#include "mse/TraceStore.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace mse {

const char* versionString() noexcept;

// What happened to the most recent segment.
enum class WriteOutcome
{
    None,      // nothing recorded, or too short to store
    Stored,
    Merged,    // consolidated into an existing trace
    Gated,     // refused by a write gate (level / novelty / probability / manual mode)
    Rejected,  // memory full and the policy (or clamping) left no room
    Frozen,
};

struct EngineStats
{
    int tracesStored = 0;
    int capacity = 0;
    int clamped = 0;
    double slotSeconds = 0.0;     // longest trace a memory slot can hold
    double traceSeconds = 0.0;    // current nominal trace length
    float segmentPhase = 0.0f;    // 0..1 through the current segment
    float intensity = 0.0f;       // last retrieval: sum of activations
    float maxActivation = 0.0f;
    int activeTraces = 0;         // traces contributing to the current echo
    uint64_t segments = 0;        // traces written (stored or merged) since memory was last cleared
    uint64_t evictions = 0;
    uint64_t merges = 0;
    uint64_t rejections = 0;
    WriteOutcome lastWrite = WriteOutcome::None;
    bool captureArmed = false;
};

// Actions sent from the UI (or any single producer thread) to the audio thread.
enum class Command
{
    Capture = 1,      // store the segment in progress, bypassing write gates (and freeze)
    ClampLast,        // clamp the newest trace
    ClampAll,         // clamp newest-first until the clamp budget is used
    UnclampAll,
    ClearUnclamped,
    ClearAll,
};

// The MINERVA echo (plan §3 Mode A, §5 memory management):
//  - the input is cut into segments of the trace length (free ms or tempo-synced);
//  - each finished segment may be stored as a trace (write gates, policies, clamping);
//  - the same segment's features cue the memory, and the resulting echo (an
//    activation-weighted blend of trace audio) plays during the next segment.
// With capacity 1, self-match on and Lf = 0 this is exactly a delay.
//
// Threading: prepare()/setMemoryConfig()/loadSnapshot()/takeSnapshot()/
// collectGarbage() are non-real-time and must be called from one thread (the
// message thread). process()/setParams() are audio-thread only and never
// allocate or block. sendCommand()/getStats() may be called from any thread.
class EchoEngine
{
public:
    static constexpr int kMaxChannels = 2;
    static constexpr int kMaxCapacity = 1000;

    EchoEngine();
    ~EchoEngine();

    // ---- non-real-time ----
    // Keeps memory if the sample rate and channel count are unchanged.
    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    // New capacity/budget; memory is rebuilt at the next block and traces are
    // carried over (clamped first, then newest).
    void setMemoryConfig (const MemoryConfig& config);
    MemoryConfig getMemoryConfig() const;
    void collectGarbage();
    void setSeed (uint64_t seed) noexcept { seed_ = seed; }

    // Copies memory without stopping audio (writes pause for a moment).
    // Returns false if no consistent copy could be made.
    bool takeSnapshot (MemorySnapshot& out);
    // Replaces memory with the snapshot at the next block (or at prepare()).
    void loadSnapshot (const MemorySnapshot& snapshot);

    // ---- any thread ----
    void sendCommand (Command c) noexcept;
    void requestClear() noexcept { sendCommand (Command::ClearAll); }
    EngineStats getStats() const noexcept;

    // ---- audio thread ----
    void setParams (const EngineParams& p) noexcept { params = p; }
    const EngineParams& getParams() const noexcept { return params; }
    void process (float* const* channels, int numChannels, int numSamples, const HostClock& clock) noexcept;

    double getSampleRate() const noexcept { return sampleRate; }
    const HostClock& getLastClock() const noexcept { return lastClock; }

private:
    struct PlayEntry
    {
        const float* audio[2];
        int64_t begin, end;
        float weight;
        int slot;
        float toneCoeff;   // wear-tone one-pole coefficient (1 = bypass)
        float z[2];
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

    // audio thread helpers
    void resetPlayback() noexcept;
    void adoptPendingStore() noexcept;
    void processCommands() noexcept;
    double nominalLength (const HostClock& clock) const noexcept;
    int64_t recordLimit() const noexcept;
    void boundary (int64_t newBegin, double newNominal) noexcept;
    WriteOutcome writeTrace (int spare, const FeatureVector& feats, float rms, int64_t recorded,
                             int generation, bool forced, int& outSlot) noexcept;
    int chooseVictim() noexcept;
    void mergeInto (TraceSlot& dst, TraceSlot& src) noexcept;
    void applyDropouts (TraceSlot& s) noexcept;
    void applyDecay() noexcept;
    int maxClamped() const noexcept;
    void dropMissingEntries (Playlist& pl) noexcept;
    void processChunk (float* const* io, int ioChannels, int offset, int len) noexcept;
    void accumulate (Playlist& pl, int ioChannels, int len) noexcept;
    void stepLingering() noexcept;
    void beginMutation() noexcept { mutationSeq.fetch_add (1, std::memory_order_acq_rel); }
    void endMutation() noexcept { mutationSeq.fetch_add (1, std::memory_order_release); }
    void publishStats() noexcept;
    float random01() noexcept;

    // non-real-time helpers
    std::unique_ptr<TraceStore> buildStoreFrom (const MemorySnapshot& snapshot, const MemoryConfig& cfg,
                                                double sr, int ch) const;

    EngineParams params;
    HostClock lastClock;
    double sampleRate = 48000.0;
    int maxBlockSize = 0;
    int numChannels = 2;
    uint64_t seed_ = 1;
    uint64_t rngState = 1;

    // memory and its hand-over between threads
    std::unique_ptr<TraceStore> store;
    std::atomic<TraceStore*> liveStore { nullptr };   // for snapshot readers
    mutable std::mutex handoffMutex;
    std::unique_ptr<TraceStore> pendingStore;
    bool pendingKeepsTraces = true;
    std::vector<std::unique_ptr<TraceStore>> retired;
    std::array<std::unique_ptr<TraceStore>, 2> lingering;  // may still be played back
    std::array<int, 2> lingerCountdown {};
    std::atomic<bool> hasPending { false };
    MemoryConfig requestedConfig;
    std::unique_ptr<MemorySnapshot> pendingLoad;        // loaded before prepare()
    bool prepared = false;

    std::atomic<uint64_t> mutationSeq { 0 };           // odd while memory is being changed
    std::atomic<int> holdRequests { 0 };               // >0: don't change memory

    // command queue (single producer, single consumer)
    static constexpr int kQueueSize = 64;
    std::array<std::atomic<int>, kQueueSize> commandQueue {};
    std::atomic<uint32_t> queueHead { 0 }, queueTail { 0 };

    FeatureExtractor features, echoFeatures;
    FeatureVector probe {}, echoProbe {};
    std::vector<EchoWeight> weights;
    Playlist current, previous;
    std::vector<float> echoBuf[kMaxChannels];
    std::vector<float> monoBuf, echoMonoBuf;

    // segment state
    int64_t writeIdx = 0;
    int64_t segBegin = 0;
    double segNominal = 0.0;
    double segEnergy = 0.0, echoSegEnergy = 0.0;
    double expectedPpq = 0.0;
    bool needResync = true;
    bool boundaryPending = false;
    bool lastTempoMode = true;
    bool recordEchoOnly = false;      // this segment records the echo instead of the input
    bool recordEchoToo = false;       // this segment also records the echo to the echo spare
    int segEchoGeneration = 1;
    int64_t fadeSamples = 0;
    bool captureArmed = false;
    bool lastCaptureParam = false;

    // counters
    uint64_t segmentCount = 0, evictionCount = 0, mergeCount = 0, rejectionCount = 0;
    WriteOutcome lastWrite = WriteOutcome::None;

    // smoothed gains
    float dryGain = 1.0f, echoGain = 1.0f, dryInc = 0.0f, echoInc = 0.0f;
    float outGain = 1.0f, outSmoothing = 0.0f;

    struct AtomicStats
    {
        std::atomic<int> tracesStored { 0 }, capacity { 0 }, clamped { 0 }, activeTraces { 0 }, lastWrite { 0 };
        std::atomic<double> slotSeconds { 0.0 }, traceSeconds { 0.0 };
        std::atomic<float> segmentPhase { 0.0f }, intensity { 0.0f }, maxActivation { 0.0f };
        std::atomic<uint64_t> segments { 0 }, evictions { 0 }, merges { 0 }, rejections { 0 };
        std::atomic<bool> captureArmed { false };
    } stats;
};

} // namespace mse
