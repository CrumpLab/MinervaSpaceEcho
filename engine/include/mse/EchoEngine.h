#pragma once

#include "mse/Features.h"
#include "mse/HostClock.h"
#include "mse/MemorySnapshot.h"
#include "mse/MemoryView.h"
#include "mse/Params.h"
#include "mse/Retrieval.h"
#include "mse/Spectral.h"
#include "mse/Tape.h"
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
    uint64_t cueUpdates = 0;      // times the echo was re-cued
    float cueLatencyMs = 0.0f;    // Rolling: age of the cue when its echo started
    int cueMode = 0;              // CueMode in use
    bool windowTooLong = false;   // Rolling: traces are too short to search with this window
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
    // Addressed to one trace by its serial (the argument):
    ClampTrace,       // respects the clamp budget
    UnclampTrace,
    DeleteTrace,
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
    static constexpr int kMaxCapacity = 4000;

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
    // Adds the snapshot's traces to memory at the next block, as the newest
    // traces (see tracesFromAudio() in Import.h). Existing traces make room
    // under the usual rule: clamped first, then newest.
    void importTraces (const MemorySnapshot& additions);

    // Memory matrix view for the UI. enableMemoryView() allocates it (call
    // once, before or while processing); readMemoryView() returns the latest
    // published picture, or nullptr if disabled. Single reader thread; the
    // returned view stays valid until the next readMemoryView() call.
    void enableMemoryView();
    const MemoryView* readMemoryView() noexcept;

    // ---- any thread ----
    void sendCommand (Command c, uint64_t traceSerial = 0) noexcept;
    void requestClear() noexcept { sendCommand (Command::ClearAll); }
    EngineStats getStats() const noexcept;

    // ---- audio thread ----
    // Queues a command from the audio thread itself (e.g. MIDI); it runs at
    // the start of the next process() call.
    void sendCommandFromAudioThread (Command c, uint64_t traceSerial = 0) noexcept;
    void setParams (const EngineParams& p) noexcept { params = p; }
    const EngineParams& getParams() const noexcept { return params; }
    // `sidechain` (optional) cues memory when Cue Source = Sidechain.
    void process (float* const* channels, int numChannels, int numSamples, const HostClock& clock,
                  const float* const* sidechain = nullptr, int sidechainChannels = 0) noexcept;

    double getSampleRate() const noexcept { return sampleRate; }
    const HostClock& getLastClock() const noexcept { return lastClock; }

