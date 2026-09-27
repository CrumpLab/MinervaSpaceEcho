#include "mse/Retrieval.h"

#include <algorithm>
#include <cmath>

namespace mse {

namespace {
constexpr float kNegligible = 1.0e-5f; // relative to the strongest activation
}

float setSimilarity (const float* p, const float* t, Similarity kind) noexcept
{
    // Eight independent float lanes so the loops vectorise; lanes are
    // combined in double.
    constexpr int kLanes = 8;
    static_assert (kSetSize % kLanes == 0);
    float dot[kLanes] {}, a[kLanes] {}, b[kLanes] {};
    const float* __restrict pv = p;
    const float* __restrict tv = t;
    if (kind == Similarity::Hintzman)
    {
        for (int j = 0; j < kSetSize; j += kLanes)
            for (int l = 0; l < kLanes; ++l)
            {
                const float x = pv[j + l], y = tv[j + l];
                dot[l] += x * y;
                a[l] += (x != 0.0f || y != 0.0f) ? 1.0f : 0.0f; // features present in either
            }
        double d = 0.0, n = 0.0;
        for (int l = 0; l < kLanes; ++l)
        {
            d += dot[l];
            n += a[l];
        }
        if (n <= 0.0)
            return 0.0f;
        return static_cast<float> (std::clamp (d / n, -1.0, 1.0));
    }

    for (int j = 0; j < kSetSize; j += kLanes)
        for (int l = 0; l < kLanes; ++l)
        {
            const float x = pv[j + l], y = tv[j + l];
            dot[l] += x * y;
            a[l] += x * x;
            b[l] += y * y;
        }
    double d = 0.0, pp = 0.0, tt = 0.0;
    for (int l = 0; l < kLanes; ++l)
    {
        d += dot[l];
        pp += a[l];
        tt += b[l];
    }
    if (pp <= 0.0 || tt <= 0.0)
        return 0.0f;
    return static_cast<float> (std::clamp (d / std::sqrt (pp * tt), -1.0, 1.0));
}

float similarity (const FeatureVector& p, const FeatureVector& t, Similarity kind) noexcept
{
    return setSimilarity (p.data(), t.data(), kind);
}

AddressWeights sanitiseWeights (const AddressWeights& weights) noexcept
{
    AddressWeights w {};
    float total = 0.0f;
    for (int s = 0; s < kNumSets; ++s)
    {
        const float x = weights[static_cast<size_t> (s)];
        w[static_cast<size_t> (s)] = std::isfinite (x) ? std::max (0.0f, x) : 0.0f;
        total += w[static_cast<size_t> (s)];
    }
    return total > 0.0f ? w : kSpectrumOnly;
}

int dominantSet (const AddressWeights& weights) noexcept
{
    int best = 0;
    for (int s = 1; s < kNumSets; ++s)
        if (weights[static_cast<size_t> (s)] > weights[static_cast<size_t> (best)])
            best = s;
    return best;
}

namespace {
// Combines per-set similarities: one weighted set is returned exactly.
template <typename SetSim>
float weightedSets (const AddressWeights& weights, SetSim&& setSim) noexcept
{
    double num = 0.0, den = 0.0;
    int used = 0, only = 0;
    for (int s = 0; s < kNumSets; ++s)
        if (weights[static_cast<size_t> (s)] > 0.0f)
        {
            ++used;
            only = s;
        }
    if (used == 0)
        return setSim (0);
    if (used == 1)
        return setSim (only);
    for (int s = 0; s < kNumSets; ++s)
    {
        const float w = weights[static_cast<size_t> (s)];
        if (w <= 0.0f)
            continue;
        num += static_cast<double> (w) * setSim (s);
        den += w;
    }
    return static_cast<float> (num / den);
}
} // namespace

float addressSimilarity (const FeatureVector& p, const FeatureVector& t, Similarity kind, const AddressWeights& weights,
                         FeatureFocus focus) noexcept
{
    return weightedSets (weights, [&] (int s) {
        if (s == 0 && focus != FeatureFocus::Full)
            return focusedSimilarity (p, t, kind, focus, 0, kSlots, false);
        return setSimilarity (p.data() + setOffset (s), t.data() + setOffset (s), kind);
    });
}

float addressPrefixSimilarity (const FeatureVector& p, const FeatureVector& t, Similarity kind,
                               const AddressWeights& weights, FeatureFocus focus, int slotBegin, int slotEnd,
                               bool renormalize) noexcept
{
    return weightedSets (weights, [&] (int s) {
        if (s == 0)
            return focusedSimilarity (p, t, kind, focus, slotBegin, slotEnd, renormalize);
        // The same stretch of the segment on this set's slots (a partly heard slot counts).
        const auto layout = kSetLayouts[static_cast<size_t> (s)];
        const int b = slotBegin * layout.slots / kSlots;
        const int e = std::max (b + 1, (slotEnd * layout.slots + kSlots - 1) / kSlots);
        return setPrefixSimilarity (p.data() + setOffset (s), t.data() + setOffset (s), layout.width, kind, b, e,
                                    renormalize);
    });
}

float activation (float s, float power) noexcept
{
    const float a = std::pow (std::abs (s), power);
    return s < 0.0f ? -a : a;
}

RetrievalResult finishRetrieval (const TraceStore& store, const RetrievalSettings& settings,
                                 EchoWeight* out, int n) noexcept
{
    RetrievalResult r;
    for (int i = 0; i < n; ++i)
    {
        auto& w = out[i];
        const auto& trace = store.slot (w.slot);
        float a = activation (w.activation, settings.power) * trace.strength;
        if (settings.habituation)
            a *= 1.0f - trace.fatigue;
        if (settings.recency > 0.0f)
        {
            const auto age = static_cast<float> (store.currentSerial() - std::min (store.currentSerial(), trace.serial + 1));
            a *= std::exp (-settings.recency * age / 4.0f);
        }
        if (settings.negativeMode == NegativeMode::Ignore)
            a = std::max (a, 0.0f);
        else if (settings.negativeMode == NegativeMode::Absolute)
            a = std::abs (a);
        w.activation = a;
        r.intensity += a;
        r.sumAbs += std::abs (a);
        r.maxAbs = std::max (r.maxAbs, std::abs (a));
    }

    if (r.maxAbs <= 0.0f)
        return r;

    switch (settings.normalization)
    {
        case Normalization::Sum:         r.gain = 1.0f / r.sumAbs; break;
        case Normalization::Max:         r.gain = 1.0f / r.maxAbs; break;
        case Normalization::Familiarity: r.gain = 1.0f / std::max (r.sumAbs, 1.0f); break;
    }

    // Drop negligible contributors so playback only mixes traces that matter.
    const float threshold = r.maxAbs * kNegligible;
    int kept = 0;
    for (int i = 0; i < n; ++i)
    {
        if (std::abs (out[i].activation) < threshold)
            continue;
        out[i].weight = out[i].activation * r.gain;
        out[kept++] = out[i];
    }
    r.numWeights = kept;
    return r;
}

RetrievalResult retrieve (const FeatureVector& probe, const TraceStore& store,
                          const RetrievalSettings& settings, EchoWeight* out) noexcept
{
    int n = 0;
    for (int i = 0; i < store.size(); ++i)
    {
        const int slot = store.storedSlot (i);
        if (slot == settings.excludeSlot)
            continue;
        const auto& t = store.slot (slot);
        auto half = [&settings] (const FeatureVector& p, const FeatureVector& f) {
            return addressSimilarity (p, f, settings.similarity, settings.address, settings.focus);
        };
        float s;
        if (settings.contextProbe == nullptr || settings.contextWeight <= 0.0f)
            s = half (probe, t.features);
        else
            s = combineHalves (half (*settings.contextProbe, t.context), settings.contextWeight,
                               settings.currentWeight > 0.0f ? half (probe, t.features) : 0.0f, settings.currentWeight);
        out[n++] = { slot, s, 0.0f, 0 };
    }
    return finishRetrieval (store, settings, out, n);
}

float prefixSimilarity (const FeatureVector& p, const FeatureVector& t, Similarity kind,
                        int slotBegin, int slotEnd, bool renormalize) noexcept
{
    return setPrefixSimilarity (p.data(), t.data(), kBands, kind, slotBegin, slotEnd, renormalize);
}

float setPrefixSimilarity (const float* p, const float* t, int width, Similarity kind,
                           int slotBegin, int slotEnd, bool renormalize) noexcept
{
    const int slots = kSetSize / std::max (1, width);
    const size_t j0 = static_cast<size_t> (std::clamp (slotBegin, 0, slots)) * static_cast<size_t> (width);
    const size_t j1 = static_cast<size_t> (std::clamp (slotEnd, 0, slots)) * static_cast<size_t> (width);
    if (j1 <= j0)
        return 0.0f;

    // Re-scale the trace over the range (its stored values were normalised
    // over the whole bar). Zeros are "unencoded" and stay zero.
    double mean = 0.0, scale = 1.0;
    if (renormalize)
    {
        double s = 0.0, sq = 0.0;
        int n = 0;
        for (size_t j = j0; j < j1; ++j)
            if (t[j] != 0.0f)
            {
                s += t[j];
                sq += static_cast<double> (t[j]) * t[j];
                ++n;
            }
        if (n > 1)
        {
            mean = s / n;
            const double sd = std::sqrt (std::max (0.0, sq / n - mean * mean));
            scale = sd > 1.0e-6 ? 1.0 / sd : 1.0;
        }
    }

    double dot = 0.0, pp = 0.0, tt = 0.0;
    int count = 0;
    for (size_t j = j0; j < j1; ++j)
    {
        const double tv = t[j] != 0.0f ? (t[j] - mean) * scale : 0.0;
        dot += p[j] * tv;
        pp += static_cast<double> (p[j]) * p[j];
        tt += tv * tv;
        count += (p[j] != 0.0f || tv != 0.0) ? 1 : 0;
    }
    double s;
    if (kind == Similarity::Hintzman)
        s = count > 0 ? dot / count : 0.0;
    else
        s = (pp > 0.0 && tt > 0.0) ? dot / std::sqrt (pp * tt) : 0.0;
    return static_cast<float> (std::clamp (s, -1.0, 1.0));
}

FeatureVector focusVector (const FeatureVector& v, FeatureFocus focus, int slotBegin, int slotEnd) noexcept
{
    if (focus == FeatureFocus::Full)
        return v;
    FeatureVector out {};
    slotBegin = std::clamp (slotBegin, 0, kSlots);
    slotEnd = std::clamp (slotEnd, slotBegin, kSlots);
    if (focus == FeatureFocus::Rhythm)
    {
        for (int s = slotBegin; s < slotEnd; ++s)
        {
            double sum = 0.0;
            int n = 0;
            for (int b = 0; b < kBands; ++b)
                if (const float x = v[static_cast<size_t> (s * kBands + b)]; x != 0.0f)
                {
                    sum += x;
                    ++n;
                }
            out[static_cast<size_t> (s * kBands)] = n > 0 ? static_cast<float> (sum / n) : 0.0f;
        }
    }
    else
    {
        for (int b = 0; b < kBands; ++b)
        {
            double sum = 0.0;
            int n = 0;
            for (int s = slotBegin; s < slotEnd; ++s)
                if (const float x = v[static_cast<size_t> (s * kBands + b)]; x != 0.0f)
                {
                    sum += x;
                    ++n;
                }
            out[static_cast<size_t> (b)] = n > 0 ? static_cast<float> (sum / n) : 0.0f;
        }
    }
    return out;
}

float focusedSimilarity (const FeatureVector& probe, const FeatureVector& trace, Similarity kind, FeatureFocus focus,
                         int slotBegin, int slotEnd, bool renormalize) noexcept
{
    if (focus == FeatureFocus::Full)
        return prefixSimilarity (probe, trace, kind, slotBegin, slotEnd, renormalize);
    const auto p = focusVector (probe, focus, slotBegin, slotEnd);
    const auto t = focusVector (trace, focus, slotBegin, slotEnd);
    // Rhythm keeps the slot range; timbre lives in slot 0.
    return focus == FeatureFocus::Rhythm ? prefixSimilarity (p, t, kind, slotBegin, slotEnd, true)
                                         : prefixSimilarity (p, t, kind, 0, 1, true);
}

void echoAddress (const EchoWeight* weights, int n, const TraceStore& store, FeatureMode mode,
                   float ternaryThreshold, FeatureVector& out, bool contextHalf) noexcept
{
    std::array<double, kAddressSize> acc {};
    double total = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const auto& t = store.slot (weights[i].slot);
        const auto& f = contextHalf ? t.context : t.features;
        const double w = weights[i].weight;
        for (size_t j = 0; j < acc.size(); ++j)
            acc[j] += w * f[j];
        total += std::abs (w);
    }
    out.fill (0.0f);
    if (total <= 0.0)
        return;

