#include "mse/Features.h"

#include <algorithm>
#include <cmath>

namespace mse {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kSilenceEnergy = 1.0e-10; // mean band energy below this (~-85 dBFS broadband) => silent
constexpr double kDynamicRangeDb = 60.0;   // bands more than this below the loudest are floored
constexpr double kMinStdDb = 3.0;          // keeps near-flat segments from blowing up noise
constexpr double kFftSeconds = 0.085;      // pitch analysis frame (~4096 samples at 48 kHz)
constexpr double kPitchMaxHz = 5000.0;     // spectrum above this doesn't feed the pitch sets
constexpr double kPitchMinHz = 50.0;
constexpr double kPresentDb = 20.0;        // pitch sets: semitones this far below the loudest count as absent
constexpr double kTimbreStartDb = 30.0;    // Timbre: the first peak is looked for from the first band this close to the loudest
constexpr int kTimbreBands = 16;           // Timbre: bands analysed from there
constexpr int kTimbreShapeCoeffs = 8;      // Timbre: cepstral coefficients used (c1..c8)
constexpr int kTimbreWidth = 2 * kTimbreCoeffs; // Timbre: cells per slot (shape, then brightness)
constexpr double kTimbrePlaceWidth = 1.5;  // Timbre: brightness bump width (cells)
constexpr double kTimbrePlacePerBand = 2.0; // Timbre: brightness cells per band of centroid

constexpr int kChromaSlots = kSetLayouts[static_cast<int> (AddressSet::PitchClass)].slots;
constexpr int kPitchSlots = kSetLayouts[static_cast<int> (AddressSet::Pitch)].slots;
static_assert (kChromaSlots == 32 && kPitchSlots == 8, "matches the extractor's accumulators");
static_assert (kSetLayouts[static_cast<int> (AddressSet::Rhythm)].slots == kRhythmSteps);
static_assert (kSetLayouts[static_cast<int> (AddressSet::Timbre)].width == kTimbreWidth);

// z-scores `n` cells (skipping unfilled ones, which stay 0 = unencoded) into
// `out`, optionally ternarised.
void normaliseCells (const double* v, const bool* filled, int n, double minSd, const FeatureSettings& settings,
                     float* out) noexcept
{
    double sum = 0.0, sq = 0.0;
    int count = 0;
    for (int i = 0; i < n; ++i)
        if (filled[i])
        {
            sum += v[i];
            sq += v[i] * v[i];
            ++count;
        }
    if (count == 0)
        return;
    const double mean = sum / count;
    const double sd = std::max (minSd, std::sqrt (std::max (0.0, sq / count - mean * mean)));
    for (int i = 0; i < n; ++i)
    {
        if (! filled[i])
            continue;
        float z = static_cast<float> ((v[i] - mean) / sd);
        if (settings.mode == FeatureMode::Ternary)
            z = z > settings.ternaryThreshold ? 1.0f : (z < -settings.ternaryThreshold ? -1.0f : 0.0f);
        out[i] = z;
    }
}

// Energies -> dB, floored kDynamicRangeDb below the loudest filled cell.
void toFlooredDb (double* v, const bool* filled, int n) noexcept
{
    double maxDb = -1.0e9;
    for (int i = 0; i < n; ++i)
        if (filled[i])
        {
            v[i] = 10.0 * std::log10 (v[i] + 1.0e-20);
            maxDb = std::max (maxDb, v[i]);
        }
    for (int i = 0; i < n; ++i)
        if (filled[i])
            v[i] = std::max (v[i], maxDb - kDynamicRangeDb);
}
} // namespace

const char* addressSetName (int set) noexcept
{
    static const char* const names[kNumSets] = { "Spectrum", "Pitch Class", "Pitch", "Timbre", "Rhythm" };
    return set >= 0 && set < kNumSets ? names[set] : "";
}

int frameHop (double sampleRate) noexcept
{
    return std::max (16, static_cast<int> (std::lround (kFrameSeconds * sampleRate)));
}

