#include "mse/Import.h"

#include <algorithm>
#include <cmath>

namespace mse {

MemorySnapshot tracesFromAudio (const std::vector<std::vector<float>>& audio, double sampleRate,
                                double targetSampleRate, const ImportSettings& settings)
{
    MemorySnapshot snap;
    snap.sampleRate = sampleRate;
    snap.channels = std::clamp (static_cast<int> (audio.size()), 1, 2);
    if (audio.empty() || audio[0].empty() || sampleRate <= 0.0)
        return snap;

    // Cut at the source rate.
    const auto total = static_cast<int64_t> (audio[0].size());
    const auto traceLen = std::max<int64_t> (16, std::llround (settings.traceSeconds * sampleRate));
    for (int64_t start = 0; start < total && static_cast<int> (snap.traces.size()) < settings.maxTraces; start += traceLen)
    {
        const int64_t len = std::min (traceLen, total - start);
        if (start > 0 && static_cast<double> (len) < settings.minFraction * static_cast<double> (traceLen))
            break;
        TraceRecord t;
        t.nominalLength = static_cast<double> (traceLen);
        t.clamped = settings.clamp;
        t.audio.resize (static_cast<size_t> (snap.channels));
        for (int c = 0; c < snap.channels; ++c)
        {
            const auto& src = audio[static_cast<size_t> (c)];
            t.audio[static_cast<size_t> (c)].assign (src.begin() + start, src.begin() + start + len);
        }
        snap.traces.push_back (std::move (t));
    }
    resampleSnapshot (snap, targetSampleRate);

    // Addresses and levels at the engine's rate.
    FeatureExtractor fx;
    std::vector<float> mono;
    std::vector<TraceRecord> kept;
    for (auto& t : snap.traces)
    {
        const auto n = static_cast<size_t> (t.length());
        mono.assign (n, 0.0f);
        for (const auto& ch : t.audio)
            for (size_t i = 0; i < n; ++i)
                mono[i] += ch[i] / static_cast<float> (t.audio.size());
        double energy = 0.0;
        for (float x : mono)
            energy += static_cast<double> (x) * x;

        fx.prepare (snap.sampleRate);
        fx.beginSegment (t.nominalLength);
        fx.push (mono.data(), static_cast<int> (n), 0);
        if (! fx.finalize (t.features, settings.features))
            continue; // silent
        t.rms = n > 0 ? static_cast<float> (std::sqrt (energy / static_cast<double> (n))) : 0.0f;
        t.serial = kept.size() + 1;
        kept.push_back (std::move (t));
    }
    snap.traces = std::move (kept);
    return snap;
}

} // namespace mse
