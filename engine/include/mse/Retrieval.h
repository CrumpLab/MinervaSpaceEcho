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
};

struct EchoWeight
{
    int slot;
    float activation;  // A_i after negative-mode handling
    float weight;      // A_i * g: the gain applied to this trace's audio
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

struct BestMatch
{
    int position = -1;   // storage position of the most similar trace, or -1
    float similarity = -2.0f;
};

// The stored trace most similar to `probe` (optionally ignoring clamped ones).
BestMatch bestMatch (const FeatureVector& probe, const TraceStore& store, Similarity kind,
                     bool unclampedOnly) noexcept;

} // namespace mse
