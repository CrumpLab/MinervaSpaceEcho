#include "mse/Tape.h"

#include <algorithm>
#include <cmath>

namespace mse {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kBassHz = 250.0;
constexpr double kTrebleHz = 3000.0;
constexpr double kWowHz = 0.55;
constexpr double kFlutterHz = 7.3;
constexpr double kMaxWowMs = 6.0;
constexpr double kMaxFlutterMs = 0.5;
} // namespace

// ---- ShelfEq --------------------------------------------------------------------

void ShelfEq::prepare (double sampleRate) noexcept
{
    sr = sampleRate;
    bass = treble = 0.0f;
    flat = true;
    low = high = Biquad {};
}

void ShelfEq::set (float bassDb, float trebleDb) noexcept
{
    if (bassDb == bass && trebleDb == treble)
        return;
    bass = bassDb;
    treble = trebleDb;
    flat = std::abs (bassDb) < 1.0e-4f && std::abs (trebleDb) < 1.0e-4f;

    // RBJ shelving filters, slope 1.
    auto design = [this] (Biquad& f, double fc, double gainDb, bool lowShelf) {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w0 = 2.0 * kPi * fc / sr;
        const double cw = std::cos (w0), sw = std::sin (w0);
        const double alpha = sw / 2.0 * std::sqrt (2.0);
        const double s = 2.0 * std::sqrt (A) * alpha;
        double b0, b1, b2, a0, a1, a2;
        if (lowShelf)
        {
            b0 = A * ((A + 1) - (A - 1) * cw + s);
            b1 = 2 * A * ((A - 1) - (A + 1) * cw);
            b2 = A * ((A + 1) - (A - 1) * cw - s);
            a0 = (A + 1) + (A - 1) * cw + s;
            a1 = -2 * ((A - 1) + (A + 1) * cw);
            a2 = (A + 1) + (A - 1) * cw - s;
        }
        else
        {
            b0 = A * ((A + 1) + (A - 1) * cw + s);
            b1 = -2 * A * ((A - 1) + (A + 1) * cw);
            b2 = A * ((A + 1) + (A - 1) * cw - s);
            a0 = (A + 1) - (A - 1) * cw + s;
            a1 = 2 * ((A - 1) - (A + 1) * cw);
            a2 = (A + 1) - (A - 1) * cw - s;
        }
        f.b0 = static_cast<float> (b0 / a0);
        f.b1 = static_cast<float> (b1 / a0);
        f.b2 = static_cast<float> (b2 / a0);
        f.a1 = static_cast<float> (a1 / a0);
        f.a2 = static_cast<float> (a2 / a0);
    };
    design (low, kBassHz, bassDb, true);
    design (high, kTrebleHz, trebleDb, false);
}

float ShelfEq::process (int c, float x) noexcept
{
    return high.process (c, low.process (c, x));
}

void ShelfEq::reset() noexcept
{
    for (auto* f : { &low, &high })
        for (int c = 0; c < 2; ++c)
            f->z1[c] = f->z2[c] = 0.0f;
}

// ---- TapeMotion -----------------------------------------------------------------

void TapeMotion::prepare (double sampleRate) noexcept
{
    sr = sampleRate;
    wowPhase = flutterPhase = flutterPhase2 = 0.0;
    drift = driftTarget = 0.0;
}

