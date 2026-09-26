#pragma once

#include "TestSignals.h"
#include "mse/Wav.h"

#include "mse/EchoEngine.h"

#include <cmath>
#include <vector>

namespace mse::test {

struct RunOptions
{
    int block = 512;
    double bpm = 120.0;
    double startPpq = 0.0;
};

// Runs `audio` through `engine` in blocks with a steadily advancing host clock.
inline AudioBuffer run (EchoEngine& engine, AudioBuffer audio, const RunOptions& o = {})
{
    const int n = audio.numSamples();
    const int ch = audio.numChannels();
    HostClock clock;
    clock.hasTempo = true;
    clock.bpm = o.bpm;
    clock.isPlaying = true;
    std::vector<float*> ptrs (static_cast<size_t> (ch));
    for (int pos = 0; pos < n; pos += o.block)
    {
        for (int c = 0; c < ch; ++c)
            ptrs[static_cast<size_t> (c)] = audio.channels[static_cast<size_t> (c)].data() + pos;
        clock.ppqPosition = o.startPpq + pos / audio.sampleRate * o.bpm / 60.0;
        engine.process (ptrs.data(), ch, std::min (o.block, n - pos), clock);
    }
    return audio;
}

inline void prepare (EchoEngine& engine, const EngineParams& p, int capacity, double sr = 48000.0,
                     int block = 512, int channels = 2)
{
    MemoryConfig m;
    m.capacity = capacity;
    engine.setMemoryConfig (m);
    engine.setParams (p);
    engine.prepare (sr, block, channels);
}

// Echo-only settings: dry off, echo at unity, no fades, no feedback.
inline EngineParams wetOnly()
{
    EngineParams p;
    p.dryLevelDb = kLevelOffDb;
    p.echoLevelDb = 0.0f;
    p.edgeFadeMs = 0.0f;
    return p;
}

inline AudioBuffer silence (int channels, int samples, double sr = 48000.0)
{
    AudioBuffer b;
    b.sampleRate = sr;
    b.resize (channels, samples);
    return b;
}

inline AudioBuffer slice (const AudioBuffer& b, int start, int len)
{
    auto out = silence (b.numChannels(), len, b.sampleRate);
    for (size_t c = 0; c < b.channels.size(); ++c)
        for (int i = 0; i < len && start + i < b.numSamples(); ++i)
            out.channels[c][static_cast<size_t> (i)] = b.channels[c][static_cast<size_t> (start + i)];
    return out;
}

inline void append (AudioBuffer& dst, const AudioBuffer& src)
{
    if (dst.channels.empty())
        dst.channels.resize (src.channels.size());
    for (size_t c = 0; c < src.channels.size(); ++c)
        dst.channels[c].insert (dst.channels[c].end(), src.channels[c].begin(), src.channels[c].end());
    dst.sampleRate = src.sampleRate;
}

// Pearson correlation of channel 0.
inline double correlation (const AudioBuffer& a, const AudioBuffer& b)
{
    const auto& x = a.channels[0];
    const auto& y = b.channels[0];
    const size_t n = std::min (x.size(), y.size());
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    for (size_t i = 0; i < n; ++i)
    {
        sx += x[i]; sy += y[i];
        sxx += static_cast<double> (x[i]) * x[i];
        syy += static_cast<double> (y[i]) * y[i];
        sxy += static_cast<double> (x[i]) * y[i];
    }
    const double cov = sxy - sx * sy / n;
    const double vx = sxx - sx * sx / n, vy = syy - sy * sy / n;
    return (vx <= 0 || vy <= 0) ? 0.0 : cov / std::sqrt (vx * vy);
}

inline double rms (const AudioBuffer& a)
{
    double s = 0;
    size_t n = 0;
    for (const auto& ch : a.channels)
        for (float x : ch) { s += static_cast<double> (x) * x; ++n; }
    return n ? std::sqrt (s / n) : 0.0;
}

} // namespace mse::test
