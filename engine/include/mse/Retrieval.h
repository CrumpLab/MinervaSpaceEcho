#pragma once

#include "mse/Features.h"
#include "mse/Params.h"
#include "mse/TraceStore.h"

namespace mse {

// Similarity between a probe and a trace, clamped to [-1, 1].
//  Hintzman: sum(p*t) / N, N = number of features non-zero in p or t (1986).
//  Cosine:   sum(p*t) / (|p| |t|).
float similarity (const FeatureVector& probe, const FeatureVector& trace, Similarity kind) noexcept;

// sign(S) * |S|^power
float activation (float s, float power) noexcept;

struct RetrievalSettings
{
    Similarity similarity = Similarity::Hintzman;
    float power = 3.0f;
    NegativeMode negativeMode = NegativeMode::Subtract;
    Normalization normalization = Normalization::Sum;
    int excludeSlot = -1;   // slot to ignore (self-match off), or -1
    FeatureFocus focus = FeatureFocus::Full;
    float recency = 0.0f;   // 0..1: activation *= exp(-recency * age / 4), age in traces stored since

    // Sequential context (Stage 9). A trace's address is [context | features]
    // ([n-1 | n]). When contextWeight > 0, `contextProbe` is compared with
    // each trace's context half, the probe with its features, and the
    // similarity is the weighted mean of the two:
    //   S = (contextWeight * S_context + currentWeight * S_current) / (contextWeight + currentWeight)
    // With contextWeight = 0 (the default) retrieval is exactly MINERVA II.
    const FeatureVector* contextProbe = nullptr;
    float contextWeight = 0.0f;
    float currentWeight = 1.0f;
};

// Combines the two halves' similarities as above.
inline float combineHalves (float sContext, float wContext, float sCurrent, float wCurrent) noexcept
{
    if (wContext <= 0.0f)
        return sCurrent;
    if (wCurrent <= 0.0f)
        return sContext;
    return (wContext * sContext + wCurrent * sCurrent) / (wContext + wCurrent);
}

// Feature focus (plan §4): compare only rhythm (loudness over time: each
// slot's mean over bands, kept in band 0) or only timbre (each band's mean
// over slots [slotBegin, slotEnd), kept in slot 0). Full returns the input.
FeatureVector focusVector (const FeatureVector& v, FeatureFocus focus, int slotBegin = 0, int slotEnd = kSlots) noexcept;

// Similarity over slots [slotBegin, slotEnd) under a feature focus.
float focusedSimilarity (const FeatureVector& probe, const FeatureVector& trace, Similarity kind, FeatureFocus focus,
                         int slotBegin, int slotEnd, bool renormalize) noexcept;


struct EchoWeight
{
    int slot;
    float activation;  // A_i after negative-mode handling (on input to finishRetrieval: the similarity S_i)
    float weight;      // A_i * g: the gain applied to this trace's audio
    int64_t offset = 0; // caller data carried through (e.g. a playback offset)
};

struct RetrievalResult
{
    float intensity = 0.0f;    // sum of A_i (MINERVA's familiarity)
    float sumAbs = 0.0f;
    float maxAbs = 0.0f;
    float gain = 0.0f;         // normalisation factor g
    int numWeights = 0;        // entries written to the output array
};

// Cues every stored trace with `probe` and writes the non-negligible echo
// weights to `out` (which must hold store.capacity() entries). A trace's
// activation is scaled by its strength (which decays over time).
RetrievalResult retrieve (const FeatureVector& probe, const TraceStore& store,
                          const RetrievalSettings& settings, EchoWeight* out) noexcept;

// Shared second half of every retrieval: out[0..n) hold {slot, S_i, -, offset};
// converts similarities to activations (power, strength, negative mode),
// normalises them, drops negligible ones and returns the result.
RetrievalResult finishRetrieval (const TraceStore& store, const RetrievalSettings& settings,
                                 EchoWeight* out, int n) noexcept;

// Similarity restricted to slots [slotBegin, slotEnd) (Progressive cue: the
// part of the bar heard so far). With `renormalize`, the trace is re-scaled
// over that range so a partial bar compares fairly with a whole one.
float prefixSimilarity (const FeatureVector& probe, const FeatureVector& trace, Similarity kind,
                        int slotBegin, int slotEnd, bool renormalize) noexcept;

// ---- Rolling cue (unclocked search over frame tracks) ----

constexpr int kMaxWindowFrames = 100; // 2 s of 20 ms frames

// The live cue: W frames, centred and scaled to unit length so matching is
// level-independent (Pearson correlation).
struct WindowProbe
{
    std::array<float, static_cast<size_t> (kMaxWindowFrames) * kBands> v {};
    int frames = 0;
};

// Builds a probe from W frame pointers (oldest first). False if the window is flat.
bool prepareWindowProbe (const float* const* frames, int numFrames, WindowProbe& out) noexcept;

struct OffsetMatch
{
    int offset = -1;         // first frame of the best-matching window in the trace
    float similarity = -2.0f;
};

// Slides the probe over the trace's frame track and returns the best match
// that still leaves `minContinuation` frames of the trace to play after it.
OffsetMatch bestOffset (const WindowProbe& probe, const TraceSlot& trace, int minContinuation) noexcept;

// The echo's own address (MINERVA's echo content): activation-weighted mean
// of the retrieved traces' features, re-normalised (or re-ternarised). Used to
// cue iterative heads with the previous echo.
void echoAddress (const EchoWeight* weights, int n, const TraceStore& store, FeatureMode mode,
                   float ternaryThreshold, FeatureVector& out, bool contextHalf = false) noexcept;

struct BestMatch
{
    int position = -1;   // storage position of the most similar trace, or -1
    float similarity = -2.0f;
};

// The stored trace most similar to `probe` (optionally ignoring clamped ones).
BestMatch bestMatch (const FeatureVector& probe, const TraceStore& store, Similarity kind,
                     bool unclampedOnly) noexcept;

} // namespace mse