void FeatureExtractor::prepare (double sampleRate, bool addressSets)
{
    sets = addressSets;
    if (sets)
    {
        fftSize = 256;
        while (fftSize * 2 <= static_cast<int> (std::lround (kFftSeconds * sampleRate * 1.4)))
            fftSize *= 2;
        fftHop = fftSize / 4;
        fft.prepare (fftSize);
        window.resize (static_cast<size_t> (fftSize));
        for (int i = 0; i < fftSize; ++i)
            window[static_cast<size_t> (i)] = static_cast<float> (0.5 - 0.5 * std::cos (2.0 * kPi * i / fftSize));
        fftRing.assign (static_cast<size_t> (fftSize), 0.0f);
        fftRe.assign (static_cast<size_t> (fftSize), 0.0f);
        fftIm.assign (static_cast<size_t> (fftSize), 0.0f);

        const double binHz = sampleRate / fftSize;
        binLow = std::max (1, static_cast<int> (std::ceil (kPitchMinHz / binHz)));
        sampleRateHz = sampleRate;
        binHigh = std::min (fftSize / 2 - 2, static_cast<int> (std::floor (std::min (kPitchMaxHz, 0.45 * sampleRate) / binHz)));
        fftFill = fftPos = 0;
    }

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
    nominal = std::max (1.0, nominalLength);
    slotScale = kSlots / nominal;
    rhythmEnergy.fill (0.0);
    rhythmCounts.fill (0);
    rhythmScale = kRhythmSteps / nominal;
    chromaEnergy.fill (0.0);
    chromaCounts.fill (0);
    pitchEnergy.fill (0.0);
    pitchCounts.fill (0);
    if (sets)
    {
        // Pitch frames see only this segment (as when a trace is analysed on
        // its own after loading or import).
        std::fill (fftRing.begin(), fftRing.end(), 0.0f);
        fftFill = fftPos = 0;
    }

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
        const auto step = static_cast<size_t> (std::min<int64_t> (
            kRhythmSteps - 1, static_cast<int64_t> (static_cast<double> (startIndex + i) * rhythmScale)));
        double* r = rhythmEnergy.data() + step * kRhythmGroups;
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
            r[b * kRhythmGroups / kBands] += p;
        }
        ++counts[slot];
        ++rhythmCounts[step];
        if (sets)
        {
            fftRing[static_cast<size_t> (fftPos)] = x;
            fftPos = fftPos + 1 == fftSize ? 0 : fftPos + 1;
            if (++fftFill == fftHop)
            {
                fftFill = 0;
                analyseFrame (startIndex + i + 1);
            }
        }
        framePower += static_cast<double> (x) * x;
        ++streamSamples;
        if (++frameCount == frameHopSamples)
            emitFrame (true);
    }
}