    // Each set is re-normalised on its own, like a stored address.
    for (int set = 0; set < kNumSets; ++set)
    {
        const auto j0 = static_cast<size_t> (setOffset (set)), j1 = j0 + kSetSize;
        double sum = 0.0, sq = 0.0;
        int count = 0;
        for (size_t j = j0; j < j1; ++j)
        {
            auto& a = acc[j];
            a /= total;
            if (a != 0.0)
            {
                sum += a;
                sq += a * a;
                ++count;
            }
        }
        if (count < 2)
            continue;
        const double mean = sum / count;
        const double sd = std::sqrt (std::max (1.0e-12, sq / count - mean * mean));
        for (size_t j = j0; j < j1; ++j)
        {
            if (acc[j] == 0.0)
                continue;
            auto z = static_cast<float> ((acc[j] - mean) / sd);
            if (mode == FeatureMode::Ternary)
                z = z > ternaryThreshold ? 1.0f : (z < -ternaryThreshold ? -1.0f : 0.0f);
            out[j] = z;
        }
    }
}

bool prepareWindowProbe (const float* const* frames, int numFrames, WindowProbe& out) noexcept
{
    out.frames = std::clamp (numFrames, 0, kMaxWindowFrames);
    const int n = out.frames * kBands;
    double sum = 0.0;
    for (int f = 0; f < out.frames; ++f)
        for (int b = 0; b < kBands; ++b)
        {
            out.v[static_cast<size_t> (f * kBands + b)] = frames[f][b];
            sum += frames[f][b];
        }
    if (n == 0)
        return false;
    const double mean = sum / n;
    double norm = 0.0;
    for (int j = 0; j < n; ++j)
    {
        auto& x = out.v[static_cast<size_t> (j)];
        x = static_cast<float> (x - mean);
        norm += static_cast<double> (x) * x;
    }
    if (norm < 1.0e-6)
        return false;
    const auto inv = static_cast<float> (1.0 / std::sqrt (norm));
    for (int j = 0; j < n; ++j)
        out.v[static_cast<size_t> (j)] *= inv;
    return true;
}

