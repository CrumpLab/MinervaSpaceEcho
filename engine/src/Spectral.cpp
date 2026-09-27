#include "mse/Spectral.h"

#include <algorithm>
#include <cmath>

namespace mse {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

float princarg (double phase) noexcept
{
    return static_cast<float> (phase - kTwoPi * std::floor ((phase + kPi) / kTwoPi));
}
} // namespace

// ---- Fft ---------------------------------------------------------------------

void Fft::prepare (int size)
{
    n = size;
    int bits = 0;
    while ((1 << bits) < n)
        ++bits;
    bitrev.resize (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            r |= ((i >> b) & 1) << (bits - 1 - b);
        bitrev[static_cast<size_t> (i)] = r;
    }
    cosT.resize (static_cast<size_t> (n / 2));
    sinT.resize (static_cast<size_t> (n / 2));
    for (int i = 0; i < n / 2; ++i)
    {
        cosT[static_cast<size_t> (i)] = static_cast<float> (std::cos (kTwoPi * i / n));
        sinT[static_cast<size_t> (i)] = static_cast<float> (std::sin (kTwoPi * i / n));
    }
}

void Fft::transform (float* re, float* im, bool inverse) const noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const int j = bitrev[static_cast<size_t> (i)];
        if (j > i)
        {
            std::swap (re[i], re[j]);
            std::swap (im[i], im[j]);
        }
    }
    const float sign = inverse ? 1.0f : -1.0f;
    for (int len = 2; len <= n; len <<= 1)
    {
        const int half = len / 2, step = n / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < half; ++k)
            {
                const float wr = cosT[static_cast<size_t> (k * step)];
                const float wi = sign * sinT[static_cast<size_t> (k * step)];
                const int a = i + k, b = a + half;
                const float xr = re[b] * wr - im[b] * wi;
                const float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
    }
}

// ---- SpectralRenderer -----------------------------------------------------------

void SpectralRenderer::prepare (double sampleRate, int /*maxChannels*/)
{
    // ~43 ms frames at any sample rate (2048 at 48 kHz).
    int size = 256;
    while (size < 0.04 * sampleRate)
        size <<= 1;
    fft.prepare (size);
    const auto n = static_cast<size_t> (size);
    window.resize (n);
    for (size_t i = 0; i < n; ++i)
        window[i] = static_cast<float> (std::sin (kPi * static_cast<double> (i) / static_cast<double> (n)));
    for (auto* v : { &re, &im, &re2, &im2 })
        v->assign (n, 0.0f);
    const size_t bins = n / 2 + 1;
    for (int c = 0; c < 2; ++c)
    {
        for (auto* v : { &mag[c], &bestMag[c], &bestPhase[c], &bestFreq[c], &outPhase[c], &frozenMag[c], &binFreq[c] })
            v->assign (bins, 0.0f);
        bestStretched[c].assign (bins, 0);
    }
    reset();
}

void SpectralRenderer::reset() noexcept
{
    for (auto& v : outPhase)
        std::fill (v.begin(), v.end(), 0.0f);
    haveFrozen = { false, false };
}

void SpectralRenderer::readFrame (const SpectralSource& s, int channels, double start) noexcept
{
    const int n = fft.size();
    const auto base = static_cast<int64_t> (std::floor (start));
    const auto frac = static_cast<float> (start - static_cast<double> (base));
    for (int i = 0; i < n; ++i)
    {
        const int64_t t = base + i;
        float l = 0.0f, r = 0.0f;
        if (t >= s.begin && t < s.end)
        {
            const bool next = frac != 0.0f && t + 1 < s.end;
            l = s.audio[0][t];
            if (next)
                l += frac * (s.audio[0][t + 1] - l);
            if (channels > 1)
            {
                r = s.audio[1][t];
                if (next)
                    r += frac * (s.audio[1][t + 1] - r);
            }
        }
        re[static_cast<size_t> (i)] = l * window[static_cast<size_t> (i)];
        im[static_cast<size_t> (i)] = r * window[static_cast<size_t> (i)];
    }
    fft.forward (re.data(), im.data());
}

void SpectralRenderer::unpack (int k, int c, float& xr, float& xi) const noexcept
{
    // Z = L + iR with L, R real: L[k] = (Z[k] + conj Z[N-k]) / 2, R[k] = (Z[k] - conj Z[N-k]) / 2i.
    const int n = fft.size();
    const auto a = static_cast<size_t> (k), b = static_cast<size_t> ((n - k) % n);
    if (c == 0)
    {
        xr = 0.5f * (re[a] + re[b]);
        xi = 0.5f * (im[a] - im[b]);
    }
    else
    {
        xr = 0.5f * (im[a] + im[b]);
        xi = -0.5f * (re[a] - re[b]);
    }
}