void FeatureExtractor::analyseFrame (int64_t endPosition) noexcept
{
    // The frame counts toward the slot its centre falls in (frames centred
    // before the segment began belong to the previous one).
    const double centre = static_cast<double> (endPosition) - 0.5 * fftSize;
    if (centre < 0.0)
        return;
    for (int i = 0; i < fftSize; ++i)
    {
        const int j = fftPos + i < fftSize ? fftPos + i : fftPos + i - fftSize; // oldest first
        fftRe[static_cast<size_t> (i)] = fftRing[static_cast<size_t> (j)] * window[static_cast<size_t> (i)];
        fftIm[static_cast<size_t> (i)] = 0.0f;
    }
    fft.forward (fftRe.data(), fftIm.data());

    // Spectral peaks only (local maxima, their frequency refined by parabolic
    // interpolation of the log magnitude), each given to its nearest
    // semitone (shared with the next only when it lies between them):
    // partials land on sharp semitones instead of smearing across the
    // window's main lobe.
    std::array<double, 128> semis {};
    const double binHz = sampleRateHz / fftSize;
    auto power = [this] (int k) {
        const auto kk = static_cast<size_t> (k);
        return static_cast<double> (fftRe[kk]) * fftRe[kk] + static_cast<double> (fftIm[kk]) * fftIm[kk];
    };
    double prev = power (binLow - 1), cur = power (binLow);
    for (int k = binLow; k <= binHigh; ++k)
    {
        const double next = power (k + 1);
        if (cur > prev && cur >= next && cur > 1.0e-12)
        {
            const double a = std::log (prev + 1.0e-30), b = std::log (cur), c = std::log (next + 1.0e-30);
            const double den = a - 2.0 * b + c;
            const double offset = den < 0.0 ? std::clamp (0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
            const double hz = (k + offset) * binHz;
            const double m = 12.0 * std::log2 (hz / 440.0) + 69.0;
            const double lo = std::floor (m);
            // Within 0.3 semitone of a semitone: all of it; in between: shared.
            const double w = std::clamp ((m - lo - 0.3) / 0.4, 0.0, 1.0);
            const int mi = static_cast<int> (lo);
            const double p = prev + cur + next; // the peak's power
            if (mi >= 0 && mi < 127)
            {
                semis[static_cast<size_t> (mi)] += p * (1.0 - w);
                semis[static_cast<size_t> (mi + 1)] += p * w;
            }
        }
        prev = cur;
        cur = next;
    }

    const auto cs = static_cast<size_t> (std::min (kChromaSlots - 1, static_cast<int> (centre * kChromaSlots / nominal)));
    const auto ps = static_cast<size_t> (std::min (kPitchSlots - 1, static_cast<int> (centre * kPitchSlots / nominal)));
    for (int m = 0; m < 128; ++m)
    {
        const double p = semis[static_cast<size_t> (m)];
        if (p <= 0.0)
            continue;
        chromaEnergy[cs * 12 + static_cast<size_t> (m % 12)] += p;
        // Pitch: C2..B5. Lower partials fold up by octaves; higher ones are
        // left out (folded down, every note's upper partials would fill the
        // top octave and make all notes look alike).
        int f = m;
        while (f < kPitchLow)
            f += 12;
        if (f < kPitchLow + kPitchCount)
            pitchEnergy[ps * kPitchCount + static_cast<size_t> (f - kPitchLow)] += p;
    }
    ++chromaCounts[cs];
    ++pitchCounts[ps];
}

void FeatureExtractor::computeFrames (const float* mono, int64_t begin, int64_t end, double sampleRate,
                                      float* sinkFrames, int capacity, int& framesBegin, int& framesEnd)
{
    FeatureExtractor fx;
    fx.prepare (sampleRate, false);
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
    std::array<double, kSetSize> db {};
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
    std::array<bool, kSetSize> filled {};
    for (int s = 0; s < kSlots; ++s)
    {
        if (counts[static_cast<size_t> (s)] == 0)
            continue;
        for (int b = 0; b < kBands; ++b)
        {
            const auto i = static_cast<size_t> (s * kBands + b);
            db[i] = std::max (db[i], floorDb);
            filled[i] = true;
        }
    }
    normaliseCells (db.data(), filled.data(), kSetSize, kMinStdDb, settings, out.data());
    if (! sets)
        return true;

    // Timbre: the band spectrum's shape per slot, as cepstral coefficients
    // c1..c12 (DCT-II of the dB levels; c0, the level, is left out). The
    // spectrum is first aligned to its first peak (a harmonic sound's
    // fundamental), so transposing a sound moves it along the axis instead
    // of changing its shape.
    {
        std::array<double, kBands> mean {};
        int filledSlots = 0;
        for (int s = 0; s < kSlots; ++s)
        {
            if (counts[static_cast<size_t> (s)] == 0)
                continue;
            ++filledSlots;
            for (int b = 0; b < kBands; ++b)
                mean[static_cast<size_t> (b)] += db[static_cast<size_t> (s * kBands + b)];
        }
        double loudest = -1.0e9;
        for (auto& m : mean)
        {
            m /= std::max (1, filledSlots);
            loudest = std::max (loudest, m);
        }
        // The first peak (for a harmonic sound, the fundamental's band), refined
        // to a fraction of a band; without one, the first strong band.
        int first = 0;
        while (first < kBands - 1 && mean[static_cast<size_t> (first)] < loudest - kTimbreStartDb)
            ++first;
        int peak = first;
        while (peak < kBands - 1 && mean[static_cast<size_t> (peak + 1)] > mean[static_cast<size_t> (peak)])
            ++peak;
        double start = peak;
        if (peak > 0 && peak < kBands - 1)
        {
            const double a = mean[static_cast<size_t> (peak - 1)], b = mean[static_cast<size_t> (peak)],
                         c = mean[static_cast<size_t> (peak + 1)];
            const double den = a - 2.0 * b + c;
            if (den < 0.0)
                start += std::clamp (0.5 * (a - c) / den, -0.5, 0.5);
        }
        auto level = [&] (int s, double x) { // band level at fractional position x (clamped to the ends)
            x = std::clamp (x, 0.0, static_cast<double> (kBands - 1));
            const int i = std::min (kBands - 2, static_cast<int> (x));
            const double w = x - i;
            const double* row = db.data() + s * kBands;
            return row[i] * (1.0 - w) + row[i + 1] * w;
        };

        // Per slot, cells [0, kTimbreCoeffs): the shape, as cepstral
        // coefficients (mildly liftered, c1..c8 weighted by sqrt(c); higher ones
        // alias with the harmonics' positions and would leak pitch).
        // Cells [kTimbreCoeffs, 2 kTimbreCoeffs): brightness, place-coded: a
        // bump at the spectral centroid (in bands above the fundamental), so
        // a brighter or darker sound moves to other cells. z-scoring alone
        // would make any two spectral tilts look alike.
        std::array<double, kSetSize> v {};
        std::array<bool, kSetSize> f {};
        float* dst = out.data() + setOffset (static_cast<int> (AddressSet::Timbre));
        std::array<float, kSetSize> place {};
        for (int s = 0; s < kSlots; ++s)
        {
            if (counts[static_cast<size_t> (s)] == 0)
                continue;
            for (int c = 1; c <= kTimbreCoeffs; ++c)
            {
                double acc = 0.0;
                if (c <= kTimbreShapeCoeffs)
                    for (int j = 0; j < kTimbreBands; ++j)
                        acc += level (s, start + j) * std::cos (kPi * c * (j + 0.5) / kTimbreBands);
                const auto i = static_cast<size_t> (s * kTimbreWidth + c - 1);
                v[i] = acc * std::sqrt (static_cast<double> (c));
                f[i] = c <= kTimbreShapeCoeffs;
            }
            // Centroid of the aligned spectrum's power, in bands above the fundamental.
            double num = 0.0, den = 0.0;
            for (int j = 0; j < kTimbreBands; ++j)
            {
                const double p = std::pow (10.0, level (s, start + j) / 10.0);
                num += p * j;
                den += p;
            }
            const double centroid = den > 0.0 ? num / den : 0.0;
            const double pos = std::min (static_cast<double> (kTimbreCoeffs - 1), centroid * kTimbrePlacePerBand);
            double sq = 0.0;
            int cells = 0;
            for (int k = 0; k < kTimbreCoeffs; ++k)
            {
                const double d = (k - pos) / kTimbrePlaceWidth;
                const double bump = std::exp (-0.5 * d * d);
                if (bump > 0.05)
                {
                    place[static_cast<size_t> (s * kTimbreWidth + kTimbreCoeffs + k)] = static_cast<float> (bump);
                    sq += bump * bump;
                    ++cells;
                }
            }
            // Unit RMS over the bump, like the z-scored shape cells.
            const auto gain = static_cast<float> (cells > 0 ? 1.0 / std::sqrt (sq / cells) : 0.0);
            for (int k = 0; k < kTimbreCoeffs; ++k)
                place[static_cast<size_t> (s * kTimbreWidth + kTimbreCoeffs + k)] *= gain;

        }
        normaliseCells (v.data(), f.data(), kSetSize, kMinStdDb, settings, dst);
        for (size_t i = 0; i < place.size(); ++i)
            if (place[i] != 0.0f)
                dst[i] = settings.mode == FeatureMode::Ternary ? (place[i] > 0.5f ? 1.0f : 0.0f) : place[i];
    }

    // Pitch class and pitch: semitone energies per slot, in dB. Only the
    // semitones that are present (within kPresentDb of the loudest) are
    // encoded, as their level above that threshold scaled to unit RMS; the
    // rest are 0 (unencoded), so notes that share no partials don't look
    // alike through a shared noise floor (z-scoring would give every absent
    // semitone the same negative value).
    auto pitchSet = [&] (const double* energyCells, const int* slotCounts, int slots, int width, AddressSet set) {
        std::array<double, kSetSize> v {};
        std::array<bool, kSetSize> f {};
        for (int s = 0; s < slots; ++s)
        {
            const int c = slotCounts[s];
            if (c == 0)
                continue;
            for (int k = 0; k < width; ++k)
            {
                const auto i = static_cast<size_t> (s * width + k);
                v[i] = energyCells[i] / c;
                f[i] = true;
            }
        }
        toFlooredDb (v.data(), f.data(), slots * width);
        float* dst = out.data() + setOffset (static_cast<int> (set));
        double maxDb = -1.0e9;
        for (int i = 0; i < slots * width; ++i)
            if (f[static_cast<size_t> (i)])
                maxDb = std::max (maxDb, v[static_cast<size_t> (i)]);
        const double threshold = maxDb - kPresentDb;
        double sq = 0.0;
        int n = 0;
        for (int i = 0; i < slots * width; ++i)
            if (f[static_cast<size_t> (i)])
            {
                const double x = std::max (0.0, v[static_cast<size_t> (i)] - threshold);
                v[static_cast<size_t> (i)] = x;
                sq += x * x;
                n += x > 0.0 ? 1 : 0;
            }
        if (n == 0 || sq <= 0.0)
            return;
        const double scale = 1.0 / std::sqrt (sq / n);
        for (int i = 0; i < slots * width; ++i)
        {
            const double x = v[static_cast<size_t> (i)];
            if (x <= 0.0)
                continue;
            float z = static_cast<float> (x * scale);
            if (settings.mode == FeatureMode::Ternary)
                z = z > settings.ternaryThreshold ? 1.0f : 0.0f;
            dst[i] = z;
        }
    };
    pitchSet (chromaEnergy.data(), chromaCounts.data(), kChromaSlots, 12, AddressSet::PitchClass);
    pitchSet (pitchEnergy.data(), pitchCounts.data(), kPitchSlots, kPitchCount, AddressSet::Pitch);

    // Rhythm: onset strength (rise in level, dB) per step in 4 frequency regions.
    {
        constexpr int n = kRhythmSteps * kRhythmGroups;
        std::array<double, n> lv {};
        std::array<bool, n> lf {};
        for (int t = 0; t < kRhythmSteps; ++t)
        {
            const auto c = rhythmCounts[static_cast<size_t> (t)];
            if (c == 0)
                continue;
            for (int g = 0; g < kRhythmGroups; ++g)
            {
                const auto i = static_cast<size_t> (t * kRhythmGroups + g);
                lv[i] = rhythmEnergy[i] / static_cast<double> (c);
                lf[i] = true;
            }
        }
        toFlooredDb (lv.data(), lf.data(), n);
        std::array<double, n> v {};
        std::array<bool, n> f {};
        for (int t = 0; t < kRhythmSteps; ++t)
            for (int g = 0; g < kRhythmGroups; ++g)
            {
                const auto i = static_cast<size_t> (t * kRhythmGroups + g);
                if (! lf[i])
                    continue;
                f[i] = true;
                v[i] = t > 0 && lf[i - kRhythmGroups] ? std::max (0.0, lv[i] - lv[i - kRhythmGroups]) : 0.0;
            }
        normaliseCells (v.data(), f.data(), n, 1.0, settings, out.data() + setOffset (static_cast<int> (AddressSet::Rhythm)));
    }
    return true;
}

} // namespace mse
