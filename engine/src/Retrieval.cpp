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

RetrievalResult retrieve (const FeatureVector& probe, const TraceStore& store,
                          const RetrievalSettings& settings, EchoWeight* out) noexcept
{
    RetrievalResult r;
    int n = 0;
    for (int i = 0; i < store.size(); ++i)
    {
        const int slot = store.storedSlot (i);
        if (slot == settings.excludeSlot)
            continue;

        float a = activation (similarity (probe, store.slot (slot).features, settings.similarity), settings.power);
        if (settings.negativeMode == NegativeMode::Ignore)
            a = std::max (a, 0.0f);
        else if (settings.negativeMode == NegativeMode::Absolute)
            a = std::abs (a);

        r.intensity += a;
        r.sumAbs += std::abs (a);
        r.maxAbs = std::max (r.maxAbs, std::abs (a));
        out[n++] = { slot, a, 0.0f };
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

} // namespace mse
