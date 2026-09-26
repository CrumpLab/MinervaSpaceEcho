#include "mse/EchoEngine.h"

#include <algorithm>
#include <cmath>

namespace mse {

const char* versionString() noexcept { return MSE_VERSION_STRING; }

namespace {

constexpr double kJumpToleranceQuarters = 0.02; // host position jumps larger than this re-sync the grid
constexpr double kMinCoverage = 0.5;            // store a segment only if at least this much was recorded
constexpr float kMaxTrackingGain = 4.0f;        // level tracking never boosts memories by more than +12 dB

float levelToGain (float db) noexcept
{
    return db <= kLevelOffDb ? 0.0f : std::pow (10.0f, db / 20.0f);
}

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

float uniform01 (uint64_t& state) noexcept
{
    return static_cast<float> (splitmix (state) >> 40) * (1.0f / 16777216.0f);
}

} // namespace

EchoEngine::EchoEngine()
{
    retired.reserve (8);
    weights.resize (kMaxCapacity);
    current.entries.resize (kMaxCapacity);
    previous.entries.resize (kMaxCapacity);
}

EchoEngine::~EchoEngine() = default;

// ---- non-real-time ------------------------------------------------------------

void EchoEngine::prepare (double sr, int maxBlock, int channels)
{
    sampleRate = sr > 0.0 ? sr : 48000.0;
    maxBlockSize = std::max (1, maxBlock);
    numChannels = std::clamp (channels, 1, kMaxChannels);

    features.prepare (sampleRate);
    for (auto& b : echoBuf)
        b.assign (static_cast<size_t> (maxBlockSize), 0.0f);
    monoBuf.assign (static_cast<size_t> (maxBlockSize), 0.0f);

    MemoryConfig cfg;
    {
        std::lock_guard lock (handoffMutex);
        cfg = requestedConfig;
        pendingStore.reset();
        retired.clear();
        hasPending.store (false);
    }
    store = std::make_unique<TraceStore> (cfg, sampleRate, numChannels);
    prepared = true;

    outSmoothing = std::exp (-1.0f / (0.02f * static_cast<float> (sampleRate)));
    rngState = seed_;
    reset();
}

void EchoEngine::setMemoryConfig (const MemoryConfig& requested)
{
    MemoryConfig cfg = requested;
    cfg.capacity = std::clamp (cfg.capacity, 1, kMaxCapacity);

    double sr = 0.0;
    int ch = 0;
    {
        std::lock_guard lock (handoffMutex);
        if (cfg == requestedConfig)
            return;
        requestedConfig = cfg;
        if (! prepared)
            return;
        sr = sampleRate;
        ch = numChannels;
    }

    auto fresh = std::make_unique<TraceStore> (cfg, sr, ch); // allocate outside the lock
    std::unique_ptr<TraceStore> replaced;
    {
        std::lock_guard lock (handoffMutex);
        replaced = std::move (pendingStore);
        pendingStore = std::move (fresh);
        hasPending.store (true, std::memory_order_release);
    }
    // `replaced` (never picked up by the audio thread) is freed here.
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

// ---- any thread -----------------------------------------------------------------

EngineStats EchoEngine::getStats() const noexcept
{
    EngineStats s;
    s.tracesStored = stats.tracesStored.load (std::memory_order_relaxed);
    s.capacity = stats.capacity.load (std::memory_order_relaxed);
    s.activeTraces = stats.activeTraces.load (std::memory_order_relaxed);
    s.slotSeconds = stats.slotSeconds.load (std::memory_order_relaxed);
    s.traceSeconds = stats.traceSeconds.load (std::memory_order_relaxed);
    s.segmentPhase = stats.segmentPhase.load (std::memory_order_relaxed);
    s.intensity = stats.intensity.load (std::memory_order_relaxed);
    s.maxActivation = stats.maxActivation.load (std::memory_order_relaxed);
    s.segments = stats.segments.load (std::memory_order_relaxed);
    return s;
}

// ---- audio thread -----------------------------------------------------------------

void EchoEngine::reset() noexcept
{
    if (store)
        store->clear();
    current.count = previous.count = 0;
    writeIdx = segBegin = 0;
    segNominal = 0.0;
    needResync = true;
    boundaryPending = false;
    segmentCount = 0;
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
    if (retired.size() == retired.capacity())
        return; // garbage not collected yet; try again next block rather than allocate
    retired.push_back (std::move (store));
    store = std::move (pendingStore);
    hasPending.store (false);

    current.count = previous.count = 0;
    writeIdx = segBegin = 0;
    needResync = true;
    boundaryPending = false;
    segmentCount = 0;
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

void EchoEngine::process (float* const* io, int ioChannels, int numSamples, const HostClock& clock) noexcept
{
    lastClock = clock;
    if (store == nullptr || numSamples <= 0)
        return;
    ioChannels = std::clamp (ioChannels, 1, kMaxChannels);

    adoptPendingStore();
    if (clearRequested.exchange (false))
    {
        store->clear();
        current.count = previous.count = 0;
        segmentCount = 0;
        writeIdx = segBegin = 0; // drop the half-recorded segment too
        segEnergy = 0.0;
        needResync = true;
        boundaryPending = false;
    }

    fadeSamples = static_cast<int64_t> (std::llround (params.edgeFadeMs * 0.001 * sampleRate));

    const double nominal = nominalLength (clock);
    const auto period = static_cast<int64_t> (std::llround (nominal));
    const int64_t slotLen = store->slotSamples();
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
    int64_t nextBoundary = std::min (firstBoundary, slotLen - writeIdx);
    while (done < numSamples)
    {
        if (nextBoundary <= 0)
        {
            boundary (0, nominal);
            nextBoundary = std::min (period, slotLen);
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
}

void EchoEngine::boundary (int64_t newBegin, double newNominal) noexcept
{
    const int64_t recorded = writeIdx - segBegin;

    if (recorded > 0)
    {
        const FeatureSettings fs { params.featureMode, params.ternaryThreshold };
        features.finalize (probe, fs);

        const float cueRms = static_cast<float> (std::sqrt (segEnergy / static_cast<double> (recorded)));
        int selfSlot = -1;
        if (static_cast<double> (recorded) >= kMinCoverage * segNominal)
        {
            auto& spare = store->spareSlot();
            spare.begin = segBegin;
            spare.end = writeIdx;
            spare.rms = cueRms;
            // Encoding failure: the stored copy loses features; the probe (what
            // was actually heard) stays intact.
            spare.features = probe;
            if (params.encodingFailure > 0.0f)
                for (auto& f : spare.features)
                    if (uniform01 (rngState) < params.encodingFailure)
                        f = 0.0f;
            selfSlot = store->commitSpare();
            ++segmentCount;
        }

        // Cue gate: a (near-)silent segment evokes nothing, even though its
        // level-independent features could still match loud memories.
        const double rmsDb = 10.0 * std::log10 (segEnergy / static_cast<double> (recorded) + 1.0e-30);
        const bool gated = params.cueGateDb > kCueGateOffDb && rmsDb < params.cueGateDb;

        RetrievalSettings rs;
        rs.similarity = params.similarity;
        rs.power = params.power;
        rs.negativeMode = params.negativeMode;
        rs.normalization = params.normalization;
        rs.excludeSlot = params.selfMatch ? -1 : selfSlot;
        const auto result = gated ? RetrievalResult {} : retrieve (probe, *store, rs, weights.data());

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

        current.count = result.numWeights;
        for (int i = 0; i < result.numWeights; ++i)
        {
            const auto& w = weights[static_cast<size_t> (i)];
            const auto& s = store->slot (w.slot);
            current.entries[static_cast<size_t> (i)] = { { s.audio[0], s.audio[1] }, s.begin, s.end, w.weight * tracking };
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
    segEnergy = 0.0;
    segNominal = newNominal;
    features.beginSegment (segNominal);
    publishStats();
}

void EchoEngine::accumulate (Playlist& pl, int ioChannels, int len) noexcept
{
    if (pl.count > 0)
    {
        const int storeCh = store->numChannels();
        for (int k = 0; k < pl.count; ++k)
        {
            const auto& e = pl.entries[static_cast<size_t> (k)];
            const int64_t fade = std::min (fadeSamples, (e.end - e.begin) / 4);
            const int64_t start = std::max<int64_t> (0, e.begin - pl.pos);
            const int64_t stop = std::min<int64_t> (len, e.end - pl.pos);
            for (int c = 0; c < ioChannels; ++c)
            {
                const float* src = e.audio[std::min (c, storeCh - 1)];
                float* dst = echoBuf[static_cast<size_t> (c)].data();
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
                    dst[i] += g * src[t];
                }
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

    auto& spare = store->spareSlot();
    const int storeCh = store->numChannels();
    const float fb = params.feedback;
    const float invCh = 1.0f / static_cast<float> (ioChannels);

    for (int i = 0; i < len; ++i)
    {
        const int64_t w = writeIdx + i;
        float mono = 0.0f;
        float rec[kMaxChannels] {};
        for (int c = 0; c < ioChannels; ++c)
        {
            float* ch = io[c] + offset;
            const float x = ch[i];
            const float e = echoBuf[static_cast<size_t> (c)][static_cast<size_t> (i)];
            rec[c] = softClip (x + fb * e);
            mono += rec[c];
            ch[i] = dryGain * x + echoGain * e;
        }
        for (int c = 0; c < storeCh; ++c)
            spare.audio[c][w] = rec[std::min (c, ioChannels - 1)];
        monoBuf[static_cast<size_t> (i)] = mono * invCh;
        segEnergy += static_cast<double> (mono * invCh) * (mono * invCh);
        dryGain += dryInc;
        echoGain += echoInc;
    }
    features.push (monoBuf.data(), len, writeIdx);
    writeIdx += len;
}

void EchoEngine::publishStats() noexcept
{
    stats.tracesStored.store (store ? store->size() : 0, std::memory_order_relaxed);
    stats.capacity.store (store ? store->capacity() : 0, std::memory_order_relaxed);
    stats.slotSeconds.store (store ? static_cast<double> (store->slotSamples()) / sampleRate : 0.0,
                             std::memory_order_relaxed);
    stats.segments.store (segmentCount, std::memory_order_relaxed);
}

} // namespace mse
