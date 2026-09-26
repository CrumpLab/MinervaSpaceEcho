#include "mse/Retrieval.h"

#include <algorithm>
#include <cmath>

namespace mse {

namespace {
constexpr float kNegligible = 1.0e-5f; // relative to the strongest activation
}

float similarity (const FeatureVector& p, const FeatureVector& t, Similarity kind) noexcept
{
    double dot = 0.0;
    if (kind == Similarity::Hintzman)
    {
        int n = 0;
        for (size_t j = 0; j < p.size(); ++j)
        {
            dot += static_cast<double> (p[j]) * t[j];
            n += (p[j] != 0.0f || t[j] != 0.0f) ? 1 : 0;
        }
        if (n == 0)
            return 0.0f;
        return static_cast<float> (std::clamp (dot / n, -1.0, 1.0));
    }

    double pp = 0.0, tt = 0.0;
    for (size_t j = 0; j < p.size(); ++j)
    {
        dot += static_cast<double> (p[j]) * t[j];
        pp += static_cast<double> (p[j]) * p[j];
        tt += static_cast<double> (t[j]) * t[j];
    }
    if (pp <= 0.0 || tt <= 0.0)
        return 0.0f;
    return static_cast<float> (std::clamp (dot / std::sqrt (pp * tt), -1.0, 1.0));
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
        float a = activation (w.activation, settings.power) * store.slot (w.slot).strength;
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
        if (slot != settings.excludeSlot)
            out[n++] = { slot, similarity (probe, store.slot (slot).features, settings.similarity), 0.0f, 0 };
    }
    return finishRetrieval (store, settings, out, n);
}

float prefixSimilarity (const FeatureVector& p, const FeatureVector& t, Similarity kind,
                        int slotBegin, int slotEnd, bool renormalize) noexcept
{
    const size_t j0 = static_cast<size_t> (std::clamp (slotBegin, 0, kSlots)) * kBands;
    const size_t j1 = static_cast<size_t> (std::clamp (slotEnd, 0, kSlots)) * kBands;
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

BestMatch bestMatch (const FeatureVector& probe, const TraceStore& store, Similarity kind, bool unclampedOnly) noexcept
{
    BestMatch best;
    for (int i = 0; i < store.size(); ++i)
    {
        const auto& t = store.slot (store.storedSlot (i));
        if (unclampedOnly && t.clamped)
            continue;
        const float s = similarity (probe, t.features, kind);
        if (s > best.similarity)
            best = { i, s };
    }
    return best;
}

} // namespace mse
