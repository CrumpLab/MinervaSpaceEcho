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
    current.entries.resize (kMaxCapacity);
    previous.entries.resize (kMaxCapacity);
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
    {
        std::lock_guard lock (handoffMutex);
        if (cfg == requestedConfig)
            return;
        requestedConfig = cfg;
        if (! prepared)
            return;
        sr = sampleRate;
        ch = numChannels;
        if (pendingStore && ! pendingKeepsTraces)
            pendingLoadStore = std::move (pendingStore); // a load not yet picked up: re-fit it
    }

    auto fresh = std::make_unique<TraceStore> (cfg, sr, ch); // allocate outside the lock
    bool keep = true;
    if (pendingLoadStore)
    {
        fresh->adoptFrom (*pendingLoadStore, true);
        keep = false;
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
        meta.serial = t.serial;
        meta.rms = t.rms;
        meta.strength = t.strength;
        meta.useCount = t.useCount;
        meta.generation = t.generation;
        meta.mergeCount = t.mergeCount;
        meta.clamped = t.clamped;
        meta.features = t.features;

        // Audio laid out from segment position 0 (silence before `begin`).
        std::vector<std::vector<float>> audio (t.audio.size());
        std::vector<const float*> ptrs;
        for (size_t c = 0; c < t.audio.size(); ++c)
        {
            audio[c].assign (static_cast<size_t> (t.begin), 0.0f);
            audio[c].insert (audio[c].end(), t.audio[c].begin(), t.audio[c].end());
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
                t.serial = s.serial;
                t.rms = s.rms;
                t.strength = s.strength;
                t.useCount = s.useCount;
                t.generation = s.generation;
                t.mergeCount = s.mergeCount;
                t.clamped = s.clamped;
                t.features = s.features;
                snap.traces.push_back (std::move (t));
            }
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

void EchoEngine::sendCommand (Command c) noexcept
{
    const uint32_t tail = queueTail.load (std::memory_order_relaxed);
    if (tail - queueHead.load (std::memory_order_acquire) >= static_cast<uint32_t> (kQueueSize))
        return; // queue full: drop
    commandQueue[tail % kQueueSize].store (static_cast<int> (c), std::memory_order_relaxed);
    queueTail.store (tail + 1, std::memory_order_release);
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
    s.lastWrite = static_cast<WriteOutcome> (stats.lastWrite.load (std::memory_order_relaxed));
    s.captureArmed = stats.captureArmed.load (std::memory_order_relaxed);
    return s;
}

// ==== audio thread ===================================================================

void EchoEngine::resetPlayback() noexcept
{
    current.count = previous.count = 0;
    writeIdx = segBegin = 0;
    segNominal = 0.0;
    segEnergy = echoSegEnergy = 0.0;
    needResync = true;
    boundaryPending = false;
    recordEchoOnly = recordEchoToo = false;
    captureArmed = false;
    dryGain = levelToGain (params.dryLevelDb);
    echoGain = levelToGain (params.echoLevelDb);
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
    if (! pendingKeepsTraces)
    {
        segmentCount = static_cast<uint64_t> (store->size());
        evictionCount = mergeCount = rejectionCount = 0;
    }
    endMutation();
    publishStats();
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

    uint32_t head = queueHead.load (std::memory_order_relaxed);
    const uint32_t tail = queueTail.load (std::memory_order_acquire);
    if (head == tail)
        return;

    beginMutation();
    for (; head != tail; ++head)
    {
        const auto c = static_cast<Command> (commandQueue[head % kQueueSize].load (std::memory_order_relaxed));
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
                store->clearUnclamped();
                dropMissingEntries (current);
                dropMissingEntries (previous);
                break;
            case Command::ClearAll:
                store->clear();
                current.count = previous.count = 0;
                segmentCount = evictionCount = mergeCount = rejectionCount = 0;
                writeIdx = segBegin = 0; // drop the half-recorded segment too
                segEnergy = echoSegEnergy = 0.0;
                needResync = true;
                boundaryPending = false;
                break;
        }
    }
    queueHead.store (head, std::memory_order_release);
    endMutation();
    publishStats();
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

void EchoEngine::process (float* const* io, int ioChannels, int numSamples, const HostClock& clock) noexcept
{
    lastClock = clock;
    if (store == nullptr || numSamples <= 0)
        return;
    ioChannels = std::clamp (ioChannels, 1, kMaxChannels);

    if (holdRequests.load (std::memory_order_acquire) == 0)
        adoptPendingStore();
    processCommands();

    if (params.capture && ! lastCaptureParam)
        captureArmed = true; // rising edge of the Capture parameter
    lastCaptureParam = params.capture;

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
    echoInc = (levelToGain (params.echoLevelDb) - echoGain) / n;

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
        const int chunk = static_cast<int> (std::min<int64_t> (numSamples - done, nextBoundary));
        processChunk (io, ioChannels, done, chunk);
        done += chunk;
        nextBoundary -= chunk;
    }
    if (nextBoundary <= 0)
        boundaryPending = true; // segment ended exactly on the block edge

    dryGain = levelToGain (params.dryLevelDb);
    echoGain = levelToGain (params.echoLevelDb);

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

    stats.traceSeconds.store (nominal / sampleRate, std::memory_order_relaxed);
    stats.segmentPhase.store (static_cast<float> (std::clamp (writeIdx / std::max (1.0, segNominal), 0.0, 1.0)),
                              std::memory_order_relaxed);
    stats.captureArmed.store (captureArmed, std::memory_order_relaxed);
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
        dst.features[j] = dst.features[j] * (1.0f - w) + src.features[j] * w;
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
            for (auto& f : s.features)
                if (f != 0.0f && random01() < params.decayForget)
                    f = 0.0f;
        if (s.strength < kDeadStrength)
            store->removeAt (i); // faded out completely: forgotten
    }
}

