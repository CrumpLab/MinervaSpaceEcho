#include "mse/EchoEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace mse {

const char* versionString() noexcept { return MSE_VERSION_STRING; }

namespace {

constexpr double kJumpToleranceQuarters = 0.02; // host position jumps larger than this re-sync the grid
constexpr double kMinCoverage = 0.5;            // store a segment only if at least this much was recorded
constexpr float kMaxTrackingGain = 4.0f;        // level tracking never boosts memories by more than +12 dB
constexpr float kDeadStrength = 1.0e-3f;        // traces fading below this (-60 dB) are forgotten entirely
constexpr float kUseDecay = 0.98f;              // per segment, so "least used" favours recent use
constexpr int kDropoutGrid = 32;                // tape dropouts: each 1/32 of a trace may drop out
constexpr float kMinMergeWeight = 0.1f;         // consolidation keeps adapting after many merges
constexpr int kSearchMacsPerSample = 400;       // rolling-search work budget (~0.1 ms per 512-sample block)
constexpr int kMaxRefinements = 4;              // rolling matches refined to ~1 ms per search

float levelToGain (float db) noexcept
{
    return db <= kLevelOffDb ? 0.0f : std::pow (10.0f, db / 20.0f);
}

float toDb (double rms) noexcept { return static_cast<float> (20.0 * std::log10 (rms + 1.0e-12)); }

// Linear below |1|, then a smooth knee that saturates at 1.5: keeps feedback
// above 1 from running away without colouring normal levels.
float softClip (float x) noexcept
{
    const float a = std::abs (x);
    if (a <= 1.0f)
        return x;
    return std::copysign (1.0f + 0.5f * std::tanh (2.0f * (a - 1.0f)), x);
}

uint64_t splitmix (uint64_t& state) noexcept
{
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

} // namespace

float EchoEngine::random01() noexcept
{
    return static_cast<float> (splitmix (rngState) >> 40) * (1.0f / 16777216.0f);
}

EchoEngine::EchoEngine()
{
    retired.reserve (8);
    weights.resize (kMaxCapacity);
    rollingSerials.resize (kMaxCapacity);
    envRing.resize (kEnvRing);
    refineScratch.resize (8192);
    for (int h = 0; h < kNumHeads; ++h)
    {
        headCurrent[static_cast<size_t> (h)].entries.resize (kMaxCapacity);
        headPrevious[static_cast<size_t> (h)].entries.resize (kMaxCapacity);
        headWeights[static_cast<size_t> (h)].resize (kMaxCapacity);
    }
    for (auto& past : history)
    {
        past.weights.resize (kMaxCapacity);
        past.serials.resize (kMaxCapacity);
    }
}

EchoEngine::~EchoEngine() = default;

// ==== non-real-time ===============================================================

void EchoEngine::prepare (double sr, int maxBlock, int channels)
{
    sampleRate = sr > 0.0 ? sr : 48000.0;
    maxBlockSize = std::max (1, maxBlock);
    numChannels = std::clamp (channels, 1, kMaxChannels);

    features.prepare (sampleRate);
    echoFeatures.prepare (sampleRate);
    scFeatures.prepare (sampleRate);
    scMonoBuf.assign (static_cast<size_t> (maxBlockSize), 0.0f);
    feedbackEq.prepare (sampleRate);
    motion.prepare (sampleRate);
    spring.prepare (sampleRate);
    motionBuf.assign (static_cast<size_t> (maxBlockSize), 0.0f);
    for (auto& sh : spectral)
    {
        sh.renderer.prepare (sampleRate, kMaxChannels);
        for (auto& r : sh.ring)
            r.assign (static_cast<size_t> (sh.renderer.frameSize()), 0.0f);
        sh.ringPos = sh.countdown = 0;
        sh.active = false;
    }
    spectralSources.resize (kMaxCapacity * 2);
    for (auto& f : spectralFrameBuf)
        f.assign (static_cast<size_t> (spectral[0].renderer.frameSize()), 0.0f);
    envStep = std::max (1, static_cast<int> (std::lround (sampleRate / 1000.0)));
    envProduced = 0;
    envAcc = 0.0;
    envCount = 0;
    streamPos = 0;
    for (auto& b : echoBuf)
        b.assign (static_cast<size_t> (maxBlockSize), 0.0f);
    monoBuf.assign (static_cast<size_t> (maxBlockSize), 0.0f);
    echoMonoBuf.assign (static_cast<size_t> (maxBlockSize), 0.0f);

    MemoryConfig cfg;
    std::unique_ptr<MemorySnapshot> load;
    {
        std::lock_guard lock (handoffMutex);
        cfg = requestedConfig;
        pendingStore.reset();
        hasPending.store (false);
        retired.clear();
        for (auto& l : lingering)
            l.reset();
        load = std::move (pendingLoad);
    }

    const bool compatible = store && std::abs (store->sampleRate() - sampleRate) < 0.5
                            && store->numChannels() == numChannels;
    if (! load && store && ! compatible && store->size() > 0)
        load = std::make_unique<MemorySnapshot> (snapshotOf (*store)); // new rate or channels: resample memory
    if (load)
    {
        store = buildStoreFrom (*load, cfg, sampleRate, numChannels);
    }
    else if (! (compatible && store->config() == cfg))
    {
        auto fresh = std::make_unique<TraceStore> (cfg, sampleRate, numChannels);
        if (compatible)
            fresh->adoptFrom (*store, true); // keep memory across re-prepares
        store = std::move (fresh);
    }
    liveStore.store (store.get(), std::memory_order_release);
    prepared = true;

    outSmoothing = std::exp (-1.0f / (0.02f * static_cast<float> (sampleRate)));
    rngState = seed_;
    segmentCount = static_cast<uint64_t> (store->size());
    resetPlayback();
}

void EchoEngine::setMemoryConfig (const MemoryConfig& requested)
{
    MemoryConfig cfg = requested;
    cfg.capacity = std::clamp (cfg.capacity, 1, kMaxCapacity);

    double sr = 0.0;
    int ch = 0;
    std::unique_ptr<TraceStore> pendingLoadStore;
    bool pendingWasLoad = false;
    {
        std::lock_guard lock (handoffMutex);
        if (cfg == requestedConfig)
            return;
        requestedConfig = cfg;
        if (! prepared)
            return;
        sr = sampleRate;
        ch = numChannels;
        if (pendingStore && (! pendingKeepsTraces || pendingStore->size() > 0))
        {
            // A load or import not yet picked up: re-fit it.
            pendingLoadStore = std::move (pendingStore);
            pendingWasLoad = ! pendingKeepsTraces;
        }
    }

    auto fresh = std::make_unique<TraceStore> (cfg, sr, ch); // allocate outside the lock
    bool keep = true;
    if (pendingLoadStore)
    {
        fresh->adoptFrom (*pendingLoadStore, true);
        keep = ! pendingWasLoad;
    }
    std::unique_ptr<TraceStore> replaced;
    {
        std::lock_guard lock (handoffMutex);
        replaced = std::move (pendingStore);
        pendingStore = std::move (fresh);
        pendingKeepsTraces = keep;
        hasPending.store (true, std::memory_order_release);
    }
}

MemoryConfig EchoEngine::getMemoryConfig() const
{
    std::lock_guard lock (handoffMutex);
    return requestedConfig;
}

void EchoEngine::collectGarbage()
{
    std::lock_guard lock (handoffMutex);
    for (auto& r : retired)
        r.reset();
    retired.clear(); // keeps capacity
}

std::unique_ptr<TraceStore> EchoEngine::buildStoreFrom (const MemorySnapshot& source, const MemoryConfig& cfg,
                                                        double sr, int ch) const
{
    MemorySnapshot s = source;
    resampleSnapshot (s, sr);
    auto st = std::make_unique<TraceStore> (cfg, sr, ch);

    // Keep clamped traces first, then the newest, up to capacity.
    std::vector<bool> keep (s.traces.size(), false);
    int budget = st->capacity();
    for (int pass = 0; pass < 2; ++pass)
        for (size_t i = s.traces.size(); i-- > 0 && budget > 0;)
            if (! keep[i] && s.traces[i].clamped == (pass == 0))
            {
                keep[i] = true;
                --budget;
            }

    for (size_t i = 0; i < s.traces.size(); ++i)
    {
        if (! keep[i])
            continue;
        const auto& t = s.traces[i];
        TraceSlot meta;
        meta.begin = t.begin;
        meta.end = t.begin + t.length();
        meta.nominalLen = t.nominalLength > 0.0 ? t.nominalLength : static_cast<double> (meta.end);
        meta.serial = t.serial;
        meta.rms = t.rms;
        meta.strength = t.strength;
        meta.useCount = t.useCount;
        meta.generation = t.generation;
        meta.mergeCount = t.mergeCount;
        meta.clamped = t.clamped;
        meta.features = t.features;
        meta.context = t.context;

        // Audio laid out from segment position 0 (silence before `begin`);
        // stereo memory loaded into a mono store is mixed down.
        const bool downmix = ch == 1 && t.audio.size() > 1;
        std::vector<std::vector<float>> audio (downmix ? 1 : t.audio.size());
        std::vector<const float*> ptrs;
        for (size_t c = 0; c < audio.size(); ++c)
        {
            audio[c].assign (static_cast<size_t> (t.begin), 0.0f);
            if (downmix)
            {
                for (size_t i = 0; i < t.audio[0].size(); ++i)
                    audio[c].push_back (0.5f * (t.audio[0][i] + t.audio[1][i]));
            }
            else
            {
                audio[c].insert (audio[c].end(), t.audio[c].begin(), t.audio[c].end());
            }
            ptrs.push_back (audio[c].data());
        }
        if (! ptrs.empty())
            st->appendTrace (meta, ptrs.data(), static_cast<int> (ptrs.size()), meta.end);
    }
    return st;
}

void EchoEngine::loadSnapshot (const MemorySnapshot& snapshot)
{
    MemoryConfig cfg;
    double sr;
    int ch;
    {
        std::lock_guard lock (handoffMutex);
        if (! prepared)
        {
            pendingLoad = std::make_unique<MemorySnapshot> (snapshot);
            return;
        }
        cfg = requestedConfig;
        sr = sampleRate;
        ch = numChannels;
    }
    auto fresh = buildStoreFrom (snapshot, cfg, sr, ch);
    std::unique_ptr<TraceStore> replaced;
    {
        std::lock_guard lock (handoffMutex);
        replaced = std::move (pendingStore);
        pendingStore = std::move (fresh);
        pendingKeepsTraces = false;
        hasPending.store (true, std::memory_order_release);
    }
}

void EchoEngine::importTraces (const MemorySnapshot& additions)
{
    if (additions.traces.empty())
        return;
    MemoryConfig cfg;
    double sr;
    int ch;
    {
        std::lock_guard lock (handoffMutex);
        if (! prepared)
        {
            if (! pendingLoad)
            {
                pendingLoad = std::make_unique<MemorySnapshot> (additions);
                return;
            }
            // Append to the memory waiting for prepare().
            auto extra = additions;
            resampleSnapshot (extra, pendingLoad->sampleRate);
            uint64_t serial = 0;
            for (const auto& t : pendingLoad->traces)
                serial = std::max (serial, t.serial);
            for (auto& t : extra.traces)
            {
                t.serial = ++serial;
                pendingLoad->traces.push_back (std::move (t));
            }
            return;
        }
        cfg = requestedConfig;
        sr = sampleRate;
        ch = numChannels;
    }
    auto fresh = buildStoreFrom (additions, cfg, sr, ch);
    std::unique_ptr<TraceStore> replaced;
    {
        std::lock_guard lock (handoffMutex);
        if (pendingStore && pendingKeepsTraces && pendingStore->size() > 0)
            fresh->adoptFrom (*pendingStore, true); // an import still waiting: keep it (as older)
        else if (pendingStore && ! pendingKeepsTraces)
        {
            // A load still waiting: import on top of it and keep it a load.
            auto combined = std::make_unique<TraceStore> (cfg, sr, ch);
            combined->adoptFrom (*pendingStore, true);
            for (int i = 0; i < fresh->size(); ++i)
            {
                const auto& t = fresh->slot (fresh->storedSlot (i));
                auto meta = t;
                meta.serial = 0;
                const float* audio[2] = { t.audio[0], t.audio[1] };
                combined->appendTrace (meta, audio, fresh->numChannels(), t.end);
            }
            replaced = std::move (pendingStore);
            pendingStore = std::move (combined);
            hasPending.store (true, std::memory_order_release);
            return;
        }
        replaced = std::move (pendingStore);
        pendingStore = std::move (fresh);
        pendingKeepsTraces = true;
        hasPending.store (true, std::memory_order_release);
    }
}

void EchoEngine::enableMemoryView()
{
    if (viewEnabled.load())
        return;
    for (auto& b : viewBuffers)
    {
        b = std::make_unique<MemoryView>();
        const size_t slots = kMaxCapacity + TraceStore::kSpares;
        b->rows.resize (kMaxCapacity);
        b->rowSlot.assign (kMaxCapacity, 0);
        b->thumbs.assign (slots * kFeatureSize, 0);
        b->contextThumbs.assign (slots * kFeatureSize, 0);
        b->thumbKeys.assign (slots, ~uint64_t { 0 });
    }
    viewActivation.assign (kMaxCapacity + TraceStore::kSpares, 0.0f);
    viewPlay.assign (kMaxCapacity + TraceStore::kSpares, {});
    viewEnabled.store (true, std::memory_order_release);
}

const MemoryView* EchoEngine::readMemoryView() noexcept
{
    if (! viewEnabled.load (std::memory_order_acquire))
        return nullptr;
    if (viewMiddle.load (std::memory_order_acquire) & kViewDirty)
        viewFront = viewMiddle.exchange (viewFront, std::memory_order_acq_rel) & 3;
    return viewBuffers[static_cast<size_t> (viewFront)].get();
}

MemorySnapshot EchoEngine::snapshotOf (const TraceStore& store_)
{
    const TraceStore* st = &store_;
    MemorySnapshot snap;
    snap.sampleRate = st->sampleRate();
    snap.channels = st->numChannels();
    snap.capacity = st->capacity();
    const int n = st->size();
    snap.traces.reserve (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        const auto& s = st->slot (st->storedSlot (i));
        TraceRecord t;
        const int64_t end = std::min (s.end, s.maxLen);
        const int64_t begin = std::min (s.begin, end);
        t.begin = begin;
        t.audio.resize (static_cast<size_t> (snap.channels));
        for (int c = 0; c < snap.channels; ++c)
            t.audio[static_cast<size_t> (c)].assign (s.audio[c] + begin, s.audio[c] + end);
        t.nominalLength = s.nominalLen;
        t.serial = s.serial;
        t.rms = s.rms;
        t.strength = s.strength;
        t.useCount = s.useCount;
        t.generation = s.generation;
        t.mergeCount = s.mergeCount;
        t.clamped = s.clamped;
        t.features = s.features;
        t.context = s.context;
        snap.traces.push_back (std::move (t));
    }
    return snap;
}

bool EchoEngine::takeSnapshot (MemorySnapshot& out)
{
    {
        std::lock_guard lock (handoffMutex);
        if (! prepared)
        {
            out = pendingLoad ? *pendingLoad : MemorySnapshot {};
            return true;
        }
    }

    // Ask the audio thread to stop changing memory, then copy under a seqlock:
    // the copy is kept only if no change happened while it was being made.
    holdRequests.fetch_add (1);
    bool ok = false;
    for (int attempt = 0; attempt < 500 && ! ok; ++attempt)
    {
        const uint64_t before = mutationSeq.load (std::memory_order_acquire);
        if ((before & 1u) == 0)
        {
            const TraceStore* st = liveStore.load (std::memory_order_acquire);
            MemorySnapshot snap = snapshotOf (*st);
            std::atomic_thread_fence (std::memory_order_acquire);
            if (mutationSeq.load (std::memory_order_relaxed) == before)
            {
                out = std::move (snap);
                ok = true;
                break;
            }
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    holdRequests.fetch_sub (1);
    return ok;
}

// ==== any thread ====================================================================

void EchoEngine::sendCommand (Command c, uint64_t traceSerial) noexcept
{
    const uint32_t tail = queueTail.load (std::memory_order_relaxed);
    if (tail - queueHead.load (std::memory_order_acquire) >= static_cast<uint32_t> (kQueueSize))
        return; // queue full: drop
    commandQueue[tail % kQueueSize].store (static_cast<uint64_t> (c) | (traceSerial << 8), std::memory_order_relaxed);
    queueTail.store (tail + 1, std::memory_order_release);
}

void EchoEngine::sendCommandFromAudioThread (Command c, uint64_t traceSerial) noexcept
{
    if (localCommandCount < static_cast<int> (localCommands.size()))
        localCommands[static_cast<size_t> (localCommandCount++)] = static_cast<uint64_t> (c) | (traceSerial << 8);
}

EngineStats EchoEngine::getStats() const noexcept
{
    EngineStats s;
    s.tracesStored = stats.tracesStored.load (std::memory_order_relaxed);
    s.capacity = stats.capacity.load (std::memory_order_relaxed);
    s.clamped = stats.clamped.load (std::memory_order_relaxed);
    s.activeTraces = stats.activeTraces.load (std::memory_order_relaxed);
    s.slotSeconds = stats.slotSeconds.load (std::memory_order_relaxed);
    s.traceSeconds = stats.traceSeconds.load (std::memory_order_relaxed);
    s.segmentPhase = stats.segmentPhase.load (std::memory_order_relaxed);
    s.intensity = stats.intensity.load (std::memory_order_relaxed);
    s.maxActivation = stats.maxActivation.load (std::memory_order_relaxed);
    s.segments = stats.segments.load (std::memory_order_relaxed);
    s.evictions = stats.evictions.load (std::memory_order_relaxed);
    s.merges = stats.merges.load (std::memory_order_relaxed);
    s.rejections = stats.rejections.load (std::memory_order_relaxed);
    s.cueUpdates = stats.cueUpdates.load (std::memory_order_relaxed);
    s.cueLatencyMs = stats.cueLatencyMs.load (std::memory_order_relaxed);
    s.cueMode = stats.cueMode.load (std::memory_order_relaxed);
    s.windowTooLong = stats.windowTooLong.load (std::memory_order_relaxed);
    s.lastWrite = static_cast<WriteOutcome> (stats.lastWrite.load (std::memory_order_relaxed));
    s.captureArmed = stats.captureArmed.load (std::memory_order_relaxed);
    s.paused = stats.paused.load (std::memory_order_relaxed);
    s.auditionSerial = stats.auditionSerial.load (std::memory_order_relaxed);
    return s;
}

// ==== audio thread ===================================================================

void EchoEngine::resetContext() noexcept
{
    prevHeard.fill (0.0f);
    heardBefore.fill (0.0f);
    prevEchoHeard.fill (0.0f);
    prevCue.fill (0.0f);
    chainValid = false;
}

void EchoEngine::clearHeads() noexcept
{
    for (int h = 0; h < kNumHeads; ++h)
    {
        headCurrent[static_cast<size_t> (h)].count = headPrevious[static_cast<size_t> (h)].count = 0;
        headWeightCount[static_cast<size_t> (h)] = 0;
    }
    for (auto& past : history)
        past.count = 0;
}

void EchoEngine::resetPlayback() noexcept
{
    clearHeads();
    familiarity = familiarityTarget = 0.0f;
    toneZ[0] = toneZ[1] = 0.0f;
    cueSegEnergy = 0.0;
    {
        HeadMode modes[kNumHeads];
        effectiveHeads (modes, headGainNow);
    }
    writeIdx = segBegin = 0;
    segNominal = 0.0;
    segEnergy = echoSegEnergy = 0.0;
    needResync = true;
    boundaryPending = false;
    recordEchoOnly = recordEchoToo = false;
    captureArmed = false;
    rolling.active = false;
    resetContext();
    lastSearchStart = -(int64_t { 1 } << 40);
    nextSlot = 1;
    dryGain = levelToGain (params.dryLevelDb);
    echoGain = levelToGain (params.echoLevelDb);
    fbGain = feedbackNow = std::clamp (params.feedback, 0.0f, 1.2f);
    driveMix = params.tapeDrive > 0.0f ? 1.0f : 0.0f;
    driveK = 1.0f + 4.0f * params.tapeDrive;
    eqBassDb = params.feedbackBassDb;
    eqTrebleDb = params.feedbackTrebleDb;
    outGain = std::pow (10.0f, params.outputGainDb / 20.0f);
    lastClock = {};
    stats.intensity = 0.0f;
    stats.maxActivation = 0.0f;
    stats.activeTraces = 0;
    publishStats();
}

void EchoEngine::adoptPendingStore() noexcept
{
    if (! hasPending.load (std::memory_order_acquire))
        return;
    std::unique_lock lock (handoffMutex, std::try_to_lock);
    if (! lock.owns_lock() || ! pendingStore)
        return;

    int freeLinger = -1;
    for (int k = 0; k < static_cast<int> (lingering.size()); ++k)
        if (! lingering[static_cast<size_t> (k)])
            freeLinger = k;
    if (freeLinger < 0)
        return; // previous swap still winding down; try next block

    beginMutation();
    auto fresh = std::move (pendingStore);
    hasPending.store (false);
    fresh->adoptFrom (*store, pendingKeepsTraces);
    // The old store keeps any evicted audio alive until playback has moved on.
    lingering[static_cast<size_t> (freeLinger)] = std::move (store);
    lingerCountdown[static_cast<size_t> (freeLinger)] = 2;
    store = std::move (fresh);
    liveStore.store (store.get(), std::memory_order_release);
    audition.active = false; // its slot numbering is gone
    audition.solo = 0.0f;
    ++viewStoreEpoch;       // slot numbering changed: redraw every thumbnail
    rolling.active = false; // slot indices refer to the old store
    for (auto& past : history)
        past.count = 0;
    for (auto& c : headWeightCount)
        c = 0;
    if (! pendingKeepsTraces)
    {
        segmentCount = static_cast<uint64_t> (store->size());
        evictionCount = mergeCount = rejectionCount = 0;
    }
    endMutation();
    publishStats();
    viewCountdown = 0;
}

void EchoEngine::stepLingering() noexcept
{
    for (size_t k = 0; k < lingering.size(); ++k)
    {
        if (! lingering[k] || --lingerCountdown[k] > 0)
            continue;
        std::unique_lock lock (handoffMutex, std::try_to_lock);
        if (lock.owns_lock() && retired.size() < retired.capacity())
            retired.push_back (std::move (lingering[k]));
        else
            lingerCountdown[k] = 1; // try again at the next boundary
    }
}

int EchoEngine::maxClamped() const noexcept
{
    return static_cast<int> (std::floor (params.clampBudget * static_cast<float> (store->capacity()) + 1.0e-4f));
}

void EchoEngine::dropMissingEntries (Playlist& pl) noexcept
{
    int kept = 0;
    for (int i = 0; i < pl.count; ++i)
        if (store->positionOf (pl.entries[static_cast<size_t> (i)].slot) >= 0)
            pl.entries[static_cast<size_t> (kept++)] = pl.entries[static_cast<size_t> (i)];
    pl.count = kept;
}

void EchoEngine::processCommands() noexcept
{
    if (holdRequests.load (std::memory_order_acquire) > 0)
        return; // memory must not change while a snapshot is being taken

    // Memory actions as parameters (rising edges).
    static constexpr Command kTriggerCommands[EngineParams::kNumTriggers] = {
        Command::ClampLast, Command::ClampAll, Command::UnclampAll, Command::ClearUnclamped, Command::ClearAll
    };
    bool fired = false;
    for (int t = 0; t < EngineParams::kNumTriggers; ++t)
    {
        if (params.trigger[t] && ! lastTrigger[t])
        {
            if (! fired)
                beginMutation();
            fired = true;
            runCommand (kTriggerCommands[t], 0);
        }
        lastTrigger[t] = params.trigger[t];
    }

    uint32_t head = queueHead.load (std::memory_order_relaxed);
    const uint32_t tail = queueTail.load (std::memory_order_acquire);
    if (head == tail && ! fired && localCommandCount == 0)
        return;

    if (! fired)
        beginMutation();
    for (int i = 0; i < localCommandCount; ++i)
        runCommand (static_cast<Command> (localCommands[static_cast<size_t> (i)] & 0xff), localCommands[static_cast<size_t> (i)] >> 8);
    localCommandCount = 0;
    for (; head != tail; ++head)
    {
        const uint64_t packed = commandQueue[head % kQueueSize].load (std::memory_order_relaxed);
        runCommand (static_cast<Command> (packed & 0xff), packed >> 8);
    }
    queueHead.store (head, std::memory_order_release);
    endMutation();
    publishStats();
    viewCountdown = 0; // show the change right away
}

void EchoEngine::deleteTrace (int position) noexcept
{
    rolling.active = false;
    store->removeAt (position);
    for (int h = 0; h < kNumHeads; ++h)
    {
        dropMissingEntries (headCurrent[static_cast<size_t> (h)]);
        dropMissingEntries (headPrevious[static_cast<size_t> (h)]);
        headWeightCount[static_cast<size_t> (h)] = 0;
    }
}

void EchoEngine::runCommand (Command c, uint64_t arg) noexcept
{
    switch (c)
    {
        case Command::Capture:
            captureArmed = true;
            break;
        case Command::ClampLast:
            if (store->size() > 0 && store->clampedCount() < maxClamped())
                store->slot (store->storedSlot (store->size() - 1)).clamped = true;
            break;
        case Command::ClampAll:
        {
            int budget = maxClamped() - store->clampedCount();
            for (int i = store->size() - 1; i >= 0 && budget > 0; --i)
            {
                auto& s = store->slot (store->storedSlot (i));
                if (! s.clamped)
                {
                    s.clamped = true;
                    --budget;
                }
            }
            break;
        }
        case Command::UnclampAll:
            for (int i = 0; i < store->size(); ++i)
                store->slot (store->storedSlot (i)).clamped = false;
            break;
        case Command::ClearUnclamped:
            rolling.active = false;
            store->clearUnclamped();
            for (int h = 0; h < kNumHeads; ++h)
            {
                dropMissingEntries (headCurrent[static_cast<size_t> (h)]);
                dropMissingEntries (headPrevious[static_cast<size_t> (h)]);
            }
            for (auto& past : history)
                past.count = 0;
            break;
        case Command::ClearAll:
            rolling.active = false;
            store->clear();
            clearHeads();
            resetContext();
            segmentCount = evictionCount = mergeCount = rejectionCount = 0;
            writeIdx = segBegin = 0; // drop the half-recorded segment too
            segEnergy = echoSegEnergy = 0.0;
            needResync = true;
            boundaryPending = false;
            break;
        case Command::AuditionTrace:
        case Command::AuditionLoop:
        case Command::AuditionPair:
        {
            const int pos = store->positionOfSerial (arg);
            if (pos < 0 || arg == 0)
                break;
            audition.firstSerial = arg;
            audition.nextSerial = 0;
            if (c == Command::AuditionPair)
            {
                // [n-1 | n]: the segment before it, if that is still in memory.
                if (store->positionOfSerial (arg - 1) >= 0)
                {
                    audition.firstSerial = arg - 1;
                    audition.nextSerial = arg;
                }
            }
            audition.serial = audition.firstSerial;
            audition.slot = store->storedSlot (store->positionOfSerial (audition.serial));
            audition.pos = store->slot (audition.slot).begin;
            audition.loop = c == Command::AuditionLoop;
            audition.active = true;
            break;
        }
        case Command::StopAudition:
            audition.active = false;
            break;
        case Command::ClampTrace:
        case Command::UnclampTrace:
        case Command::DeleteTrace:
        {
            const int pos = store->positionOfSerial (arg);
            if (pos < 0 || arg == 0)
                break;
            auto& s = store->slot (store->storedSlot (pos));
            if (c == Command::ClampTrace)
            {
                if (! s.clamped && store->clampedCount() < maxClamped())
                    s.clamped = true;
            }
            else if (c == Command::UnclampTrace)
            {
                s.clamped = false;
            }
            else
            {
                deleteTrace (pos);
            }
            break;
        }
    }
}

double EchoEngine::nominalLength (const HostClock& clock) const noexcept
{
    double len;
    if (params.syncMode == SyncMode::Tempo)
    {
        const double quarters = divisionQuarters (params.traceDivision, clock.timeSigNumerator, clock.timeSigDenominator);
        len = quarters * 60.0 / std::max (1.0, clock.bpm) * sampleRate;
    }
    else
    {
        len = params.traceMs * 0.001 * sampleRate;
    }
    return std::clamp (len, 16.0, static_cast<double> (store->slotSamples()));
}

int64_t EchoEngine::recordLimit() const noexcept
{
    return std::min (store->spareSlot (TraceStore::kInputSpare).maxLen, store->spareSlot (TraceStore::kEchoSpare).maxLen);
}

void EchoEngine::process (float* const* io, int ioChannels, int numSamples, const HostClock& clock,
                          const float* const* sidechain, int sidechainChannels) noexcept
{
    lastClock = clock;
    if (store == nullptr || numSamples <= 0)
        return;
    ioChannels = std::clamp (ioChannels, 1, kMaxChannels);

    // Non-finite input (a misbehaving plugin upstream) must never reach memory.
    for (int c = 0; c < ioChannels; ++c)
        for (int i = 0; i < numSamples; ++i)
            if (! std::isfinite (io[c][i]))
                io[c][i] = 0.0f;

    // Hosts may send more than they announced (offline bounces): split.
    if (numSamples > maxBlockSize)
    {
        const double samplesPerQuarter = sampleRate * 60.0 / std::max (1.0, clock.bpm);
        for (int done = 0; done < numSamples; done += maxBlockSize)
        {
            const int len = std::min (maxBlockSize, numSamples - done);
            float* part[kMaxChannels] {};
            const float* scPart[kMaxChannels] {};
            for (int c = 0; c < ioChannels; ++c)
                part[c] = io[c] + done;
            const int scCh = std::clamp (sidechainChannels, 0, kMaxChannels);
            for (int c = 0; c < scCh && sidechain != nullptr; ++c)
                scPart[c] = sidechain[c] + done;
            HostClock sub = clock;
            sub.ppqPosition = clock.ppqPosition + done / samplesPerQuarter;
            processBlock (part, ioChannels, len, sub, sidechain != nullptr ? scPart : nullptr, scCh);
        }
        lastClock = clock;
        return;
    }
    processBlock (io, ioChannels, numSamples, clock, sidechain, sidechainChannels);
}

void EchoEngine::processBlock (float* const* io, int ioChannels, int numSamples, const HostClock& clock,
                               const float* const* sidechain, int sidechainChannels) noexcept
{
    lastClock = clock;
    ioChannels = std::clamp (ioChannels, 1, kMaxChannels);
    sidechainActive = params.cueSource == CueSource::Sidechain && sidechain != nullptr && sidechainChannels > 0;

    if (holdRequests.load (std::memory_order_acquire) == 0)
        adoptPendingStore();
    processCommands();

    if (params.capture && ! lastCaptureParam)
        captureArmed = true; // rising edge of the Capture parameter
    lastCaptureParam = params.capture;

    // Running / paused. Pausing lets this block run with the echo fading
    // out; after that memory is left untouched and only the dry signal (and
    // any audition) is heard. Resuming starts cleanly, like a transport start.
    const bool pausing = ! params.running && ! paused;
    if (params.running && paused)
    {
        paused = false;
        clearHeads();
        resetContext();
        writeIdx = segBegin = 0;
        segEnergy = echoSegEnergy = cueSegEnergy = 0.0;
        needResync = true;
        boundaryPending = false;
        echoGain = 0.0f; // the echo fades back in
        springGain = 0.0f;
    }
    if (paused)
        processPaused (io, ioChannels, numSamples);
    else
        runMemory (io, ioChannels, numSamples, clock, sidechain, sidechainChannels, pausing);
    if (pausing)
        paused = true;

    mixAudition (io, ioChannels, numSamples);

    // Output gain (one-pole smoothed; exact pass-through when settled at unity).
    const float target = std::pow (10.0f, params.outputGainDb / 20.0f);
    if (! (target == 1.0f && outGain == 1.0f))
    {
        float g = outGain;
        for (int i = 0; i < numSamples; ++i)
        {
            g = target + (g - target) * outSmoothing;
            for (int c = 0; c < ioChannels; ++c)
                io[c][i] *= g;
        }
        outGain = std::abs (g - target) < 1.0e-6f ? target : g;
    }

    viewCountdown -= numSamples;
    if (viewCountdown <= 0)
    {
        viewCountdown = static_cast<int64_t> (sampleRate / 30.0);
        publishView();
    }
    stats.paused.store (paused, std::memory_order_relaxed);
}

void EchoEngine::processPaused (float* const* io, int ioChannels, int numSamples) noexcept
{
    const float target = levelToGain (params.dryLevelDb);
    const float inc = (target - dryGain) / static_cast<float> (numSamples);
    for (int c = 0; c < ioChannels; ++c)
    {
        float g = dryGain;
        for (int i = 0; i < numSamples; ++i)
        {
            io[c][i] *= g;
            g += inc;
        }
    }
    dryGain = target;
    echoGain = 0.0f;
    springGain = springTarget = 0.0f;
}

void EchoEngine::runMemory (float* const* io, int ioChannels, int numSamples, const HostClock& clock,
                            const float* const* sidechain, int sidechainChannels, bool fadeOut) noexcept
{
    fadeSamples = static_cast<int64_t> (std::llround (params.edgeFadeMs * 0.001 * sampleRate));

    const double nominal = nominalLength (clock);
    const auto period = static_cast<int64_t> (std::llround (nominal));
    const bool tempo = params.syncMode == SyncMode::Tempo;
    if (tempo != lastTempoMode)
        needResync = true;
    lastTempoMode = tempo;

    int64_t firstBoundary;
    if (tempo)
    {
        const double spq = sampleRate * 60.0 / std::max (1.0, clock.bpm);
        const double segQ = nominal / spq; // equals the division unless clamped
        const double ppq = clock.ppqPosition;
        if (std::abs (ppq - expectedPpq) > kJumpToleranceQuarters)
            needResync = true;
        expectedPpq = ppq + numSamples / spq;

        const double phaseS = (ppq - std::floor (ppq / segQ) * segQ) * spq;
        auto toBoundary = static_cast<int64_t> (std::llround (nominal - phaseS));

        if (needResync)
        {
            auto begin = static_cast<int64_t> (std::llround (phaseS));
            if (begin >= period || toBoundary <= 0)
            {
                begin = 0;
                toBoundary = period;
            }
            boundary (begin, nominal);
            boundaryPending = false;
            resetContext(); // after a jump the next segment doesn't follow the last one
        }
        firstBoundary = toBoundary <= 0 ? 0 : toBoundary;
        if (boundaryPending)
            firstBoundary = 0;
    }
    else
    {
        if (needResync)
        {
            boundary (0, nominal);
            boundaryPending = false;
        }
        firstBoundary = std::max<int64_t> (0, period - writeIdx);
    }
    needResync = false;
    boundaryPending = false;

    // Per-block linear gain ramps.
    const float n = static_cast<float> (numSamples);
    dryInc = (levelToGain (params.dryLevelDb) - dryGain) / n;
    echoInc = ((fadeOut ? 0.0f : levelToGain (params.echoLevelDb)) - echoGain) / n;
    {
        HeadMode modes[kNumHeads];
        float target[kNumHeads];
        effectiveHeads (modes, target);
        for (int h = 0; h < kNumHeads; ++h)
            headGainInc[h] = (target[h] - headGainNow[h]) / n;
    }

    // Familiarity (the strongest activation of the main echo), smoothed, can
    // steer the echo's tone and the feedback.
    familiarity += (familiarityTarget - familiarity) * (1.0f - std::exp (-n / (0.1f * static_cast<float> (sampleRate))));
    const float fam = 2.0f * familiarity - 1.0f; // -1 unfamiliar .. +1 familiar
    feedbackNow = std::clamp (params.feedback + 0.5f * params.intensityToFeedback * fam, 0.0f, 1.2f);
    fbInc = (feedbackNow - fbGain) / n; // glides over the block
    toneActive = params.echoToneHz < 19999.0f || params.intensityToTone != 0.0f;

    // Tape character.
    motionActive = params.wow > 0.0f || params.flutter > 0.0f || motion.isMoving();
    // Drive: the curve's steepness glides, and switching it on or off
    // crossfades between clean and saturated.
    {
        const bool on = params.tapeDrive > 0.0f;
        const float kTarget = on ? 1.0f + 4.0f * params.tapeDrive : driveK;
        if (driveMix <= 0.0f && on)
            driveK = kTarget;
        driveKInc = (kTarget - driveK) / n;
        driveMixInc = ((on ? 1.0f : 0.0f) - driveMix) / n;
    }
    hissGain = params.hissDb > kGateOffDb ? std::pow (10.0f, params.hissDb / 20.0f) : 0.0f;
    // The feedback shelves move at most 0.25 dB per block (~20 dB/s), so
    // sweeping them doesn't step what gets recorded.
    eqBassDb += std::clamp (params.feedbackBassDb - eqBassDb, -0.25f, 0.25f);
    eqTrebleDb += std::clamp (params.feedbackTrebleDb - eqTrebleDb, -0.25f, 0.25f);
    feedbackEq.set (eqBassDb, eqTrebleDb);
    feedbackEqActive = ! feedbackEq.isFlat();
    const float newSpringGain = fadeOut ? 0.0f : levelToGain (params.springLevelDb);
    if (newSpringGain > 0.0f && springGain <= 0.0f)
        spring.reset(); // start from silence, not an old tail
    springTarget = newSpringGain;
    springInc = (springTarget - springGain) / n; // glides over the block
    if (springTarget > 0.0f || springGain > 0.0f)
        spring.setDecay (params.springDecaySeconds);
    if (toneActive)
    {
        const double fc = std::clamp (params.echoToneHz * std::pow (2.0, 3.0 * params.intensityToTone * fam), 20.0,
                                      0.45 * sampleRate);
        const auto target = static_cast<float> (1.0 - std::exp (-2.0 * 3.14159265358979 * fc / sampleRate));
        if (! toneWasActive)
            toneCoeff = target;
        toneInc = (target - toneCoeff) / n; // the cutoff glides over the block
    }
    toneWasActive = toneActive;

    // Rolling cue: search memory incrementally, a bounded amount per block.
    if (params.cueMode == CueMode::Rolling)
    {
        const auto interval = static_cast<int64_t> (params.rollingIntervalMs * 0.001 * sampleRate);
        const bool liveCue = params.cueSource == CueSource::Input || params.cueSource == CueSource::Sidechain;
        if (liveCue && ! rolling.active && cueFx().streamPosition() - lastSearchStart >= interval)
            startRollingSearch();
        if (rolling.active)
            continueRollingSearch (static_cast<int64_t> (kSearchMacsPerSample) * numSamples);
    }
    else
    {
        rolling.active = false;
    }

    int done = 0;
    int64_t nextBoundary = std::min (firstBoundary, recordLimit() - writeIdx);
    while (done < numSamples)
    {
        if (nextBoundary <= 0)
        {
            boundary (0, nominal);
            nextBoundary = std::min (period, recordLimit());
            continue;
        }
        int64_t toEvent = nextBoundary;
        if (params.cueMode == CueMode::Progressive)
        {
            // Progressive cue: re-cue memory at every slot edge of the bar.
            while (nextSlot < kSlots && slotEdge (nextSlot) <= writeIdx)
                progressiveUpdate (nextSlot++);
            if (nextSlot < kSlots)
                toEvent = std::min (toEvent, slotEdge (nextSlot) - writeIdx);
        }
        const int chunk = static_cast<int> (std::min<int64_t> (numSamples - done, toEvent));
        processChunk (io, ioChannels, done, chunk, sidechain, sidechainChannels);
        done += chunk;
        nextBoundary -= chunk;
    }
    if (nextBoundary <= 0)
        boundaryPending = true; // segment ended exactly on the block edge

    dryGain = levelToGain (params.dryLevelDb);
    echoGain = fadeOut ? 0.0f : levelToGain (params.echoLevelDb);
    springGain = springTarget;
    fbGain = feedbackNow;
    driveMix = std::round (driveMix); // settles exactly at 0 or 1
    {
        HeadMode modes[kNumHeads];
        effectiveHeads (modes, headGainNow);
    }

    stats.traceSeconds.store (nominal / sampleRate, std::memory_order_relaxed);
    stats.segmentPhase.store (static_cast<float> (std::clamp (writeIdx / std::max (1.0, segNominal), 0.0, 1.0)),
                              std::memory_order_relaxed);
    stats.captureArmed.store (captureArmed, std::memory_order_relaxed);
    stats.cueMode.store (static_cast<int> (params.cueMode), std::memory_order_relaxed);
    stats.windowTooLong.store (params.cueMode == CueMode::Rolling
                                   && params.rollingWindowMs + 2.0f * kFrameSeconds * 1000.0f >= 1000.0 * nominal / sampleRate,
                               std::memory_order_relaxed);
}

// ---- memory writes ---------------------------------------------------------------------

int EchoEngine::chooseVictim() noexcept
{
    const int n = store->size();
    int best = -1;
    switch (params.fullPolicy)
    {
        case FullPolicy::Oldest:
            for (int i = 0; i < n; ++i)
                if (! store->slot (store->storedSlot (i)).clamped)
                    return i;
            return -1;

        case FullPolicy::Random:
        {
            int unclamped = 0;
            for (int i = 0; i < n; ++i)
                unclamped += store->slot (store->storedSlot (i)).clamped ? 0 : 1;
            if (unclamped == 0)
                return -1;
            int k = static_cast<int> (random01() * static_cast<float> (unclamped));
            k = std::min (k, unclamped - 1);
            for (int i = 0; i < n; ++i)
                if (! store->slot (store->storedSlot (i)).clamped && k-- == 0)
                    return i;
            return -1;
        }

        case FullPolicy::LeastUsed:
        case FullPolicy::Weakest:
        {
            float bestValue = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                const auto& s = store->slot (store->storedSlot (i));
                if (s.clamped)
                    continue;
                const float v = params.fullPolicy == FullPolicy::LeastUsed ? s.useCount : s.strength;
                if (best < 0 || v < bestValue) // strict: ties go to the oldest
                {
                    best = i;
                    bestValue = v;
                }
            }
            return best;
        }

        case FullPolicy::MergeSimilar:
        case FullPolicy::Reject:
            return -1;
    }
    return -1;
}

void EchoEngine::mergeInto (TraceSlot& dst, TraceSlot& src) noexcept
{
    const float w = std::max (1.0f / static_cast<float> (dst.mergeCount + 1), kMinMergeWeight);
    const int64_t srcEnd = std::min (src.end, src.maxLen);
    const int64_t b = std::min (dst.begin, src.begin);
    const int64_t e = std::min (std::max (dst.end, srcEnd), dst.maxLen);
    const int ch = store->numChannels();

    double energy = 0.0;
    for (int c = 0; c < ch; ++c)
    {
        float* d = dst.audio[c];
        const float* s = src.audio[c];
        for (int64_t t = b; t < e; ++t)
        {
            const bool inD = t >= dst.begin && t < dst.end;
            const bool inS = t >= src.begin && t < srcEnd;
            const float v = (inD && inS) ? d[t] * (1.0f - w) + s[t] * w : (inD ? d[t] : (inS ? s[t] : 0.0f));
            d[t] = v;
            energy += static_cast<double> (v) * v;
        }
    }
    dst.begin = b;
    dst.end = std::max (b, e);
    for (size_t j = 0; j < dst.features.size(); ++j)
    {
        dst.features[j] = dst.features[j] * (1.0f - w) + src.features[j] * w;
        dst.context[j] = dst.context[j] * (1.0f - w) + src.context[j] * w;
    }
    ++dst.featureVersion;

    // Frame tracks (dB levels) merge the same way over their union.
    const int fb = std::min (dst.frameBegin, src.frameBegin);
    const int fe = std::min ({ std::max (dst.frameEnd, src.frameEnd), dst.maxFrames, src.maxFrames });
    for (int f = fb; f < fe; ++f)
    {
        const bool inD = f >= dst.frameBegin && f < dst.frameEnd;
        const bool inS = f >= src.frameBegin && f < src.frameEnd;
        float* d = dst.frames + static_cast<size_t> (f) * kBands;
        const float* sf = src.frames + static_cast<size_t> (f) * kBands;
        for (int b = 0; b < kBands; ++b)
            d[b] = (inD && inS) ? d[b] * (1.0f - w) + sf[b] * w : (inD ? d[b] : (inS ? sf[b] : -120.0f));
    }
    if (fe > fb)
    {
        dst.frameBegin = fb;
        dst.frameEnd = fe;
    }
    dst.rms = e > b ? static_cast<float> (std::sqrt (energy / (static_cast<double> (ch) * static_cast<double> (e - b)))) : 0.0f;
    dst.mergeCount += 1;
    dst.strength = 1.0f;
    dst.generation = std::max (dst.generation, src.generation);
}

void EchoEngine::applyDropouts (TraceSlot& s) noexcept
{
    const int64_t len = s.end - s.begin;
    if (len < kDropoutGrid * 4)
        return;
    const int64_t fade = std::max<int64_t> (1, std::min<int64_t> (static_cast<int64_t> (0.002 * sampleRate), len / kDropoutGrid / 4));
    bool dropped[kDropoutGrid];
    for (auto& d : dropped)
        d = random01() < params.contentDropout;

    // Neighbouring dropped chunks form one dropout, faded only at its ends.
    for (int k = 0; k < kDropoutGrid;)
    {
        if (! dropped[k])
        {
            ++k;
            continue;
        }
        int last = k;
        while (last + 1 < kDropoutGrid && dropped[last + 1])
            ++last;
        const int64_t rs = s.begin + len * k / kDropoutGrid;
        const int64_t re = s.begin + len * (last + 1) / kDropoutGrid;
        const bool fadeIn = k > 0, fadeOut = last + 1 < kDropoutGrid; // no fade needed at the trace's own edges
        k = last + 1;
        for (int c = 0; c < store->numChannels(); ++c)
            for (int64_t t = rs; t < re; ++t)
            {
                const int64_t dist = std::min (fadeIn ? t - rs : fade, fadeOut ? re - 1 - t : fade);
                const float g = dist >= fade ? 0.0f : 1.0f - static_cast<float> (dist + 1) / static_cast<float> (fade);
                s.audio[c][t] *= g;
            }
    }
}

void EchoEngine::applyDecay() noexcept
{
    const float fade = std::pow (10.0f, -params.decayFadeDb / 20.0f);
    for (int i = store->size() - 1; i >= 0; --i)
    {
        auto& s = store->slot (store->storedSlot (i));
        s.useCount *= kUseDecay;
        if (s.clamped && params.clampProtects)
            continue;
        s.strength *= fade;
        if (params.decayForget > 0.0f)
        {
            for (auto& f : s.features)
                if (f != 0.0f && random01() < params.decayForget)
                    f = 0.0f;
            for (auto& f : s.context)
                if (f != 0.0f && random01() < params.decayForget)
                    f = 0.0f;
            ++s.featureVersion;
        }
        if (s.strength < kDeadStrength)
            store->removeAt (i); // faded out completely: forgotten
    }
}

WriteOutcome EchoEngine::writeTrace (int spareIdx, const FeatureExtractor& fx, const FeatureVector& feats,
                                     const FeatureVector& context, float rms, int64_t recorded, int generation, bool forced,
                                     int& outSlot) noexcept
{
    outSlot = -1;
    if (params.freeze && ! forced)
        return WriteOutcome::Frozen;
    if (static_cast<double> (recorded) < kMinCoverage * segNominal)
        return WriteOutcome::None;

    if (! forced)
    {
        if (params.writeMode == WriteMode::Manual)
            return WriteOutcome::Gated;
        if (params.writeGateDb > kGateOffDb && toDb (rms) < params.writeGateDb)
            return WriteOutcome::Gated;
        if (params.noveltyMode != NoveltyMode::Off && store->size() > 0)
        {
            const bool familiar = bestMatch (feats, *store, params.similarity, false).similarity >= params.noveltyThreshold;
            if ((params.noveltyMode == NoveltyMode::StoreNovel) == familiar)
                return WriteOutcome::Gated;
        }
        if (params.writeProbability < 1.0f && random01() >= params.writeProbability)
            return WriteOutcome::Gated;
    }

    auto& spare = store->spareSlot (spareIdx);
    spare.begin = std::min (segBegin, spare.maxLen);
    spare.end = std::min (writeIdx, spare.maxLen);
    spare.frameBegin = fx.sinkBegin();
    spare.frameEnd = fx.sinkEnd();
    spare.nominalLen = segNominal;
    spare.rms = rms;
    spare.generation = generation;
    spare.strength = 1.0f;
    spare.useCount = 0.0f;
    spare.mergeCount = 1;
    spare.clamped = false;
    // Encoding failure: the stored copy loses features; the cue (what was
    // actually heard) stays intact.
    spare.features = feats;
    spare.context = context; // the previous segment's address (Stage 9)
    if (params.encodingFailure > 0.0f)
    {
        for (auto& f : spare.features)
            if (random01() < params.encodingFailure)
                f = 0.0f;
        for (auto& f : spare.context)
            if (f != 0.0f && random01() < params.encodingFailure)
                f = 0.0f;
    }
    if (params.contentDropout > 0.0f)
        applyDropouts (spare);

    // Consolidation: merge into a very similar trace instead of adding one.
    const bool mergeWhenFull = store->full() && params.fullPolicy == FullPolicy::MergeSimilar;
    if ((params.mergeThreshold < 1.0f || mergeWhenFull) && store->size() > 0)
    {
        const auto best = bestMatch (feats, *store, params.similarity, true);
        if (best.position >= 0 && (mergeWhenFull || best.similarity >= params.mergeThreshold))
        {
            const int target = store->storedSlot (best.position);
            mergeInto (store->slot (target), spare);
            ++mergeCount;
            ++segmentCount;
            outSlot = target;
            return WriteOutcome::Merged;
        }
    }

    if (store->full())
    {
        const int victim = chooseVictim();
        if (victim < 0)
        {
            ++rejectionCount;
            return WriteOutcome::Rejected; // Reject policy, or everything is clamped
        }
        store->removeAt (victim);
        ++evictionCount;
    }

    const int slot = store->commitSpare (spareIdx);
    if (slot < 0)
    {
        ++rejectionCount;
        return WriteOutcome::Rejected;
    }
    if (params.clampIncoming && store->clampedCount() < maxClamped())
        store->slot (slot).clamped = true;
    ++segmentCount;
    outSlot = slot;
    return WriteOutcome::Stored;
}

// ---- segment boundary --------------------------------------------------------------------

void EchoEngine::boundary (int64_t newBegin, double newNominal) noexcept
{
    const int64_t recorded = writeIdx - segBegin;
    stepLingering();

    if (recorded > 0)
    {
        const bool held = holdRequests.load (std::memory_order_acquire) > 0;
        const FeatureSettings fs { params.featureMode, params.ternaryThreshold };
        features.finalize (probe, fs);
        const float cueRms = static_cast<float> (std::sqrt (segEnergy / static_cast<double> (recorded)));

        int selfSlot = -1;
        WriteOutcome outcome = WriteOutcome::None;
        if (! held)
        {
            beginMutation();
            if (! params.freeze)
                applyDecay(); // one segment of time has passed

            const bool forced = captureArmed;
            outcome = writeTrace (TraceStore::kInputSpare, features, probe, prevHeard, cueRms, recorded,
                                  recordEchoOnly ? segEchoGeneration : 0, forced, selfSlot);
            if (forced && outcome != WriteOutcome::None)
                captureArmed = false;

            if (recordEchoToo && echoSegEnergy > 0.0)
            {
                echoFeatures.finalize (echoProbe, fs);
                const float echoRms = static_cast<float> (std::sqrt (echoSegEnergy / static_cast<double> (recorded)));
                int echoSlot = -1;
                writeTrace (TraceStore::kEchoSpare, echoFeatures, echoProbe, prevEchoHeard, echoRms, recorded,
                            segEchoGeneration, false, echoSlot);
            }
        }
        lastWrite = outcome;

        // In Rolling mode the echo is driven by the search, not by bar lines.
        if (params.cueMode != CueMode::Rolling)
        {
            // The cue: this segment's input (or sidechain), a random address or a
            // frozen one. Cue gate: a (near-)silent live cue evokes nothing, even
            // though its level-independent features could still match loud memories.
            float liveCueRms = cueRms;
            const bool live = makeCue (cue, liveCueRms, recorded);
            bool gated = live && params.cueGateDb > kCueGateOffDb && toDb (liveCueRms) < params.cueGateDb;
            auto rs = retrievalSettings();
            rs.excludeSlot = params.selfMatch ? -1 : selfSlot;

            // Echo Chain (Stage 9): the last echo's content cues memory, blended
            // with the live input by Chain Input; without an echo it restarts
            // from the input.
            const bool chaining = params.cueSource == CueSource::EchoChain && chainValid;
            if (chaining)
            {
                const float w = std::clamp (params.chainInput, 0.0f, 1.0f);
                for (size_t j = 0; j < cue.size(); ++j)
                    cue[j] = (1.0f - w) * chainState[j] + (gated ? 0.0f : w * cue[j]);
                gated = false;
                if (w <= 0.0f)
                    liveCueRms = -1.0f; // no level to track: the chain runs on its own
            }
            addCueNoise (cue);
            applyContextCue (rs, cue, prevCue);
            prevCue = cue;
            const auto result = gated ? RetrievalResult {} : retrieve (contextProbeCurrent, *store, rs, weights.data());
            if (! held)
                noteUse (result);
            installNominal = newNominal;
            installMain (result, liveCueRms, newBegin, fadeSamples, newBegin > 0 ? fadeSamples : 0);
            updateOtherHeads (mainTracking, newBegin);

            if (! held)
                updateHabituation();

            // The chain's next step: what memory expects next, either the
            // blend of every answering trace or one of them, drawn by
            // activation (a random walk through memory).
            chainValid = params.cueSource == CueSource::EchoChain && headWeightCount[0] > 0;
            if (chainValid && params.chainStep == ChainStep::Sample)
            {
                const int pick = sampleChainTrace();
                chainValid = pick >= 0;
                if (chainValid)
                    chainState = store->slot (pick).features;
            }
            else if (chainValid)
                echoAddress (headWeights[0].data(), headWeightCount[0], *store, params.featureMode,
                             params.ternaryThreshold, chainState);
        }
        // Context for the next segment's trace, and what the view shows as [n-1 | n].
        heardBefore = prevHeard;
        prevHeard = probe;
        if (recordEchoToo)
            prevEchoHeard = echoProbe;
        if (! held)
            endMutation();
    }
    else if (params.cueMode != CueMode::Rolling)
    {
        current.pos = newBegin;
        current.rampPos = 0;
        current.rampLen = newBegin > 0 ? fadeSamples : 0;
    }

    segBegin = writeIdx = newBegin;
    segEnergy = echoSegEnergy = 0.0;
    segNominal = newNominal;
    auto& spare = store->spareSlot (TraceStore::kInputSpare);
    auto& echoSpare = store->spareSlot (TraceStore::kEchoSpare);
    features.beginSegment (segNominal, spare.frames, spare.maxFrames);
    scFeatures.beginSegment (segNominal);
    cueSegEnergy = 0.0;
    echoFeatures.beginSegment (segNominal, echoSpare.frames, echoSpare.maxFrames);
    nextSlot = static_cast<int> (static_cast<double> (newBegin) * kSlots / std::max (1.0, segNominal)) + 1;

    // What does the new segment record? (Echo re-encoding, plan §5.5.)
    int maxGen = 0;
    for (int i = 0; i < current.count; ++i)
        maxGen = std::max (maxGen, store->slot (current.entries[static_cast<size_t> (i)].slot).generation);
    segEchoGeneration = maxGen + 1;
    recordEchoOnly = params.recordSource == RecordSource::Echo && current.count > 0;
    recordEchoToo = params.recordSource == RecordSource::InputAndEcho && current.count > 0;

    publishStats();
}

void EchoEngine::addCueNoise (FeatureVector& cueVec) noexcept
{
    // Gaussian noise (in feature units, i.e. standard deviations) on the
    // features the cue has; unencoded (0) features stay unencoded.
    if (params.cueNoise <= 0.0f)
        return;
    for (size_t j = 0; j < cueVec.size(); j += 2)
    {
        const float u1 = std::max (1.0e-7f, random01()), u2 = random01();
        const float r = params.cueNoise * std::sqrt (-2.0f * std::log (u1));
        const float z0 = r * std::cos (6.2831853f * u2), z1 = r * std::sin (6.2831853f * u2);
        if (cueVec[j] != 0.0f)
            cueVec[j] += z0;
        if (j + 1 < cueVec.size() && cueVec[j + 1] != 0.0f)
            cueVec[j + 1] += z1;
    }
}

void EchoEngine::updateHabituation() noexcept
{
    // Traces that just answered tire, and recover by half each segment.
    if (params.habituation <= 0.0f)
        return;
    for (int i = 0; i < store->size(); ++i)
        store->slot (store->storedSlot (i)).fatigue *= 0.5f;
    float maxW = 0.0f;
    for (int i = 0; i < current.count; ++i)
        maxW = std::max (maxW, std::abs (current.entries[static_cast<size_t> (i)].weight));
    if (maxW <= 0.0f)
        return;
    for (int i = 0; i < current.count; ++i)
    {
        const auto& e = current.entries[static_cast<size_t> (i)];
        if (store->positionOf (e.slot) < 0)
            continue;
        auto& f = store->slot (e.slot).fatigue;
        f = std::min (0.95f, f + params.habituation * std::abs (e.weight) / maxW);
    }
}

int EchoEngine::sampleChainTrace() noexcept
{
    // Sample playback: the trace being played is the one that steers.
    if (params.playback == Playback::Sample && current.count > 0)
        return current.entries[0].slot;
    // Otherwise draw one of the answering traces, in proportion to activation.
    double total = 0.0;
    for (int i = 0; i < headWeightCount[0]; ++i)
        total += std::abs (headWeights[0][static_cast<size_t> (i)].activation);
    if (total <= 0.0)
        return -1;
    double r = random01() * total;
    for (int i = 0; i < headWeightCount[0]; ++i)
        if ((r -= std::abs (headWeights[0][static_cast<size_t> (i)].activation)) <= 0.0)
            return headWeights[0][static_cast<size_t> (i)].slot;
    return headWeights[0][static_cast<size_t> (headWeightCount[0] - 1)].slot;
}

void EchoEngine::applyContextCue (RetrievalSettings& rs, const FeatureVector& now, const FeatureVector& before) noexcept
{
    // Sequential context (Stage 9): which halves of the traces' [n-1 | n]
    // addresses the cue is compared with.
    contextProbeCurrent = now;
    rs.contextProbe = nullptr;
    rs.contextWeight = 0.0f;
    rs.currentWeight = 1.0f;
    const bool chain = params.cueSource == CueSource::EchoChain;
    if (! params.sequenceContext)
        return; // MINERVA II: the current half only (a chain then free-associates)
    const ContextCue mode = chain ? ContextCue::PredictNext : params.contextCue;
    switch (mode)
    {
        case ContextCue::MatchBoth:
            // [previous cue | cue] against [n-1 | n]: recall that depends on what came before.
            contextProbeContext = before;
            rs.contextProbe = &contextProbeContext;
            rs.contextWeight = std::clamp (params.contextWeight, 0.0f, 1.0f);
            break;
        case ContextCue::PredictNext:
            // [cue | blank]: traces whose n-1 matches the cue; their audio is what came next.
            contextProbeContext = now;
            rs.contextProbe = &contextProbeContext;
            rs.contextWeight = 1.0f;
            rs.currentWeight = 0.0f;
            break;
        case ContextCue::CurrentOnly:
            break;
    }
}

RetrievalSettings EchoEngine::retrievalSettings() const noexcept
{
    RetrievalSettings rs;
    rs.similarity = params.similarity;
    rs.power = params.power;
    rs.negativeMode = params.negativeMode;
    rs.normalization = params.normalization;
    rs.focus = params.featureFocus;
    rs.recency = params.recency;
    rs.habituation = params.habituation > 0.0f;
    return rs;
}

void EchoEngine::noteUse (const RetrievalResult& result) noexcept
{
    for (int i = 0; i < result.numWeights; ++i)
        store->slot (weights[static_cast<size_t> (i)].slot).useCount += std::abs (weights[static_cast<size_t> (i)].activation);
}

float EchoEngine::trackingFor (const EchoWeight* w, int n, float cueRms) const noexcept
{
    // Level tracking: scale the echo so its level follows the cue's, the way
    // a tape repeat follows what was played. Memory picks *what* returns;
    // the input decides *how loud*. (Capacity 1: the factor is exactly 1.)
    // A negative cueRms means the cue has no level (random / frozen cues).
    if (params.levelTracking <= 0.0f || n == 0 || cueRms < 0.0f)
        return 1.0f;
    // Activation-weighted mean level of what was retrieved. Using the mean
    // (not the weighted sum) leaves the normalisation mode's own loudness
    // behaviour (e.g. Familiarity) intact.
    double weighted = 0.0, total = 0.0;
    for (int i = 0; i < n; ++i)
    {
        weighted += std::abs (w[i].weight) * store->slot (w[i].slot).rms;
        total += std::abs (w[i].weight);
    }
    const double memoryRms = total > 0.0 ? weighted / total : 0.0;
    const float ratio = memoryRms > 0.0 ? std::min (kMaxTrackingGain, static_cast<float> (cueRms / memoryRms)) : 0.0f;
    return std::pow (ratio, params.levelTracking);
}

void EchoEngine::effectiveHeads (HeadMode modes[kNumHeads], float gains[kNumHeads]) const noexcept
{
    bool head1 = true;
    HeadMode m2 = params.headMode[1], m3 = params.headMode[2];
    switch (params.modeSelector)
    {
        case ModeSelector::Custom:       break;
        case ModeSelector::H1:           m2 = m3 = HeadMode::Off; break;
        case ModeSelector::H2:           head1 = false; m2 = HeadMode::Delay; m3 = HeadMode::Off; break;
        case ModeSelector::H3:           head1 = false; m2 = HeadMode::Off; m3 = HeadMode::Delay; break;
        case ModeSelector::H2H3:         head1 = false; m2 = m3 = HeadMode::Delay; break;
        case ModeSelector::H1H2:         m2 = HeadMode::Delay; m3 = HeadMode::Off; break;
        case ModeSelector::H1H3:         m2 = HeadMode::Off; m3 = HeadMode::Delay; break;
        case ModeSelector::H1H2H3:       m2 = m3 = HeadMode::Delay; break;
        case ModeSelector::Iterative123: m2 = m3 = HeadMode::Iterative; break;
    }
    modes[0] = HeadMode::Delay;
    modes[1] = m2;
    modes[2] = m3;
    gains[0] = head1 ? levelToGain (params.headLevelDb[0]) : 0.0f;
    for (int h = 1; h < kNumHeads; ++h)
        gains[h] = modes[h] == HeadMode::Off ? 0.0f : levelToGain (params.headLevelDb[h]);
}

int EchoEngine::shapePlayback (EchoWeight* w, int n) noexcept
{
    if (n <= 1 || (params.playback == Playback::Blend && n <= params.maxActive))
        return n;

    double total = 0.0;
    for (int i = 0; i < n; ++i)
        total += std::abs (w[i].weight);

    if (params.playback == Playback::Sample)
    {
        // One trace, chosen with probability proportional to its activation.
        double sum = 0.0;
        for (int i = 0; i < n; ++i)
            sum += std::abs (w[i].activation);
        double r = random01() * sum;
        int pick = n - 1;
        for (int i = 0; i < n; ++i)
            if ((r -= std::abs (w[i].activation)) <= 0.0)
            {
                pick = i;
                break;
            }
        w[0] = w[pick];
        w[0].weight = static_cast<float> (std::copysign (total, w[0].weight));
        return 1;
    }

    // Voices (or a blend capped at Max Active Traces): the k strongest traces,
    // strongest first, at the same total level.
    const int k = std::min (params.playback == Playback::Voices ? params.voices : params.maxActive, n);
    for (int i = 0; i < k; ++i)
    {
        int best = i;
        for (int j = i + 1; j < n; ++j)
            if (std::abs (w[j].activation) > std::abs (w[best].activation))
                best = j;
        std::swap (w[i], w[best]);
    }
    double kept = 0.0;
    for (int i = 0; i < k; ++i)
        kept += std::abs (w[i].weight);
    const float scale = kept > 0.0 ? static_cast<float> (total / kept) : 0.0f;
    for (int i = 0; i < k; ++i)
        w[i].weight *= scale;
    return k;
}

void EchoEngine::installEcho (int head, const EchoWeight* src, int n, float tracking, int64_t pos,
                              int64_t fadeOutLen, int64_t fadeInLen) noexcept
{
    auto& cur = headCurrent[static_cast<size_t> (head)];
    auto& prev = headPrevious[static_cast<size_t> (head)];

    // The current echo keeps playing briefly while fading out.
    std::swap (prev.entries, cur.entries);
    prev.count = cur.count;
    prev.pos = cur.pos;
    prev.rampPos = 0;
    prev.rampLen = fadeOutLen;
    prev.fadingOut = true;
    if (fadeOutLen == 0)
        prev.count = 0;

    const auto lookahead = static_cast<int64_t> (std::llround (params.lookaheadMs * 0.001 * sampleRate));
    const uint64_t now = store->currentSerial();
    const bool voices = params.playback == Playback::Voices && n > 1;
    const float headPan = params.headPan[head];

    cur.count = n;
    for (int i = 0; i < n; ++i)
    {
        const auto& w = src[i];
        const auto& s = store->slot (w.slot);
        float coeff = 1.0f;
        if (params.wearTone > 0.0f)
        {
            // Older traces play back duller, like worn tape.
            const double age = static_cast<double> (now - std::min (now, s.serial));
            const double fc = std::clamp (18000.0 * std::exp (-params.wearTone * age / 8.0), 200.0, 0.45 * sampleRate);
            coeff = static_cast<float> (1.0 - std::exp (-2.0 * 3.14159265358979 * fc / sampleRate));
        }

        // Chorus voices: the strongest sits in the centre; the others fan out
        // alternately left and right, each more detuned and delayed.
        float voicePos = 0.0f;
        if (voices)
        {
            const int rank = (i + 1) / 2;
            const int maxRank = n / 2;
            voicePos = static_cast<float> (rank) / static_cast<float> (std::max (1, maxRank)) * (i % 2 == 1 ? -1.0f : 1.0f);
        }
        const float pan = std::clamp (headPan + voicePos * params.voiceSpread, -1.0f, 1.0f);
        const float cents = voicePos * params.voiceDetuneCents;
        const auto voiceDelay = static_cast<int64_t> (std::llround (std::abs (voicePos) * params.voiceDelayMs * 0.001 * sampleRate));
        const float g = w.weight * tracking;

        auto& e = cur.entries[static_cast<size_t> (i)];
        e.audio[0] = s.audio[0];
        e.audio[1] = s.audio[1];
        e.begin = s.begin;
        e.end = s.end;
        e.offset = w.offset + lookahead - voiceDelay;
        e.fade = std::min (fadeSamples, (s.end - s.begin) / 4); // fixed while it plays
        e.weight = g;
        e.slot = w.slot;
        e.toneCoeff = coeff;
        e.z[0] = e.z[1] = 0.0f;
        e.gain[0] = g * std::min (1.0f, 1.0f - pan);
        e.gain[1] = g * std::min (1.0f, 1.0f + pan);
        // Length mismatch: a trace recorded at another trace length either
        // changes speed to fit (and pitch, like tape), is cut, or loops.
        double fit = 1.0;
        e.loopLen = 0;
        if (installNominal > 0.0 && s.nominalLen > 0.0)
        {
            const double ratio = s.nominalLen / installNominal;
            const bool fitLength = params.lengthMismatch == LengthMismatch::Varispeed
                                   || params.lengthMismatch == LengthMismatch::Stretch; // stretch: spectral, pitch kept
            if (fitLength && std::abs (ratio - 1.0) > 1.0e-9)
                fit = ratio;
            else if (params.lengthMismatch == LengthMismatch::Loop && static_cast<double> (s.end - s.begin) < installNominal)
                e.loopLen = s.end - s.begin;
        }
        e.rate = fit * (cents != 0.0f ? std::pow (2.0, cents / 1200.0) : 1.0);
        e.anchor = pos;
        e.anchorTrace = static_cast<double> (pos) * fit + static_cast<double> (e.offset);
    }
    cur.pos = pos;
    cur.rampPos = 0;
    cur.rampLen = fadeInLen;
    cur.fadingOut = false;
}

void EchoEngine::installMain (const RetrievalResult& result, float cueRms, int64_t pos, int64_t fadeOutLen,
                              int64_t fadeInLen) noexcept
{
    const int n = result.numWeights;
    auto& mine = headWeights[0];
    std::copy (weights.begin(), weights.begin() + n, mine.begin());
    headWeightCount[0] = n;
    const float tracking = trackingFor (mine.data(), n, cueRms);
    mainTracking = tracking;

    // Shape a copy so the unshaped echo stays available to the other heads.
    std::copy (mine.begin(), mine.begin() + n, weights.begin());
    const int shaped = shapePlayback (weights.data(), n);
    installEcho (0, weights.data(), shaped, tracking, pos, fadeOutLen, fadeInLen);

    ++cueUpdates;
    familiarityTarget = std::clamp (result.maxAbs, 0.0f, 1.0f);
    stats.intensity.store (result.intensity, std::memory_order_relaxed);
    stats.maxActivation.store (result.maxAbs, std::memory_order_relaxed);
    stats.activeTraces.store (shaped, std::memory_order_relaxed);
}

void EchoEngine::updateOtherHeads (float tracking, int64_t pos) noexcept
{
    HeadMode modes[kNumHeads];
    float gains[kNumHeads];
    effectiveHeads (modes, gains);
    const int64_t fadeIn = pos > 0 ? fadeSamples : 0;

    for (int h = 1; h < kNumHeads; ++h)
    {
        auto& out = headWeights[static_cast<size_t> (h)];
        int n = 0;
        float headTracking = tracking;
        if (modes[h] == HeadMode::Delay)
        {
            // Tape head h+1: what head 1 played h segments ago (still in memory).
            const auto& past = history[static_cast<size_t> (h - 1)];
            for (int i = 0; i < past.count; ++i)
            {
                const auto& w = past.weights[static_cast<size_t> (i)];
                if (store->slot (w.slot).serial == past.serials[static_cast<size_t> (i)])
                    out[static_cast<size_t> (n++)] = w;
            }
            headTracking = past.tracking;
        }
        else if (modes[h] == HeadMode::Iterative)
        {
            // Echo of the echo: cue memory with the previous head's echo content.
            const int from = headWeightCount[static_cast<size_t> (h - 1)] > 0 ? h - 1 : 0;
            echoAddress (headWeights[static_cast<size_t> (from)].data(), headWeightCount[static_cast<size_t> (from)],
                         *store, params.featureMode, params.ternaryThreshold, iterCue);
            // With sequence context the same rule applies: Predict Next plays
            // what follows the previous head's echo, and so on down the heads.
            addCueNoise (iterCue);
            FeatureVector iterBefore {};
            if (params.sequenceContext && params.contextCue == ContextCue::MatchBoth)
                echoAddress (headWeights[static_cast<size_t> (from)].data(), headWeightCount[static_cast<size_t> (from)],
                             *store, params.featureMode, params.ternaryThreshold, iterBefore, true);
            auto rs = retrievalSettings();
            applyContextCue (rs, iterCue, iterBefore);
            n = retrieve (contextProbeCurrent, *store, rs, out.data()).numWeights;
        }
        headWeightCount[static_cast<size_t> (h)] = n;

        std::copy (out.begin(), out.begin() + n, weights.begin());
        const int shaped = shapePlayback (weights.data(), n);
        installEcho (h, weights.data(), shaped, headTracking, pos, fadeSamples, fadeIn);
    }

    // Remember head 1's echo for the delay heads.
    std::swap (history[1], history[0]);
    auto& now = history[0];
    now.count = headWeightCount[0];
    for (int i = 0; i < now.count; ++i)
    {
        now.weights[static_cast<size_t> (i)] = headWeights[0][static_cast<size_t> (i)];
        now.serials[static_cast<size_t> (i)] = store->slot (headWeights[0][static_cast<size_t> (i)].slot).serial;
    }
    now.tracking = tracking;
}

bool EchoEngine::makeCue (FeatureVector& out, float& cueRms, int64_t recorded) noexcept
{
    switch (params.cueSource)
    {
        case CueSource::Random:
            // Dreaming: a random address.
            for (auto& f : out)
            {
                const float r = random01() * 2.0f - 1.0f;
                f = params.featureMode == FeatureMode::Ternary ? (r > 0.33f ? 1.0f : (r < -0.33f ? -1.0f : 0.0f))
                                                               : r * 1.7f;
            }
            cueRms = -1.0f;
            return false;
        case CueSource::Frozen:
            out = frozenCue;
            cueRms = frozenCueRms;
            return false;
        case CueSource::Sidechain:
        case CueSource::Input:
        case CueSource::EchoChain: // the live input seeds and steers the chain
            break;
    }
    if (sidechainActive)
        scFeatures.finalize (out, { params.featureMode, params.ternaryThreshold });
    else
        out = probe; // the input's own address, computed at the boundary
    cueRms = static_cast<float> (std::sqrt (cueSegEnergy / static_cast<double> (std::max<int64_t> (1, recorded))));
    frozenCue = out;
    frozenCueRms = cueRms;
    return true;
}

// ---- live cueing (plan §3 Modes B and C) --------------------------------------------------

int64_t EchoEngine::slotEdge (int k) const noexcept
{
    return static_cast<int64_t> (std::ceil (static_cast<double> (k) * segNominal / kSlots));
}

void EchoEngine::progressiveUpdate (int k) noexcept
{
    const int64_t recorded = writeIdx - segBegin;
    if (recorded <= 0 || k < params.progressiveStart)
        return;
    if (params.cueSource == CueSource::Random || params.cueSource == CueSource::Frozen
        || params.cueSource == CueSource::EchoChain)
        return; // only a live cue unfolds within the bar (the chain steps at bar lines)

    // The bar so far, compared with the same stretch of every stored trace.
    cueFx().finalize (partialProbe, { params.featureMode, params.ternaryThreshold });
    addCueNoise (partialProbe);
    const float cueRms = static_cast<float> (std::sqrt (cueSegEnergy / static_cast<double> (recorded)));
    const auto smooth = static_cast<int64_t> (std::llround (params.cueSmoothingMs * 0.001 * sampleRate));
    if (params.cueGateDb > kCueGateOffDb && toDb (cueRms) < params.cueGateDb)
    {
        installMain ({}, cueRms, writeIdx, smooth, smooth);
        return;
    }

    const int firstSlot = static_cast<int> (static_cast<double> (segBegin) * kSlots / std::max (1.0, segNominal));
    const bool renormalize = params.featureMode == FeatureMode::Continuous;
    int n = 0;
    for (int i = 0; i < store->size(); ++i)
    {
        const int slot = store->storedSlot (i);
        const auto& t = store->slot (slot);
        float sim;
        if (! params.sequenceContext || params.contextCue == ContextCue::CurrentOnly)
            sim = focusedSimilarity (partialProbe, t.features, params.similarity, params.featureFocus, firstSlot, k,
                                     renormalize);
        else if (params.contextCue == ContextCue::PredictNext)
            // The bar so far against the same stretch of each trace's n-1 half.
            sim = focusedSimilarity (partialProbe, t.context, params.similarity, params.featureFocus, firstSlot, k,
                                     renormalize);
        else
            // Match Both: the whole previous bar against n-1, the bar so far against n.
            sim = combineHalves (params.featureFocus == FeatureFocus::Full
                                     ? similarity (prevHeard, t.context, params.similarity)
                                     : focusedSimilarity (prevHeard, t.context, params.similarity, params.featureFocus,
                                                          0, kSlots, false),
                                 params.contextWeight,
                                 focusedSimilarity (partialProbe, t.features, params.similarity, params.featureFocus,
                                                    firstSlot, k, renormalize),
                                 1.0f);
        weights[static_cast<size_t> (n++)] = { slot, sim, 0.0f, 0 };
    }
    const auto result = finishRetrieval (*store, retrievalSettings(), weights.data(), n);
    if (holdRequests.load (std::memory_order_acquire) == 0)
    {
        beginMutation();
        noteUse (result);
        endMutation();
    }

    // Memories play in step with the bar being played (position writeIdx).
    installNominal = segNominal;
    installMain (result, cueRms, writeIdx, smooth, smooth);
}

int64_t EchoEngine::refineOffset (const TraceSlot& trace, int64_t offset, int64_t probeEnd, int windowSamples) const noexcept
{
    // Live window in envelope points: [a, b) covers stream [a*E, b*E).
    const int64_t E = envStep;
    const int64_t a = (probeEnd - windowSamples + E - 1) / E;
    const int64_t b = probeEnd / E;
    const int n = static_cast<int> (b - a);
    if (n < 8 || b > envProduced || envProduced - a > kEnvRing)
        return offset;

    float live[2048];
    const int count = std::min (n, 2048);
    double ls = 0.0, lsq = 0.0;
    for (int k = 0; k < count; ++k)
    {
        live[k] = envRing[static_cast<size_t> ((a + k) % kEnvRing)];
        ls += live[k];
        lsq += static_cast<double> (live[k]) * live[k];
    }
    const double lvar = lsq - ls * ls / count;
    if (lvar <= 1.0e-12)
        return offset;

    // The trace sample that lines up with stream sample a*E, as first estimated.
    const int64_t x0 = offset - (probeEnd - a * E);
    const int64_t hop = frameHop (sampleRate);
    const int64_t sub = std::max<int64_t> (1, E / 2); // search step: half an envelope point
    const int lags = static_cast<int> (2 * hop / sub) + 1;
    const int64_t base = x0 - (lags / 2) * sub;

    // Trace energy in half-point steps, computed once; each lag then pairs
    // sub-points (j + 2k, j + 2k + 1) into one envelope point.
    const int subPoints = std::min (lags + 2 * count + 1, static_cast<int> (refineScratch.size()));
    const int ch = store->numChannels();
    for (int m = 0; m < subPoints; ++m)
    {
        double acc = 0.0;
        const int64_t t0 = std::max (base + m * sub, trace.begin);
        const int64_t t1 = std::min (base + (m + 1) * sub, trace.end);
        for (int64_t t = t0; t < t1; ++t)
        {
            float mono = 0.0f;
            for (int c = 0; c < ch; ++c)
                mono += trace.audio[c][t];
            mono /= static_cast<float> (ch);
            acc += static_cast<double> (mono) * mono;
        }
        refineScratch[static_cast<size_t> (m)] = static_cast<float> (acc);
    }

    int64_t bestX = x0;
    double bestR = -2.0;
    for (int j = 0; j < lags && j + 2 * count < subPoints; ++j)
    {
        double ts = 0.0, tsq = 0.0, cross = 0.0;
        for (int k = 0; k < count; ++k)
        {
            const double v = std::sqrt ((refineScratch[static_cast<size_t> (j + 2 * k)]
                                         + refineScratch[static_cast<size_t> (j + 2 * k + 1)]) / static_cast<double> (2 * sub));
            ts += v;
            tsq += v * v;
            cross += v * live[k];
        }
        const double tvar = tsq - ts * ts / count;
        if (tvar <= 1.0e-12)
            continue;
        const double r = (cross - ls * ts / count) / std::sqrt (lvar * tvar);
        if (r > bestR)
        {
            bestR = r;
            bestX = base + j * sub;
        }
    }
    return bestX + (probeEnd - a * E);
}

void EchoEngine::startRollingSearch() noexcept
{
    const int W = std::clamp (static_cast<int> (std::lround (params.rollingWindowMs * 0.001 / kFrameSeconds)), 3,
                              kMaxWindowFrames);
    auto& fx = cueFx();
    const int64_t produced = fx.framesProduced();
    if (produced < W || W > kFrameRing)
        return;

    const float* frames[kMaxWindowFrames];
    double power = 0.0;
    for (int f = 0; f < W; ++f)
    {
        const int64_t idx = produced - W + f;
        frames[f] = fx.ringFrame (idx);
        power += fx.ringEnergy (idx);
    }
    lastSearchStart = fx.streamPosition();
    rolling.cueRms = static_cast<float> (std::sqrt (power / W));
    rolling.probeEnd = fx.ringEnd (produced - 1);

    const auto smooth = static_cast<int64_t> (std::llround (params.cueSmoothingMs * 0.001 * sampleRate));
    if ((params.cueGateDb > kCueGateOffDb && toDb (rolling.cueRms) < params.cueGateDb)
        || ! prepareWindowProbe (frames, W, rolling.probe))
    {
        if (current.count > 0)
            installMain ({}, rolling.cueRms, 0, smooth, smooth); // silence: let the echo fade
        return;
    }
    rolling.active = true;
    rolling.position = 0;
    rolling.count = 0;
}

void EchoEngine::continueRollingSearch (int64_t budget) noexcept
{
    const int W = rolling.probe.frames;
    const int hop = cueFx().hop();
    const int newest = store->size() - 1;
    while (rolling.position < store->size() && budget > 0)
    {
        const int pos = rolling.position++;
        if (! params.selfMatch && pos == newest)
            continue; // the trace just stored: matching it would only echo what was just played
        const int slot = store->storedSlot (pos);
        const auto& trace = store->slot (slot);
        budget -= static_cast<int64_t> (std::max (0, trace.frameEnd - trace.frameBegin)) * W * kBands;
        const auto m = bestOffset (rolling.probe, trace, 1);
        if (m.offset < 0)
            continue;
        // Offset = the trace sample that lines up with the end of the probe.
        weights[static_cast<size_t> (rolling.count)] = { slot, m.similarity, 0.0f,
                                                         static_cast<int64_t> (m.offset + W) * hop };
        rollingSerials[static_cast<size_t> (rolling.count)] = trace.serial;
        ++rolling.count;
    }
    if (rolling.position < store->size())
        return; // continue next block

    // Search complete. Drop results whose trace changed meanwhile, then align
    // every match with "now": the probe ended (now - probeEnd) samples ago.
    const int64_t elapsed = cueFx().streamPosition() - rolling.probeEnd;
    int n = 0;
    for (int i = 0; i < rolling.count; ++i)
    {
        auto w = weights[static_cast<size_t> (i)];
        if (store->positionOf (w.slot) < 0 || store->slot (w.slot).serial != rollingSerials[static_cast<size_t> (i)])
            continue;
        w.offset += elapsed;
        weights[static_cast<size_t> (n++)] = w;
    }
    auto result = finishRetrieval (*store, retrievalSettings(), weights.data(), n);

    // Frame matches are only accurate to half a frame (~10 ms); refine the
    // strongest ones against a 1 ms loudness envelope so echoes land in time.
    const float strongest = result.maxAbs;
    int refined = 0;
    for (int i = 0; i < result.numWeights && refined < kMaxRefinements; ++i)
    {
        auto& w = weights[static_cast<size_t> (i)];
        if (std::abs (w.activation) < 0.1f * strongest)
            continue;
        const int64_t aligned = w.offset - elapsed; // trace sample aligned with the probe's end
        w.offset = refineOffset (store->slot (w.slot), aligned, rolling.probeEnd, rolling.probe.frames * hop) + elapsed;
        ++refined;
    }

    if (holdRequests.load (std::memory_order_acquire) == 0)
    {
        beginMutation();
        noteUse (result);
        endMutation();
    }
    const auto smooth = static_cast<int64_t> (std::llround (params.cueSmoothingMs * 0.001 * sampleRate));
    // installMain copies the result from `weights`; keep the refined offsets there.
    installNominal = 0.0; // continuations play at their own speed
    installMain (result, rolling.cueRms, 0, smooth, smooth);
    stats.cueLatencyMs.store (static_cast<float> (1000.0 * static_cast<double> (elapsed) / sampleRate),
                              std::memory_order_relaxed);
    rolling.active = false;
}

// ---- playback and recording ------------------------------------------------------------------

void EchoEngine::accumulate (Playlist& pl, int head, int ioChannels, int blockOffset, int len) noexcept
{
    if (pl.count > 0)
    {
        const int storeCh = store->numChannels();
        const float hg0 = headGainNow[head] + headGainInc[head] * static_cast<float> (blockOffset);
        const float hgInc = headGainInc[head];
        for (int k = 0; k < pl.count; ++k)
        {
            auto& e = pl.entries[static_cast<size_t> (k)];
            const int64_t fade = e.fade;
            for (int c = 0; c < ioChannels; ++c)
            {
                const float chanGain = ioChannels == 1 ? 0.5f * (e.gain[0] + e.gain[1]) : e.gain[c];
                if (chanGain == 0.0f)
                    continue;
                const float* src = e.audio[std::min (c, storeCh - 1)];
                float* dst = echoBuf[static_cast<size_t> (c)].data();
                float z = e.z[c];
                // At normal speed only the samples inside the trace need visiting.
                const bool exact = e.rate == 1.0 && ! motionActive && e.loopLen == 0;
                int iBegin = 0, iEnd = len;
                if (exact)
                {
                    const int64_t base = pl.pos + e.offset;
                    iBegin = static_cast<int> (std::clamp<int64_t> (e.begin - base, 0, len));
                    iEnd = static_cast<int> (std::clamp<int64_t> (e.end - base, 0, len));
                }
                // Fast path: at normal speed, away from the trace's edges and
                // from any crossfade, a sample is just source x gain.
                int pBegin = iBegin, pEnd = iBegin;
                if (exact && e.toneCoeff >= 1.0f && ! (pl.rampLen > 0 && pl.fadingOut))
                {
                    const int64_t base = pl.pos + e.offset;
                    const int64_t rampDone = pl.rampLen > 0 ? pl.rampLen - pl.rampPos - 1 : 0; // first i with ramp == 1
                    pBegin = static_cast<int> (std::clamp<int64_t> (std::max (e.begin + fade - base, rampDone), iBegin, iEnd));
                    pEnd = static_cast<int> (std::clamp<int64_t> (e.end - fade - base, pBegin, iEnd));
                    const float* __restrict s = src + base;
                    float* __restrict d = dst;
                    const float g0 = chanGain * hg0, gi = chanGain * hgInc;
                    for (int i = pBegin; i < pEnd; ++i)
                        d[i] += s[i] * (g0 + gi * static_cast<float> (i));
                }
                for (int i = iBegin; i < iEnd; ++i)
                {
                    if (i == pBegin && pEnd > pBegin)
                    {
                        i = pEnd - 1; // done above
                        continue;
                    }
                    // Read position in the trace (varispeed for detuned voices).
                    float x;
                    int64_t t;
                    if (exact)
                    {
                        t = pl.pos + e.offset + i;
                        if (t < e.begin || t >= e.end)
                            continue;
                        x = src[t];
                    }
                    else
                    {
                        // Varispeed / detune / wow & flutter / loop: fractional read.
                        double tf = e.anchorTrace + static_cast<double> (pl.pos + i - e.anchor) * e.rate;
                        if (motionActive)
                            tf -= motionBuf[static_cast<size_t> (i)];
                        if (e.loopLen > 0)
                        {
                            double rel = std::fmod (tf - static_cast<double> (e.begin), static_cast<double> (e.loopLen));
                            if (rel < 0.0)
                                rel += static_cast<double> (e.loopLen);
                            tf = static_cast<double> (e.begin) + rel;
                        }
                        t = static_cast<int64_t> (std::floor (tf));
                        if (t < e.begin || t >= e.end)
                            continue;
                        const int64_t t1 = t + 1 < e.end ? t + 1 : (e.loopLen > 0 ? e.begin : t);
                        const auto frac = static_cast<float> (tf - static_cast<double> (t));
                        x = src[t] + frac * (src[t1] - src[t]);
                    }
                    float g = chanGain * (hg0 + hgInc * static_cast<float> (i));
                    if (fade > 0)
                        g *= std::min ({ 1.0f, static_cast<float> (t - e.begin + 1) / fade,
                                         static_cast<float> (e.end - t) / fade });
                    if (pl.rampLen > 0)
                    {
                        const float r = static_cast<float> (pl.rampPos + i + 1) / static_cast<float> (pl.rampLen);
                        g *= pl.fadingOut ? std::max (0.0f, 1.0f - r) : std::min (1.0f, r);
                    }
                    x *= g;
                    if (e.toneCoeff < 1.0f)
                    {
                        z += e.toneCoeff * (x - z);
                        x = z;
                    }
                    dst[i] += x;
                }
                e.z[c] = z;
            }
        }
    }
    pl.pos += len;
    pl.rampPos += len;
    if (pl.fadingOut && pl.rampPos >= pl.rampLen)
        pl.count = 0;
}

void EchoEngine::advance (Playlist& pl, int len) noexcept
{
    pl.pos += len;
    pl.rampPos += len;
    if (pl.fadingOut && pl.rampPos >= pl.rampLen)
        pl.count = 0;
}

bool EchoEngine::spectralHeads() const noexcept
{
    return params.blendDomain == BlendDomain::Spectral || params.lengthMismatch == LengthMismatch::Stretch;
}

void EchoEngine::renderSpectral (int head, int ioChannels, int blockOffset, int len) noexcept
{
    auto& sh = spectral[static_cast<size_t> (head)];
    const int N = sh.renderer.frameSize();
    if (! sh.active)
    {
        // Entering spectral mode: start from a clean overlap-add state.
        for (auto& r : sh.ring)
            std::fill (r.begin(), r.end(), 0.0f);
        sh.ringPos = 0;
        sh.countdown = 0;
        sh.renderer.reset();
        sh.active = true;
    }

    int i = 0;
    while (i < len)
    {
        if (sh.countdown <= 0)
        {
            spectralFrame (head, ioChannels, blockOffset, i);
            sh.countdown = sh.renderer.hop();
        }
        const int n = std::min (len - i, sh.countdown);
        for (int c = 0; c < ioChannels; ++c)
        {
            float* dst = echoBuf[static_cast<size_t> (c)].data() + i;
            auto& ring = sh.ring[static_cast<size_t> (c)];
            for (int j = 0; j < n; ++j)
            {
                auto& slot = ring[static_cast<size_t> ((sh.ringPos + j) % N)];
                dst[j] += slot;
                slot = 0.0f;
            }
        }
        sh.ringPos = (sh.ringPos + n) % N;
        sh.countdown -= n;
        i += n;
    }
    advance (headCurrent[static_cast<size_t> (head)], len);
    advance (headPrevious[static_cast<size_t> (head)], len);
}

void EchoEngine::spectralFrame (int head, int ioChannels, int blockOffset, int at) noexcept
{
    auto& sh = spectral[static_cast<size_t> (head)];
    const int N = sh.renderer.frameSize();
    const int storeCh = store->numChannels();
    const float headGain = headGainNow[head] + headGainInc[head] * static_cast<float> (blockOffset + at);

    // Every playing memory of this head (current echo and the one fading out),
    // with its gain and read position at this frame.
    int n = 0;
    for (auto* pl : { &headCurrent[static_cast<size_t> (head)], &headPrevious[static_cast<size_t> (head)] })
    {
        if (pl->count == 0)
            continue;
        float ramp = 1.0f;
        if (pl->rampLen > 0)
        {
            const float r = static_cast<float> (pl->rampPos + at + 1) / static_cast<float> (pl->rampLen);
            ramp = pl->fadingOut ? std::max (0.0f, 1.0f - r) : std::min (1.0f, r);
        }
        for (int k = 0; k < pl->count; ++k)
        {
            const auto& e = pl->entries[static_cast<size_t> (k)];
            const double start = e.rate == 1.0 ? static_cast<double> (pl->pos + at + e.offset)
                                               : e.anchorTrace + static_cast<double> (pl->pos + at - e.anchor) * e.rate;
            if (start >= static_cast<double> (e.end) || start + N <= static_cast<double> (e.begin))
                continue;
            auto& s = spectralSources[static_cast<size_t> (n++)];
            s.audio[0] = e.audio[0];
            s.audio[1] = e.audio[std::min (1, storeCh - 1)];
            s.begin = e.begin;
            s.end = e.end;
            s.start = start;
            s.rate = e.rate;
            const float g = ramp * headGain;
            s.gain[0] = (ioChannels == 1 ? 0.5f * (e.gain[0] + e.gain[1]) : e.gain[0]) * g;
            s.gain[1] = e.gain[1] * g;
        }
    }

    // Only the strongest few are worth transforming.
    const int keep = std::min (n, params.spectralVoices);
    for (int a = 0; a < keep; ++a)
    {
        int best = a;
        for (int b = a + 1; b < n; ++b)
            if (std::abs (spectralSources[static_cast<size_t> (b)].gain[0]) + std::abs (spectralSources[static_cast<size_t> (b)].gain[1])
                > std::abs (spectralSources[static_cast<size_t> (best)].gain[0]) + std::abs (spectralSources[static_cast<size_t> (best)].gain[1]))
                best = b;
        std::swap (spectralSources[static_cast<size_t> (a)], spectralSources[static_cast<size_t> (best)]);
    }

    float* frames[kMaxChannels] = { spectralFrameBuf[0].data(), spectralFrameBuf[1].data() };
    sh.renderer.render (spectralSources.data(), keep, ioChannels, params.spectralFreeze, frames);
    for (int c = 0; c < ioChannels; ++c)
    {
        auto& ring = sh.ring[static_cast<size_t> (c)];
        const float* f = frames[c];
        for (int j = 0; j < N; ++j)
            ring[static_cast<size_t> ((sh.ringPos + j) % N)] += f[j];
    }
}

void EchoEngine::processChunk (float* const* io, int ioChannels, int offset, int len, const float* const* sc,
                               int scChannels) noexcept
{
    for (int c = 0; c < ioChannels; ++c)
        std::fill_n (echoBuf[static_cast<size_t> (c)].data(), len, 0.0f);
    if (motionActive)
        motion.fill (motionBuf.data(), len, params.wow, params.flutter);
    const bool useSpectral = spectralHeads();
    for (int h = 0; h < kNumHeads; ++h)
    {
        if (useSpectral)
        {
            renderSpectral (h, ioChannels, offset, len);
            continue;
        }
        spectral[static_cast<size_t> (h)].active = false;
        accumulate (headCurrent[static_cast<size_t> (h)], h, ioChannels, offset, len);
        accumulate (headPrevious[static_cast<size_t> (h)], h, ioChannels, offset, len);
    }

    // Echo tone: one-pole low-pass on the echo (its cutoff can follow familiarity).
    if (toneActive)
        for (int c = 0; c < ioChannels; ++c)
        {
            float* e = echoBuf[static_cast<size_t> (c)].data();
            float z = toneZ[c];
            float k = toneCoeff;
            for (int i = 0; i < len; ++i)
            {
                z += k * (e[i] - z);
                e[i] = z;
                k += toneInc;
            }
            toneZ[c] = z;
        }
    if (toneActive)
        toneCoeff += toneInc * static_cast<float> (len);

    // Tape drive (soft saturation, unity gain for small signals) and hiss.
    const bool driving = driveMix > 0.0f || driveMixInc > 0.0f;
    if (driving || hissGain > 0.0f)
        for (int c = 0; c < ioChannels; ++c)
        {
            float* e = echoBuf[static_cast<size_t> (c)].data();
            float k = driveK, mix = driveMix;
            for (int i = 0; i < len; ++i)
            {
                float v = e[i];
                if (driving)
                {
                    v += mix * (std::tanh (k * v) / k - v);
                    k += driveKInc;
                    mix += driveMixInc;
                }
                if (hissGain > 0.0f)
                {
                    hissRng = hissRng * 6364136223846793005ull + 1442695040888963407ull;
                    const float white = static_cast<float> (static_cast<int64_t> (hissRng >> 32) - 2147483648LL) * (1.0f / 2147483648.0f);
                    const float hiss = white - hissHp[c]; // first difference: bright, hiss-like
                    hissHp[c] = white;
                    v += hissGain * hiss;
                }
                e[i] = v;
            }
        }

    driveK += driveKInc * static_cast<float> (len);
    driveMix = std::clamp (driveMix + driveMixInc * static_cast<float> (len), 0.0f, 1.0f);

    if (sidechainActive)
    {
        const float inv = 1.0f / static_cast<float> (scChannels);
        for (int i = 0; i < len; ++i)
        {
            float m = 0.0f;
            for (int c = 0; c < scChannels; ++c)
                m += sc[c][offset + i];
            scMonoBuf[static_cast<size_t> (i)] = std::isfinite (m) ? m * inv : 0.0f;
        }
    }

    auto& spare = store->spareSlot (TraceStore::kInputSpare);
    auto& echoSpare = store->spareSlot (TraceStore::kEchoSpare);
    const int storeCh = store->numChannels();

    const float invCh = 1.0f / static_cast<float> (ioChannels);

    for (int i = 0; i < len; ++i)
    {
        const int64_t w = writeIdx + i;
        float mono = 0.0f, echoMono = 0.0f;
        float rec[kMaxChannels] {};
        float ech[kMaxChannels] {};
        for (int c = 0; c < ioChannels; ++c)
        {
            float* ch = io[c] + offset;
            const float x = ch[i];
            const float e = echoBuf[static_cast<size_t> (c)][static_cast<size_t> (i)];
            float fbSignal = fbGain * e;
            if (feedbackEqActive)
                fbSignal = feedbackEq.process (c, fbSignal); // RE-201 bass/treble act on the repeats
            rec[c] = recordEchoOnly ? e : softClip (x + fbSignal);
            ech[c] = e;
            mono += rec[c];
            echoMono += e;
            float out = dryGain * x + echoGain * e;
            if (springGain > 0.0f || springInc > 0.0f)
                out += springGain * spring.process (c, echoGain * e + (params.springOnDry ? dryGain * x : 0.0f));
            ch[i] = out;
        }
        for (int c = 0; c < storeCh; ++c)
        {
            spare.audio[c][w] = rec[std::min (c, ioChannels - 1)];
            if (recordEchoToo)
                echoSpare.audio[c][w] = ech[std::min (c, ioChannels - 1)];
        }
        mono *= invCh;
        monoBuf[static_cast<size_t> (i)] = mono;
        segEnergy += static_cast<double> (mono) * mono;

        // The cue signal: the recorded input, or the sidechain.
        const float cueMono = sidechainActive ? scMonoBuf[static_cast<size_t> (i)] : mono;
        cueSegEnergy += static_cast<double> (cueMono) * cueMono;
        envAcc += static_cast<double> (cueMono) * cueMono;
        if (++envCount == envStep)
        {
            envRing[static_cast<size_t> (envProduced % kEnvRing)] = static_cast<float> (std::sqrt (envAcc / envStep));
            ++envProduced;
            envAcc = 0.0;
            envCount = 0;
        }
        ++streamPos;

        if (recordEchoToo)
        {
            echoMono *= invCh;
            echoMonoBuf[static_cast<size_t> (i)] = echoMono;
            echoSegEnergy += static_cast<double> (echoMono) * echoMono;
        }
        dryGain += dryInc;
        echoGain += echoInc;
        springGain += springInc;
        fbGain += fbInc;
    }
    features.push (monoBuf.data(), len, writeIdx);
    if (sidechainActive)
        scFeatures.push (scMonoBuf.data(), len, writeIdx);
    if (recordEchoToo)
        echoFeatures.push (echoMonoBuf.data(), len, writeIdx);
    writeIdx += len;
}

void EchoEngine::mixAudition (float* const* io, int ioChannels, int numSamples) noexcept
{
    if (! audition.active && audition.solo <= 0.0f)
        return;
    // The trace must still be the one we started (it may have been replaced).
    if (audition.active && (audition.slot < 0 || store->slot (audition.slot).serial != audition.serial))
        audition.active = false;

    const float soloStep = 1.0f / std::max (1.0f, static_cast<float> (0.01 * sampleRate)); // 10 ms crossfade
    const int storeCh = store->numChannels();
    for (int i = 0; i < numSamples; ++i)
    {
        float x[kMaxChannels] {};
        if (audition.active)
        {
            const auto& t = store->slot (audition.slot);
            const int64_t fade = std::max<int64_t> (1, std::min<int64_t> (static_cast<int64_t> (0.005 * sampleRate),
                                                                          (t.end - t.begin) / 4));
            if (audition.pos >= t.end)
            {
                // Next in the pair, back to the start (loop), or done.
                const uint64_t next = audition.nextSerial != 0 && audition.serial != audition.nextSerial
                                          ? audition.nextSerial
                                          : (audition.loop ? audition.firstSerial : 0);
                const int pos = next != 0 ? store->positionOfSerial (next) : -1;
                if (pos < 0)
                    audition.active = false;
                else
                {
                    audition.serial = next;
                    audition.slot = store->storedSlot (pos);
                    audition.pos = store->slot (audition.slot).begin;
                }
            }
            if (audition.active)
            {
                const auto& tr = store->slot (audition.slot);
                const float g = std::min ({ 1.0f, static_cast<float> (audition.pos - tr.begin + 1) / static_cast<float> (fade),
                                            static_cast<float> (tr.end - audition.pos) / static_cast<float> (fade) });
                for (int c = 0; c < ioChannels; ++c)
                    x[c] = g * tr.audio[std::min (c, storeCh - 1)][audition.pos];
                ++audition.pos;
            }
        }
        audition.solo = std::clamp (audition.solo + (audition.active ? soloStep : -soloStep), 0.0f, 1.0f);
        for (int c = 0; c < ioChannels; ++c)
            io[c][i] = io[c][i] * (1.0f - audition.solo) + x[c] * audition.solo;
    }
    stats.auditionSerial.store (audition.active ? audition.serial : 0, std::memory_order_relaxed);
}

void EchoEngine::publishView() noexcept
{
    if (! viewEnabled.load (std::memory_order_acquire) || store == nullptr)
        return;
    auto& v = *viewBuffers[static_cast<size_t> (viewBack)];
    const int slots = std::min (store->capacity() + TraceStore::kSpares, static_cast<int> (viewActivation.size()));

    // Per-slot activation (main head) and what is playing on each head.
    std::fill (viewActivation.begin(), viewActivation.begin() + slots, 0.0f);
    std::fill (viewPlay.begin(), viewPlay.begin() + slots, std::array<float, kNumHeads> {});
    for (int i = 0; i < headWeightCount[0]; ++i)
    {
        const auto& w = headWeights[0][static_cast<size_t> (i)];
        if (w.slot >= 0 && w.slot < slots)
            viewActivation[static_cast<size_t> (w.slot)] = w.activation;
    }
    for (int h = 0; h < kNumHeads; ++h)
    {
        const auto& pl = headCurrent[static_cast<size_t> (h)];
        const float headGain = headGainNow[h];
        for (int i = 0; i < pl.count; ++i)
        {
            const auto& e = pl.entries[static_cast<size_t> (i)];
            // Entries can still point into a store that was just replaced.
            if (e.slot < 0 || e.slot >= slots || store->slot (e.slot).audio[0] != e.audio[0])
                continue;
            viewPlay[static_cast<size_t> (e.slot)][static_cast<size_t> (h)] += std::abs (e.weight) * headGain;
        }
    }

    const int n = std::min (store->size(), static_cast<int> (v.rows.size()));
    const uint64_t now = store->currentSerial();
    const double sr = store->sampleRate();
    for (int i = 0; i < n; ++i)
    {
        const int slot = store->storedSlot (i);
        const auto& s = store->slot (slot);
        auto& r = v.rows[static_cast<size_t> (i)];
        r.serial = s.serial;
        r.activation = viewActivation[static_cast<size_t> (slot)];
        for (int h = 0; h < kNumHeads; ++h)
            r.play[h] = viewPlay[static_cast<size_t> (slot)][static_cast<size_t> (h)];
        r.rms = s.rms;
        r.strength = s.strength;
        r.useCount = s.useCount;
        r.seconds = static_cast<float> (static_cast<double> (s.end - s.begin) / sr);
        r.age = static_cast<int> (now > s.serial ? now - s.serial - 1 : 0);
        r.generation = s.generation;
        r.mergeCount = s.mergeCount;
        r.clamped = s.clamped;

        v.rowSlot[static_cast<size_t> (i)] = slot;
        const uint64_t key = (s.serial << 20) ^ s.featureVersion ^ (static_cast<uint64_t> (viewStoreEpoch) << 58);
        if (slot < slots && v.thumbKeys[static_cast<size_t> (slot)] != key)
        {
            v.thumbKeys[static_cast<size_t> (slot)] = key;
            auto quantise = [] (const FeatureVector& f, int8_t* t) {
                for (int j = 0; j < kFeatureSize; ++j)
                {
                    const float q = std::clamp (f[static_cast<size_t> (j)] * MemoryView::kThumbScale, -127.0f, 127.0f);
                    t[j] = static_cast<int8_t> (q >= 0.0f ? q + 0.5f : q - 0.5f);
                }
            };
            quantise (s.features, v.thumbs.data() + static_cast<size_t> (slot) * kFeatureSize);
            quantise (s.context, v.contextThumbs.data() + static_cast<size_t> (slot) * kFeatureSize);
        }
    }
    v.count = n;
    v.capacity = store->capacity();
    v.clampLimit = maxClamped();
    v.intensity = stats.intensity.load (std::memory_order_relaxed);
    v.maxActivation = stats.maxActivation.load (std::memory_order_relaxed);
    v.heard = prevHeard;           // the last complete segment
    v.heardBefore = heardBefore;   // and the one before it
    v.sequence = params.sequenceContext;
    v.contextCue = static_cast<int> (params.contextCue);
    v.chain = params.cueSource == CueSource::EchoChain;
    v.version = ++viewVersion;

    viewBack = viewMiddle.exchange (viewBack | kViewDirty, std::memory_order_acq_rel) & 3;
}

void EchoEngine::publishStats() noexcept
{
    stats.tracesStored.store (store ? store->size() : 0, std::memory_order_relaxed);
    stats.capacity.store (store ? store->capacity() : 0, std::memory_order_relaxed);
    stats.clamped.store (store ? store->clampedCount() : 0, std::memory_order_relaxed);
    stats.slotSeconds.store (store ? static_cast<double> (store->slotSamples()) / sampleRate : 0.0,
                             std::memory_order_relaxed);
    stats.segments.store (segmentCount, std::memory_order_relaxed);
    stats.evictions.store (evictionCount, std::memory_order_relaxed);
    stats.merges.store (mergeCount, std::memory_order_relaxed);
    stats.rejections.store (rejectionCount, std::memory_order_relaxed);
    stats.cueUpdates.store (cueUpdates, std::memory_order_relaxed);
    stats.lastWrite.store (static_cast<int> (lastWrite), std::memory_order_relaxed);
    stats.captureArmed.store (captureArmed, std::memory_order_relaxed);
}

} // namespace mse