void SpectralRenderer::render (const SpectralSource* sources, int numSources, int channels, bool freeze,
                               float* const* out) noexcept
{
    const int n = fft.size();
    const int bins = n / 2 + 1;
    const int H = hop();
    channels = std::clamp (channels, 1, 2);

    bool frozenNow[2] = { false, false };
    for (int c = 0; c < channels; ++c)
    {
        frozenNow[c] = freeze && haveFrozen[static_cast<size_t> (c)];
        std::fill (mag[c].begin(), mag[c].end(), 0.0f);
        std::fill (bestMag[c].begin(), bestMag[c].end(), 0.0f);
        std::fill (bestPhase[c].begin(), bestPhase[c].end(), 0.0f);
        std::fill (bestStretched[c].begin(), bestStretched[c].end(), 0);
    }
    const bool analyse = ! (frozenNow[0] && (channels == 1 || frozenNow[1]));

    // Analysis: one packed transform per source (two if stretched).
    for (int si = 0; analyse && si < numSources; ++si)
    {
        const auto& s = sources[si];
        if (std::abs (s.gain[0]) <= 0.0f && (channels == 1 || std::abs (s.gain[1]) <= 0.0f))
            continue;
        const bool stretched = s.rate != 1.0;
        const double analysisHop = H * s.rate;
        if (stretched)
        {
            // Phase vocoder: the frame one analysis hop earlier gives each
            // bin's instantaneous frequency.
            readFrame (s, channels, s.start - analysisHop);
            std::copy (re.begin(), re.end(), re2.begin());
            std::copy (im.begin(), im.end(), im2.begin());
        }
        readFrame (s, channels, s.start);

        for (int c = 0; c < channels; ++c)
        {
            const float g = std::abs (s.gain[c]);
            if (g <= 0.0f || frozenNow[c])
                continue;
            for (int k = 0; k < bins; ++k)
            {
                const auto kk = static_cast<size_t> (k);
                float xr, xi;
                unpack (k, c, xr, xi);
                const float m = g * std::hypot (xr, xi);
                mag[c][kk] += m;
                if (m <= bestMag[c][kk])
                    continue;
                bestMag[c][kk] = m;
                const float ph = std::atan2 (xi, xr);
                if (stretched)
                {
                    // Previous frame's phase for this channel and bin.
                    const auto b = static_cast<size_t> ((n - k) % n);
                    const float pr = c == 0 ? 0.5f * (re2[kk] + re2[b]) : 0.5f * (im2[kk] + im2[b]);
                    const float pi = c == 0 ? 0.5f * (im2[kk] - im2[b]) : -0.5f * (re2[kk] - re2[b]);
                    const double expected = kTwoPi * k * analysisHop / n;
                    const double dphi = ph - std::atan2 (pi, pr);
                    bestFreq[c][kk] = static_cast<float> ((expected + princarg (dphi - expected)) / analysisHop);
                    bestStretched[c][kk] = 1;
                }
                else
                {
                    bestPhase[c][kk] = ph;
                    bestStretched[c][kk] = 0;
                }
            }
        }
    }

    // Phases and magnitudes per channel.
    for (int c = 0; c < channels; ++c)
    {
        auto& phase = outPhase[static_cast<size_t> (c)];
        auto& held = frozenMag[static_cast<size_t> (c)];
        auto& freq = binFreq[static_cast<size_t> (c)];
        if (frozenNow[c])
        {
            // Hold the spectrum; each bin's phase keeps turning at the frequency
            // measured just before the freeze (so partials stay coherent), with
            // a little jitter so the drone doesn't sound static.
            for (int k = 0; k < bins; ++k)
            {
                const auto kk = static_cast<size_t> (k);
                rng = rng * 6364136223846793005ull + 1442695040888963407ull;
                const double jitter = (static_cast<double> (rng >> 40) / 16777216.0 - 0.5) * 0.08;
                phase[kk] = princarg (phase[kk] + freq[kk] * H + jitter);
                mag[c][kk] = held[kk];
            }
            continue;
        }
        for (int k = 0; k < bins; ++k)
        {
            const auto kk = static_cast<size_t> (k);
            const float next = bestStretched[c][kk] ? princarg (phase[kk] + bestFreq[c][kk] * H) : bestPhase[c][kk];
            // Measured frequency of this bin (for freezing), smoothed over
            // frames so one frame where the echo switches memories can't
            // throw it off.
            const double expected = kTwoPi * k * H / n;
            const auto measured = static_cast<float> ((expected + princarg (next - phase[kk] - expected)) / H);
            freq[kk] += 0.3f * (measured - freq[kk]);
            phase[kk] = next;
        }
        std::copy (mag[c].begin(), mag[c].end(), held.begin());
        haveFrozen[static_cast<size_t> (c)] = true;
    }

    // Resynthesis: both channels' (Hermitian) spectra packed into one inverse
    // transform, Z = YL + i YR, so the left output is Re z and the right Im z.
    for (int k = 0; k < n; ++k)
    {
        const int kb = k < bins ? k : n - k; // mirror bin
        const float sgn = k < bins ? 1.0f : -1.0f;
        const auto kk = static_cast<size_t> (kb);
        const float lr = mag[0][kk] * std::cos (outPhase[0][kk]);
        const float li = sgn * mag[0][kk] * std::sin (outPhase[0][kk]);
        float rr = 0.0f, ri = 0.0f;
        if (channels > 1)
        {
            rr = mag[1][kk] * std::cos (outPhase[1][kk]);
            ri = sgn * mag[1][kk] * std::sin (outPhase[1][kk]);
        }
        re[static_cast<size_t> (k)] = lr - ri;
        im[static_cast<size_t> (k)] = li + rr;
    }
    fft.inverse (re.data(), im.data());
    // sqrt-Hann analysis x synthesis = Hann; Hann at 75 % overlap sums to 2.
    const float scale = 0.5f / static_cast<float> (n);
    for (int i = 0; i < n; ++i)
    {
        const float w = window[static_cast<size_t> (i)] * scale;
        out[0][i] = re[static_cast<size_t> (i)] * w;
        if (channels > 1)
            out[1][i] = im[static_cast<size_t> (i)] * w;
    }
}

} // namespace mse