WriteOutcome EchoEngine::writeTrace (int spareIdx, const FeatureVector& feats, float rms, int64_t recorded,
                                     int generation, bool forced, int& outSlot) noexcept
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
    spare.rms = rms;
    spare.generation = generation;
    spare.strength = 1.0f;
    spare.useCount = 0.0f;
    spare.mergeCount = 1;
    spare.clamped = false;
    // Encoding failure: the stored copy loses features; the cue (what was
    // actually heard) stays intact.
    spare.features = feats;
    if (params.encodingFailure > 0.0f)
        for (auto& f : spare.features)
            if (random01() < params.encodingFailure)
                f = 0.0f;
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
            outcome = writeTrace (TraceStore::kInputSpare, probe, cueRms, recorded,
                                  recordEchoOnly ? segEchoGeneration : 0, forced, selfSlot);
            if (forced && outcome != WriteOutcome::None)
                captureArmed = false;

            if (recordEchoToo && echoSegEnergy > 0.0)
            {
                echoFeatures.finalize (echoProbe, fs);
                const float echoRms = static_cast<float> (std::sqrt (echoSegEnergy / static_cast<double> (recorded)));
                int echoSlot = -1;
                writeTrace (TraceStore::kEchoSpare, echoProbe, echoRms, recorded, segEchoGeneration, false, echoSlot);
            }
        }
        lastWrite = outcome;

        // Cue gate: a (near-)silent segment evokes nothing, even though its
        // level-independent features could still match loud memories.
        const bool gated = params.cueGateDb > kCueGateOffDb && toDb (cueRms) < params.cueGateDb;

        RetrievalSettings rs;
        rs.similarity = params.similarity;
        rs.power = params.power;
        rs.negativeMode = params.negativeMode;
        rs.normalization = params.normalization;
        rs.excludeSlot = params.selfMatch ? -1 : selfSlot;
        const auto result = gated ? RetrievalResult {} : retrieve (probe, *store, rs, weights.data());

        if (! held)
        {
            for (int i = 0; i < result.numWeights; ++i)
                store->slot (weights[static_cast<size_t> (i)].slot).useCount += std::abs (weights[static_cast<size_t> (i)].activation);
            endMutation();
        }

        // Level tracking: scale the echo so its level follows the cue's, the way
        // a tape repeat follows what was played. Memory picks *what* returns;
        // the input decides *how loud*. (Capacity 1: the factor is exactly 1.)
        float tracking = 1.0f;
        if (params.levelTracking > 0.0f && result.numWeights > 0)
        {
            // Activation-weighted mean level of what was retrieved. Using the
            // mean (not the weighted sum) leaves the normalisation mode's own
            // loudness behaviour (e.g. Familiarity) intact.
            double weighted = 0.0, total = 0.0;
            for (int i = 0; i < result.numWeights; ++i)
            {
                const auto& w = weights[static_cast<size_t> (i)];
                weighted += std::abs (w.weight) * store->slot (w.slot).rms;
                total += std::abs (w.weight);
            }
            const double memoryRms = total > 0.0 ? weighted / total : 0.0;
            const float ratio = memoryRms > 0.0 ? std::min (kMaxTrackingGain, static_cast<float> (cueRms / memoryRms)) : 0.0f;
            tracking = std::pow (ratio, params.levelTracking);
        }

        // The current echo keeps playing briefly while fading out.
        std::swap (previous.entries, current.entries);
        previous.count = current.count;
        previous.pos = current.pos;
        previous.rampPos = 0;
        previous.rampLen = fadeSamples;
        previous.fadingOut = true;
        if (fadeSamples == 0)
            previous.count = 0;

        const uint64_t now = store->currentSerial();
        current.count = result.numWeights;
        for (int i = 0; i < result.numWeights; ++i)
        {
            const auto& w = weights[static_cast<size_t> (i)];
            const auto& s = store->slot (w.slot);
            float coeff = 1.0f;
            if (params.wearTone > 0.0f)
            {
                // Older traces play back duller, like worn tape.
                const double age = static_cast<double> (now - std::min (now, s.serial));
                const double fc = std::clamp (18000.0 * std::exp (-params.wearTone * age / 8.0), 200.0, 0.45 * sampleRate);
                coeff = static_cast<float> (1.0 - std::exp (-2.0 * 3.14159265358979 * fc / sampleRate));
            }
            current.entries[static_cast<size_t> (i)] = { { s.audio[0], s.audio[1] }, s.begin, s.end,
                                                          w.weight * tracking, w.slot, coeff, { 0.0f, 0.0f } };
        }
        current.fadingOut = false;

        stats.intensity.store (result.intensity, std::memory_order_relaxed);
        stats.maxActivation.store (result.maxAbs, std::memory_order_relaxed);
        stats.activeTraces.store (result.numWeights, std::memory_order_relaxed);
    }

    current.pos = newBegin;
    current.rampPos = 0;
    current.rampLen = newBegin > 0 ? fadeSamples : 0;

    segBegin = writeIdx = newBegin;
    segEnergy = echoSegEnergy = 0.0;
    segNominal = newNominal;
    features.beginSegment (segNominal);
    echoFeatures.beginSegment (segNominal);

    // What does the new segment record? (Echo re-encoding, plan §5.5.)
    int maxGen = 0;
    for (int i = 0; i < current.count; ++i)
        maxGen = std::max (maxGen, store->slot (current.entries[static_cast<size_t> (i)].slot).generation);
    segEchoGeneration = maxGen + 1;
    recordEchoOnly = params.recordSource == RecordSource::Echo && current.count > 0;
    recordEchoToo = params.recordSource == RecordSource::InputAndEcho && current.count > 0;

    publishStats();
}

