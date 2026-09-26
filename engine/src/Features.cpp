#include "mse/Features.h"

#include <algorithm>
#include <cmath>

namespace mse {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kSilenceEnergy = 1.0e-10; // mean band energy below this (~-85 dBFS broadband) => silent
constexpr double kDynamicRangeDb = 60.0;   // bands more than this below the loudest are floored
constexpr double kMinStdDb = 3.0;          // keeps near-flat segments from blowing up noise
} // namespace

int frameHop (double sampleRate) noexcept
{
    return std::max (16, static_cast<int> (std::lround (kFrameSeconds * sampleRate)));
}

void FeatureExtractor::prepare (double sampleRate)
{
    const double fMin = 60.0;
    const double fMax = std::min (16000.0, 0.45 * sampleRate);
    const double ratio = std::pow (fMax / fMin, 1.0 / (kBands - 1));
    const double bwOct = std::log2 (ratio);
    const double q = std::sqrt (std::pow (2.0, bwOct)) / (std::pow (2.0, bwOct) - 1.0);

    for (int b = 0; b < kBands; ++b)
    {
        const double fc = fMin * std::pow (ratio, b);
        centres[static_cast<size_t> (b)] = static_cast<float> (fc);

        // RBJ band-pass, constant 0 dB peak gain.
        const double w0 = 2.0 * kPi * fc / sampleRate;
        const double alpha = std::sin (w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        auto& f = filters[static_cast<size_t> (b)];
        f.b0 = static_cast<float> (alpha / a0);
        f.b2 = static_cast<float> (-alpha / a0);
        f.a1 = static_cast<float> (-2.0 * std::cos (w0) / a0);
        f.a2 = static_cast<float> ((1.0 - alpha) / a0);
        f.z1 = f.z2 = 0.0f;
    }
    frameHopSamples = frameHop (sampleRate);
    produced = 0;
    streamSamples = 0;
    frameCount = 0;
    beginSegment (sampleRate);
}

void FeatureExtractor::beginSegment (double nominalLength, float* frameSink, int sinkFrames) noexcept
{
    energy.fill (0.0);
    counts.fill (0);
    slotScale = kSlots / std::max (1.0, nominalLength);

    // A partial frame from the previous segment still feeds the live ring.
    if (frameCount >= frameHopSamples / 2)
        emitFrame (false);
    frameAcc.fill (0.0);
    framePower = 0.0;
    frameCount = 0;
    sink = frameSink;
    sinkCapacity = frameSink ? sinkFrames : 0;
    sinkFirst = sinkLast = 0;
}

void FeatureExtractor::emitFrame (bool toSink) noexcept
{
    const double n = std::max (1, frameCount);
    float* dst = ring.data() + static_cast<size_t> (produced % kFrameRing) * kBands;
    for (size_t b = 0; b < static_cast<size_t> (kBands); ++b)
        dst[b] = static_cast<float> (10.0 * std::log10 (frameAcc[b] / n + 1.0e-12));
    ringPower[static_cast<size_t> (produced % kFrameRing)] = static_cast<float> (framePower / n);
    ringEndSample[static_cast<size_t> (produced % kFrameRing)] = streamSamples;
    ++produced;

    if (toSink && sink != nullptr)
    {
        const auto f = static_cast<int> (frameStartPos / frameHopSamples);
        if (f >= 0 && f < sinkCapacity)
        {
            std::copy (dst, dst + kBands, sink + static_cast<size_t> (f) * kBands);
            if (sinkLast == 0)
                sinkFirst = f;
            sinkLast = f + 1;
        }
    }
    frameAcc.fill (0.0);
    framePower = 0.0;
    frameCount = 0;
}

void FeatureExtractor::push (const float* mono, int n, int64_t startIndex) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const auto slot = static_cast<size_t> (std::min<int64_t> (
            kSlots - 1, static_cast<int64_t> (static_cast<double> (startIndex + i) * slotScale)));
        const float x = mono[i];
        if (frameCount == 0)
            frameStartPos = startIndex + i;
        double* e = energy.data() + slot * kBands;
        for (size_t b = 0; b < static_cast<size_t> (kBands); ++b)
        {
            auto& f = filters[b];
            // Transposed direct form II.
            const float y = f.b0 * x + f.z1;
            f.z1 = -f.a1 * y + f.z2;
            f.z2 = f.b2 * x - f.a2 * y;
            const double p = static_cast<double> (y) * y;
            e[b] += p;
            frameAcc[b] += p;
        }
        ++counts[slot];
        framePower += static_cast<double> (x) * x;
        ++streamSamples;
        if (++frameCount == frameHopSamples)
            emitFrame (true);
    }
}

void FeatureExtractor::computeFrames (const float* mono, int64_t begin, int64_t end, double sampleRate,
                                      float* sinkFrames, int capacity, int& framesBegin, int& framesEnd)
{
    FeatureExtractor fx;
    fx.prepare (sampleRate);
    fx.beginSegment (static_cast<double> (std::max<int64_t> (1, end)), sinkFrames, capacity);
    if (end > begin)
        fx.push (mono + begin, static_cast<int> (end - begin), begin);
    framesBegin = fx.sinkBegin();
    framesEnd = fx.sinkEnd();
}

bool FeatureExtractor::finalize (FeatureVector& out, const FeatureSettings& settings) const noexcept
{
    out.fill (0.0f);

    // Mean energy per filled cell, and overall loudness.
    std::array<double, kFeatureSize> db {};
    double total = 0.0, maxDb = -1.0e9;
    int filledSlots = 0;
    for (int s = 0; s < kSlots; ++s)
    {
        const auto c = counts[static_cast<size_t> (s)];
        if (c == 0)
            continue;
        ++filledSlots;
        for (int b = 0; b < kBands; ++b)
        {
            const auto i = static_cast<size_t> (s * kBands + b);
            const double e = energy[i] / static_cast<double> (c);
            total += e;
            db[i] = 10.0 * std::log10 (e + 1.0e-20);
            maxDb = std::max (maxDb, db[i]);
        }
    }
    if (filledSlots == 0 || total / (filledSlots * kBands) < kSilenceEnergy)
        return false;

    // Floor, then z-score over filled cells: keeps spectral / temporal shape,
    // discards absolute level.
    const double floorDb = maxDb - kDynamicRangeDb;
    double sum = 0.0, sumSq = 0.0;
    const int n = filledSlots * kBands;
    for (int s = 0; s < kSlots; ++s)
    {
        if (counts[static_cast<size_t> (s)] == 0)
            continue;
        for (int b = 0; b < kBands; ++b)
        {
            auto& d = db[static_cast<size_t> (s * kBands + b)];
            d = std::max (d, floorDb);
            sum += d;
            sumSq += d * d;
        }
    }
    const double mean = sum / n;
    const double sd = std::max (kMinStdDb, std::sqrt (std::max (0.0, sumSq / n - mean * mean)));

    for (int s = 0; s < kSlots; ++s)
    {
        if (counts[static_cast<size_t> (s)] == 0)
            continue;
        for (int b = 0; b < kBands; ++b)
        {
            const auto i = static_cast<size_t> (s * kBands + b);
            float z = static_cast<float> ((db[i] - mean) / sd);
            if (settings.mode == FeatureMode::Ternary)
                z = z > settings.ternaryThreshold ? 1.0f : (z < -settings.ternaryThreshold ? -1.0f : 0.0f);
            out[i] = z;
        }
    }
    return true;
}

} // namespace mse