private:
    struct PlayEntry
    {
        const float* audio[2];
        int64_t begin, end;
        int64_t offset;    // read position = playlist pos + offset (at rate 1)
        float weight;
        int slot;
        float toneCoeff;   // wear-tone one-pole coefficient (1 = bypass)
        float z[2];
        float gain[2];     // per output channel: weight x tracking x head level x pan
        double rate;       // playback speed (varispeed x voice detune); 1 = exact
        int64_t anchor;    // playlist pos where varispeed reading started
        double anchorTrace; // trace position read at `anchor`
        int64_t loopLen;   // > 0: loop the trace (Length Mismatch = Loop)
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
    void clearHeads() noexcept;
    void adoptPendingStore() noexcept;
    void processCommands() noexcept;
    void runCommand (Command c, uint64_t arg) noexcept; // inside a mutation
    double nominalLength (const HostClock& clock) const noexcept;
    int64_t recordLimit() const noexcept;
    void boundary (int64_t newBegin, double newNominal) noexcept;
    WriteOutcome writeTrace (int spare, const FeatureExtractor& fx, const FeatureVector& feats, float rms,
                             int64_t recorded, int generation, bool forced, int& outSlot) noexcept;
    RetrievalSettings retrievalSettings() const noexcept;
    void noteUse (const RetrievalResult& result) noexcept;
    // Installs weights[0..n) on a head, shaped by the playback mode.
    void installEcho (int head, const EchoWeight* w, int n, float tracking, int64_t pos, int64_t fadeOutLen,
                      int64_t fadeInLen) noexcept;
    // Head 1's echo: tracking from the cue level, stats, familiarity.
    void installMain (const RetrievalResult& result, float cueRms, int64_t pos, int64_t fadeOutLen, int64_t fadeInLen) noexcept;
    float trackingFor (const EchoWeight* w, int n, float cueRms) const noexcept;
    void updateOtherHeads (float tracking, int64_t pos) noexcept;
    int shapePlayback (EchoWeight* w, int n) noexcept; // returns new count
    void effectiveHeads (HeadMode modes[kNumHeads], float gains[kNumHeads]) const noexcept;
    FeatureExtractor& cueFx() noexcept { return sidechainActive ? scFeatures : features; }
    bool makeCue (FeatureVector& out, float& cueRms, int64_t recorded) noexcept;
    int64_t slotEdge (int k) const noexcept;
    void progressiveUpdate (int k) noexcept;
    void startRollingSearch() noexcept;
    void continueRollingSearch (int64_t budget) noexcept;
    int64_t refineOffset (const TraceSlot& trace, int64_t offset, int64_t probeEnd, int windowSamples) const noexcept; // audio thread only
    int chooseVictim() noexcept;
    void mergeInto (TraceSlot& dst, TraceSlot& src) noexcept;
    void applyDropouts (TraceSlot& s) noexcept;
    void applyDecay() noexcept;
    int maxClamped() const noexcept;
    void dropMissingEntries (Playlist& pl) noexcept;
    void processChunk (float* const* io, int ioChannels, int offset, int len, const float* const* sc, int scChannels) noexcept;
    void accumulate (Playlist& pl, int head, int ioChannels, int blockOffset, int len) noexcept;
    static void advance (Playlist& pl, int len) noexcept;
    bool spectralHeads() const noexcept;
    void renderSpectral (int head, int ioChannels, int blockOffset, int len) noexcept;
    void spectralFrame (int head, int ioChannels, int blockOffset, int offsetInChunk) noexcept;
    void stepLingering() noexcept;
    void beginMutation() noexcept { mutationSeq.fetch_add (1, std::memory_order_acq_rel); }
    void endMutation() noexcept { mutationSeq.fetch_add (1, std::memory_order_release); }
    void publishStats() noexcept;
    void publishView() noexcept;
    void deleteTrace (int position) noexcept;
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
    std::array<std::atomic<uint64_t>, kQueueSize> commandQueue {}; // command | serial << 8
    std::atomic<uint32_t> queueHead { 0 }, queueTail { 0 };
    std::array<uint64_t, 16> localCommands {};           // from the audio thread
    int localCommandCount = 0;

    FeatureExtractor features, echoFeatures, scFeatures;
    FeatureVector probe {}, echoProbe {}, partialProbe {}, cue {}, frozenCue {};
    float frozenCueRms = 0.0f;
    bool sidechainActive = false;
    double cueSegEnergy = 0.0;               // energy of the cue signal (input or sidechain) this segment
    std::vector<float> scMonoBuf;

    // heads: head 0 is the main echo; current/previous refer to its playlists
    struct PastEcho
    {
        std::vector<EchoWeight> weights;
        std::vector<uint64_t> serials;
        int count = 0;
        float tracking = 1.0f;
    };
    std::array<PastEcho, kNumHeads - 1> history;   // head 1's echo 1 and 2 segments ago
    std::array<std::vector<EchoWeight>, kNumHeads> headWeights;
    std::array<int, kNumHeads> headWeightCount {};
    float familiarity = 0.0f, familiarityTarget = 0.0f;
    float mainTracking = 1.0f;
    FeatureVector iterCue {};
    float headGainNow[kNumHeads] { 1.0f, 0.0f, 0.0f };
    float headGainInc[kNumHeads] {};
    float toneCoeff = 1.0f;
    bool toneActive = false;
    float feedbackNow = 0.0f;

    // tape character
    double installNominal = 0.0;    // segment length the echo being installed will play in (0: no varispeed)
    ShelfEq feedbackEq;
    TapeMotion motion;
    SpringReverb spring;
    std::vector<float> motionBuf;
    bool motionActive = false;
    float driveK = 0.0f;
    float hissGain = 0.0f, hissHp[kMaxChannels] {};
    uint64_t hissRng = 0x9e3779b97f4a7c15ull;
    float springGain = 0.0f;
    bool feedbackEqActive = false;

    // spectral engine: per head, a renderer and an overlap-add ring
    struct SpectralHead
    {
        SpectralRenderer renderer;
        std::array<std::vector<float>, kMaxChannels> ring;
        int ringPos = 0;
        int countdown = 0;       // samples until the next frame
        bool active = false;
    };
    std::array<SpectralHead, kNumHeads> spectral;
    std::vector<SpectralSource> spectralSources;
    std::array<std::vector<float>, kMaxChannels> spectralFrameBuf;
    float toneZ[kMaxChannels] {};

    // live cueing
    int nextSlot = 1;                        // Progressive: next slot edge to act on
    struct RollingState
    {
        bool active = false;
        WindowProbe probe;
        int64_t probeEnd = 0;                // stream sample where the probe ended
        float cueRms = 0.0f;
        int position = 0;                    // next storage position to search
        int count = 0;                       // results so far (in `weights`)
    } rolling;
    std::vector<uint64_t> rollingSerials;    // serial of each result's trace, to detect changes

    // 1 ms loudness envelope of the recorded stream, for sub-frame alignment.
    static constexpr int kEnvRing = 4096;
    std::vector<float> envRing;
    mutable std::vector<float> refineScratch;
    int envStep = 48;                        // samples per envelope point
    int64_t envProduced = 0;                 // points produced; point k covers stream [k*envStep, (k+1)*envStep)
    double envAcc = 0.0;
    int envCount = 0;
    int64_t streamPos = 0;                   // samples recorded since prepare()
    int64_t lastSearchStart = -(int64_t { 1 } << 40);
    uint64_t cueUpdates = 0;
    std::vector<EchoWeight> weights;
    std::array<Playlist, kNumHeads> headCurrent, headPrevious;
    Playlist& current = headCurrent[0];
    Playlist& previous = headPrevious[0];
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
    bool lastTrigger[EngineParams::kNumTriggers] {};

    // memory view (triple buffer: the audio thread fills `viewBack`, then swaps
    // it with the middle; the reader takes the middle when it is newer)
    static constexpr int kViewDirty = 4;
    std::array<std::unique_ptr<MemoryView>, 3> viewBuffers;
    std::atomic<int> viewMiddle { 1 };
    int viewBack = 0, viewFront = 2;
    std::atomic<bool> viewEnabled { false };
    uint64_t viewVersion = 0;
    uint32_t viewStoreEpoch = 0;
    int64_t viewCountdown = 0;
    std::vector<float> viewActivation;                    // per slot
    std::vector<std::array<float, kNumHeads>> viewPlay;   // per slot

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
        std::atomic<uint64_t> segments { 0 }, evictions { 0 }, merges { 0 }, rejections { 0 }, cueUpdates { 0 };
        std::atomic<float> cueLatencyMs { 0.0f };
        std::atomic<int> cueMode { 0 };
        std::atomic<bool> windowTooLong { false };
        std::atomic<bool> captureArmed { false };
    } stats;
};

} // namespace mse