// ---- playback and recording ------------------------------------------------------------------

void EchoEngine::accumulate (Playlist& pl, int ioChannels, int len) noexcept
{
    if (pl.count > 0)
    {
        const int storeCh = store->numChannels();
        for (int k = 0; k < pl.count; ++k)
        {
            auto& e = pl.entries[static_cast<size_t> (k)];
            const int64_t fade = std::min (fadeSamples, (e.end - e.begin) / 4);
            const int64_t start = std::max<int64_t> (0, e.begin - pl.pos);
            const int64_t stop = std::min<int64_t> (len, e.end - pl.pos);
            for (int c = 0; c < ioChannels; ++c)
            {
                const float* src = e.audio[std::min (c, storeCh - 1)];
                float* dst = echoBuf[static_cast<size_t> (c)].data();
                float z = e.z[c];
                for (int64_t i = start; i < stop; ++i)
                {
                    const int64_t t = pl.pos + i;
                    float g = e.weight;
                    if (fade > 0)
                        g *= std::min ({ 1.0f, static_cast<float> (t - e.begin + 1) / fade,
                                         static_cast<float> (e.end - t) / fade });
                    if (pl.rampLen > 0)
                    {
                        const float r = static_cast<float> (pl.rampPos + i + 1) / static_cast<float> (pl.rampLen);
                        g *= pl.fadingOut ? std::max (0.0f, 1.0f - r) : std::min (1.0f, r);
                    }
                    float x = g * src[t];
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

void EchoEngine::processChunk (float* const* io, int ioChannels, int offset, int len) noexcept
{
    for (int c = 0; c < ioChannels; ++c)
        std::fill_n (echoBuf[static_cast<size_t> (c)].data(), len, 0.0f);
    accumulate (current, ioChannels, len);
    accumulate (previous, ioChannels, len);

    auto& spare = store->spareSlot (TraceStore::kInputSpare);
    auto& echoSpare = store->spareSlot (TraceStore::kEchoSpare);
    const int storeCh = store->numChannels();
    const float fb = params.feedback;
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
            rec[c] = recordEchoOnly ? e : softClip (x + fb * e);
            ech[c] = e;
            mono += rec[c];
            echoMono += e;
            ch[i] = dryGain * x + echoGain * e;
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
        if (recordEchoToo)
        {
            echoMono *= invCh;
            echoMonoBuf[static_cast<size_t> (i)] = echoMono;
            echoSegEnergy += static_cast<double> (echoMono) * echoMono;
        }
        dryGain += dryInc;
        echoGain += echoInc;
    }
    features.push (monoBuf.data(), len, writeIdx);
    if (recordEchoToo)
        echoFeatures.push (echoMonoBuf.data(), len, writeIdx);
    writeIdx += len;
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
    stats.lastWrite.store (static_cast<int> (lastWrite), std::memory_order_relaxed);
    stats.captureArmed.store (captureArmed, std::memory_order_relaxed);
}

} // namespace mse
