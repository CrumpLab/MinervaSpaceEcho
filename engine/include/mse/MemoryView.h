#pragma once

#include "mse/Features.h"
#include "mse/Params.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mse {

// A picture of memory for the UI (plan Stage 7, memory matrix view),
// published by the audio thread a few dozen times a second and read on the
// message thread without locks (triple buffer, see EchoEngine::readMemoryView).
struct TraceView
{
    uint64_t serial = 0;       // identifies the trace (commands address traces by serial)
    float activation = 0.0f;   // A_i in the main head's last retrieval (0 if negligible)
    float play[kNumHeads] {};  // |gain| this trace currently plays at, per head
    float rms = 0.0f;
    float strength = 1.0f;
    float useCount = 0.0f;
    float seconds = 0.0f;      // recorded length
    int age = 0;               // traces stored since this one
    int generation = 0;
    int mergeCount = 1;
    bool clamped = false;
    // Probe (Stage 11), when one is set:
    float probe = 0.0f;        // activation A_i for the probe (0 = negligible, or the probe itself left out)
    float probeSim = 0.0f;     // similarity to the probe under the current Address (weighted mix)
    std::array<float, kNumSets> probeSims {}; // similarity to the probe under each address set
};

struct MemoryView
{
    uint64_t version = 0;      // increments with every publish
    int count = 0;             // rows[0..count) are valid, oldest first
    int capacity = 0;
    int clampLimit = 0;        // clamp budget in traces
    float intensity = 0.0f;
    float maxActivation = 0.0f;
    std::vector<TraceView> rows;

    // Each trace's address in the shown set (Stage 10: the most weighted
    // address set; layout kSetLayouts[shownSet]), quantised: value * kThumbScale.
    // Stored per memory slot (traces keep their slot while others come and
    // go), so only new or changed traces are re-quantised.
    static constexpr float kThumbScale = 40.0f;
    int shownSet = 0;                    // AddressSet index the thumbs show
    std::vector<int> rowSlot;            // per row: its memory slot
    std::vector<int8_t> thumbs;          // per slot: kSetSize values
    std::vector<int8_t> contextThumbs;   // per slot: the context half (n-1), same layout
    std::vector<uint64_t> thumbKeys;     // per slot: which trace / feature version the thumb shows

    FeatureVector heard {};    // address of the last segment heard (what cued memory), every set
    FeatureVector heardBefore {}; // the segment before it: [heardBefore | heard] is the last [n-1 | n]

    // Sequential context (Stage 9)
    bool sequence = false;     // Sequence Context on: show [n-1 | n]
    int contextCue = 1;        // ContextCue index
    bool chain = false;        // Cue Source = Echo Chain

    // Probe (Stage 11): a trace's address used as a cue, for inspection.
    uint64_t probeSerial = 0;  // 0 = none
    bool probeNext = false;    // compared with traces' n-1 halves (else n)
    bool probeIncludeSelf = false;
    bool probePlaying = false, probeLoop = false, probeFull = false;
    float probeIntensity = 0.0f; // sum of the probe's activations
    int probeCount = 0;        // traces with a non-negligible activation

private:
    bool blend (SetVector& out, bool contextHalf) const noexcept
    {
        out.fill (0.0f);
        double total = 0.0;
        for (int i = 0; i < count; ++i)
        {
            const float a = rows[static_cast<size_t> (i)].activation;
            if (! (a > 0.0f || a < 0.0f))
                continue;
            total += a < 0.0f ? -a : a;
            const int8_t* t = contextHalf ? contextThumb (i) : thumb (i);
            for (int j = 0; j < kSetSize; ++j)
                out[static_cast<size_t> (j)] += a * static_cast<float> (t[j]);
        }
        if (total <= 0.0)
            return false;
        // Scaled to unit RMS over the encoded cells; no mean is taken out,
        // so unencoded (0) cells stay 0 (the pitch sets are mostly zeros).
        const int n = setCells (shownSet);
        double sq = 0.0;
        int encoded = 0;
        for (int j = 0; j < n; ++j)
            if (const float v = out[static_cast<size_t> (j)]; v != 0.0f)
            {
                sq += static_cast<double> (v) * v;
                ++encoded;
            }
        const double rms = encoded > 0 ? std::sqrt (sq / encoded) : 0.0;
        for (int j = 0; j < n; ++j)
        {
            auto& v = out[static_cast<size_t> (j)];
            v = rms > 0.0 ? static_cast<float> (v / rms) : 0.0f;
        }
        return true;
    }

public:
    const int8_t* thumb (int row) const noexcept
    {
        return thumbs.data() + static_cast<size_t> (rowSlot[static_cast<size_t> (row)]) * kSetSize;
    }
    // The echo's content (MINERVA's echo): the activation-weighted mean of
    // the rows' addresses, re-normalised. False if nothing is active.
    // Computed by the reader, off the audio thread.
    bool echoContent (SetVector& out) const noexcept { return blend (out, false); }

    // The echo's context half: the weighted mean of the answering traces' n-1 halves.
    bool echoContextContent (SetVector& out) const noexcept { return blend (out, true); }

    // One set of an address (e.g. `heard`) as the thumbs show it.
    SetVector shown (const FeatureVector& f) const noexcept
    {
        SetVector out {};
        std::copy (f.begin() + setOffset (shownSet), f.begin() + setOffset (shownSet) + kSetSize, out.begin());
        return out;
    }

    const int8_t* contextThumb (int row) const noexcept
    {
        return contextThumbs.data() + static_cast<size_t> (rowSlot[static_cast<size_t> (row)]) * kSetSize;
    }

    // Changes whenever the row shows a different trace or its address changes.
    uint64_t thumbKey (int row) const noexcept { return thumbKeys[static_cast<size_t> (rowSlot[static_cast<size_t> (row)])]; }
};

} // namespace mse