OffsetMatch bestOffset (const WindowProbe& probe, const TraceSlot& trace, int minContinuation) noexcept
{
    OffsetMatch best;
    const int W = probe.frames;
    const int last = trace.frameEnd - W - std::max (0, minContinuation); // inclusive
    if (W <= 0 || trace.frames == nullptr || last < trace.frameBegin)
        return best;

    const int n = W * kBands;
    auto frameSums = [&] (int f, double& s, double& sq) {
        const float* x = trace.frames + static_cast<size_t> (f) * kBands;
        for (int b = 0; b < kBands; ++b)
        {
            s += x[b];
            sq += static_cast<double> (x[b]) * x[b];
        }
    };

    // Running window sums make the normalisation O(1) per offset; the dot
    // product with the (zero-mean, unit) probe is the only O(W) part.
    double s = 0.0, sq = 0.0;
    for (int f = trace.frameBegin; f < trace.frameBegin + W; ++f)
        frameSums (f, s, sq);

    for (int o = trace.frameBegin; o <= last; ++o)
    {
        if (o > trace.frameBegin)
        {
            double rs = 0.0, rsq = 0.0, as = 0.0, asq = 0.0;
            frameSums (o - 1, rs, rsq);
            frameSums (o + W - 1, as, asq);
            s += as - rs;
            sq += asq - rsq;
        }
        const double var = sq - s * s / n;
        if (var <= 1.0e-9)
            continue;
        const float* t = trace.frames + static_cast<size_t> (o) * kBands;
        double dot = 0.0;
        for (int j = 0; j < n; ++j)
            dot += static_cast<double> (probe.v[static_cast<size_t> (j)]) * t[j];
        const auto r = static_cast<float> (dot / std::sqrt (var)); // probe is zero-mean: sum(p*(t-mean)) = sum(p*t)
        if (r > best.similarity)
            best = { o, std::clamp (r, -1.0f, 1.0f) };
    }
    return best;
}

BestMatch bestMatch (const FeatureVector& probe, const TraceStore& store, Similarity kind, bool unclampedOnly,
                     const AddressWeights& weights) noexcept
{
    BestMatch best;
    for (int i = 0; i < store.size(); ++i)
    {
        const auto& t = store.slot (store.storedSlot (i));
        if (unclampedOnly && t.clamped)
            continue;
        const float s = addressSimilarity (probe, t.features, kind, weights);
        if (s > best.similarity)
            best = { i, s };
    }
    return best;
}

} // namespace mse