void TapeMotion::fill (float* out, int n, float wow, float flutter) noexcept
{
    const double wowDepth = wow * kMaxWowMs * 0.001 * sr;
    const double flutterDepth = flutter * kMaxFlutterMs * 0.001 * sr;
    const double driftCoeff = 1.0 - std::exp (-1.0 / (0.7 * sr));
    for (int i = 0; i < n; ++i)
    {
        // A slowly wandering wow rate keeps it from sounding like a clean LFO.
        if ((i & 1023) == 0)
        {
            rng = rng * 6364136223846793005ull + 1442695040888963407ull;
            driftTarget = (static_cast<double> (rng >> 11) / 9007199254740992.0 - 0.5) * 0.6;
        }
        drift += (driftTarget - drift) * driftCoeff;
        wowPhase += 2.0 * kPi * kWowHz * (1.0 + drift) / sr;
        flutterPhase += 2.0 * kPi * kFlutterHz / sr;
        flutterPhase2 += 2.0 * kPi * kFlutterHz * 1.61 / sr;
        if (wowPhase > 2.0 * kPi) wowPhase -= 2.0 * kPi;
        if (flutterPhase > 2.0 * kPi) flutterPhase -= 2.0 * kPi;
        if (flutterPhase2 > 2.0 * kPi) flutterPhase2 -= 2.0 * kPi;

        const double w = wowDepth * 0.5 * (1.0 + std::sin (wowPhase));
        const double f = flutterDepth * 0.5 * (1.0 + 0.7 * std::sin (flutterPhase) + 0.3 * std::sin (flutterPhase2));
        out[i] = static_cast<float> (w + f);
    }
}

// ---- SpringReverb ---------------------------------------------------------------

void SpringReverb::prepare (double sampleRate)
{
    sr = sampleRate;
    const double lengthsMs[2] = { 43.0, 47.3 };
    for (size_t c = 0; c < ch.size(); ++c)
        ch[c].delay.assign (static_cast<size_t> (lengthsMs[c] * 0.001 * sr) + 1, 0.0f);
    setDecay (decay);
    reset();
}

void SpringReverb::reset() noexcept
{
    for (auto& c : ch)
    {
        std::fill (c.delay.begin(), c.delay.end(), 0.0f);
        c.writePos = 0;
        c.damp = c.hp = c.hpIn = c.tone = 0.0f;
        for (auto& s : c.apX)
            s.fill (0.0f);
        for (auto& s : c.apY)
            s.fill (0.0f);
        c.apPos = 0;
    }
}

void SpringReverb::setDecay (float seconds) noexcept
{
    decay = std::max (0.05f, seconds);
    for (auto& c : ch)
    {
        // Gain per loop so that the tail falls 60 dB in `decay` seconds.
        const double loopSeconds = static_cast<double> (c.delay.size()) / sr;
        c.feedbackGain = static_cast<float> (std::pow (10.0, -3.0 * loopSeconds / decay));
    }
}

float SpringReverb::process (int channel, float x) noexcept
{
    auto& c = ch[static_cast<size_t> (channel & 1)];
    if (c.delay.empty())
        return 0.0f;

    // Springs don't pass much bass: high-pass the input around 150 Hz.
    const float hpCoeff = static_cast<float> (std::exp (-2.0 * kPi * 150.0 / sr));
    c.hp = hpCoeff * (c.hp + x - c.hpIn);
    c.hpIn = x;

    // Loop: input + damped feedback -> dispersion -> delay.
    const float delayed = c.delay[static_cast<size_t> (c.writePos)];
    const float dampCoeff = static_cast<float> (1.0 - std::exp (-2.0 * kPi * 4500.0 / sr));
    c.damp += dampCoeff * (delayed - c.damp);
    float v = c.hp + c.feedbackGain * c.damp;

    // Stretched allpass chain: y = a*x + x[n-k] - a*y[n-k].
    constexpr float a = 0.62f;
    const int p = c.apPos;
    for (int s = 0; s < kStages; ++s)
    {
        const float xk = c.apX[static_cast<size_t> (s)][static_cast<size_t> (p)];
        const float yk = c.apY[static_cast<size_t> (s)][static_cast<size_t> (p)];
        const float y = a * v + xk - a * yk;
        c.apX[static_cast<size_t> (s)][static_cast<size_t> (p)] = v;
        c.apY[static_cast<size_t> (s)][static_cast<size_t> (p)] = y;
        v = y;
    }
    c.apPos = (p + 1) % kStretch;

    c.delay[static_cast<size_t> (c.writePos)] = v;
    c.writePos = (c.writePos + 1) % static_cast<int> (c.delay.size());

    // Output: the dispersed signal, gently darkened.
    const float toneCoeff = static_cast<float> (1.0 - std::exp (-2.0 * kPi * 6000.0 / sr));
    c.tone += toneCoeff * (v - c.tone);
    return c.tone;
}

} // namespace mse
