#include "TestSignals.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace mse::testgen {

using mse::AudioBuffer;

namespace {

constexpr double kPi = 3.14159265358979323846;

// splitmix64: tiny, fast and identical on every platform.
struct Rng
{
    uint64_t state;
    explicit Rng (uint64_t seed) : state (seed * 0x9E3779B97F4A7C15ull + 1) {}

    uint64_t next()
    {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double uniform() { return static_cast<double> (next() >> 11) * (1.0 / 9007199254740992.0); } // [0,1)
    double bipolar() { return uniform() * 2.0 - 1.0; }
};

double midiToHz (double note) { return 440.0 * std::pow (2.0, (note - 69.0) / 12.0); }

double polyBlep (double t, double dt)
{
    if (t < dt)
    {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt)
    {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}

struct Timing
{
    double sr;
    double samplesPerBeat;
    int spb; // samples per bar
    explicit Timing (const Options& o)
        : sr (o.sampleRate),
          samplesPerBeat (o.sampleRate * 60.0 / o.bpm),
          spb (static_cast<int> (std::lround (4.0 * o.sampleRate * 60.0 / o.bpm))) {}

    int step16 (int bar, int step) const
    {
        return bar * spb + static_cast<int> (std::lround (step * samplesPerBeat / 4.0));
    }
};

AudioBuffer makeBuffer (const Options& o)
{
    AudioBuffer b;
    b.sampleRate = o.sampleRate;
    b.resize (2, Timing (o).spb * o.bars);
    return b;
}

// Adds a mono signal with equal-power pan (-1 = left, +1 = right).
template <typename Fn>
void addVoice (AudioBuffer& buf, int start, int length, double pan, Fn&& sampleAt)
{
    const double angle = (pan + 1.0) * 0.25 * kPi;
    const float gl = static_cast<float> (std::cos (angle));
    const float gr = static_cast<float> (std::sin (angle));
    const int n = buf.numSamples();
    for (int i = 0; i < length; ++i)
    {
        const int idx = start + i;
        if (idx < 0)
            continue;
        if (idx >= n)
            break;
        const float v = static_cast<float> (sampleAt (i));
        buf.channels[0][static_cast<size_t> (idx)] += v * gl;
        buf.channels[1][static_cast<size_t> (idx)] += v * gr;
    }
}

void kick (AudioBuffer& b, int start, double vel, double sr)
{
    double phase = 0.0;
    addVoice (b, start, static_cast<int> (0.45 * sr), 0.0, [&] (int i) {
        const double t = i / sr;
        const double f = 45.0 + 110.0 * std::exp (-t / 0.035);
        phase += f / sr;
        const double body = std::sin (2.0 * kPi * phase) * std::exp (-t / 0.22);
        const double click = std::exp (-t / 0.002) * 0.4;
        return vel * (body + click);
    });
}

void snare (AudioBuffer& b, int start, double vel, double sr, Rng& rng, double pan = 0.05)
{
    double prev = 0.0;
    addVoice (b, start, static_cast<int> (0.3 * sr), pan, [&] (int i) {
        const double t = i / sr;
        const double tone = std::sin (2.0 * kPi * 190.0 * t) * std::exp (-t / 0.06) * 0.5;
        const double white = rng.bipolar();
        const double hp = white - prev; // crude high-pass
        prev = white;
        const double noise = hp * std::exp (-t / 0.11) * 0.45;
        return vel * (tone + noise);
    });
}

void hat (AudioBuffer& b, int start, double vel, double sr, Rng& rng, bool open, double pan = 0.35)
{
    double p1 = 0.0, p2 = 0.0;
    const double decay = open ? 0.18 : 0.035;
    addVoice (b, start, static_cast<int> ((open ? 0.5 : 0.12) * sr), pan, [&] (int i) {
        const double t = i / sr;
        const double white = rng.bipolar();
        const double hp = white - 2.0 * p1 + p2; // second difference: brighter
        p2 = p1;
        p1 = white;
        return vel * hp * 0.18 * std::exp (-t / decay);
    });
}

// Detuned polyBLEP saw voice through an enveloped one-pole low-pass.
void sawVoice (AudioBuffer& b, int start, int length, double note, double vel, double sr,
               double cutoffHz, double pan, double attack, double release, int unison = 3)
{
    std::array<double, 5> phases {};
    const double base = midiToHz (note);
    double lp = 0.0;
    const int total = length + static_cast<int> (release * sr);
    addVoice (b, start, total, pan, [&] (int i) {
        const double t = i / sr;
        double s = 0.0;
        for (int u = 0; u < unison; ++u)
        {
            const double detune = unison > 1 ? (u - (unison - 1) * 0.5) * 0.08 : 0.0; // semitones
            const double dt = base * std::pow (2.0, detune / 12.0) / sr;
            phases[static_cast<size_t> (u)] += dt;
            phases[static_cast<size_t> (u)] -= std::floor (phases[static_cast<size_t> (u)]);
            const double ph = phases[static_cast<size_t> (u)];
            s += (2.0 * ph - 1.0) - polyBlep (ph, dt);
        }
        s /= unison;

        double env = std::min (1.0, t / attack);
        if (i >= length)
            env *= std::exp (-(i - length) / (release * sr / 5.0));
        const double fc = cutoffHz * (0.4 + 0.6 * std::exp (-t / 0.3));
        const double a = 1.0 - std::exp (-2.0 * kPi * fc / sr);
        lp += a * (s - lp);
        return vel * env * lp;
    });
}

void triVoice (AudioBuffer& b, int start, int length, double note, double vel, double sr, double pan)
{
    double phase = 0.0;
    const double f = midiToHz (note);
    const int total = length + static_cast<int> (0.05 * sr);
    addVoice (b, start, total, pan, [&] (int i) {
        const double t = i / sr;
        const double vib = 1.0 + 0.004 * std::sin (2.0 * kPi * 5.5 * t) * std::min (1.0, t / 0.2);
        phase += f * vib / sr;
        phase -= std::floor (phase);
        const double tri = 1.0 - 4.0 * std::abs (phase - 0.5);
        double env = std::min (1.0, t / 0.005) * std::exp (-t / 0.6);
        if (i >= length)
            env *= std::exp (-(i - length) / (0.01 * sr));
        return vel * env * tri;
    });
}

void normalise (AudioBuffer& b, float peakTarget = 0.89f)
{
    float peak = 0.0f;
    for (auto& ch : b.channels)
        for (float x : ch)
            peak = std::max (peak, std::abs (x));
    if (peak <= 0.0f)
        return;
    const float g = peakTarget / peak;
    for (auto& ch : b.channels)
        for (float& x : ch)
            x *= g;
}

void mixInto (AudioBuffer& dst, const AudioBuffer& src, float gain)
{
    for (size_t c = 0; c < dst.channels.size(); ++c)
        for (size_t i = 0; i < dst.channels[c].size() && i < src.channels[c].size(); ++i)
            dst.channels[c][i] += src.channels[c][i] * gain;
}

// ---- styles -----------------------------------------------------------------

void renderDrumsA (AudioBuffer& b, const Timing& tm, Rng& rng, int firstBar, int numBars)
{
    constexpr std::array<int, 16> kickPat  { 1,0,0,0, 0,0,0,1, 1,0,1,0, 0,0,0,0 };
    constexpr std::array<int, 16> snarePat { 0,0,0,0, 1,0,0,0, 0,0,0,0, 1,0,0,0 };
    for (int bar = firstBar; bar < firstBar + numBars; ++bar)
    {
        const int local = bar - firstBar;
        const bool fill = (local % 4) == 3;
        const bool openHats = (local % 8) == 5;
        for (int s = 0; s < 16; ++s)
        {
            const double h = 1.0 + 0.08 * rng.bipolar(); // humanised velocity
            if (kickPat[static_cast<size_t> (s)])
                kick (b, tm.step16 (bar, s), 0.9 * h, tm.sr);
            if (fill && s >= 12)
                snare (b, tm.step16 (bar, s), (0.45 + 0.1 * (s - 12)) * h, tm.sr, rng, -0.2 + 0.13 * (s - 12));
            else if (snarePat[static_cast<size_t> (s)])
                snare (b, tm.step16 (bar, s), 0.8 * h, tm.sr, rng);
            if (s % 2 == 0)
                hat (b, tm.step16 (bar, s), (s % 4 == 0 ? 0.7 : 0.5) * h, tm.sr, rng, openHats && s % 4 == 2);
        }
    }
}

// Half-time, busier hats: a clearly different groove for novelty tests.
void renderDrumsB (AudioBuffer& b, const Timing& tm, Rng& rng, int firstBar, int numBars)
{
    constexpr std::array<int, 16> kickPat  { 1,0,0,1, 0,0,1,0, 0,0,0,0, 0,1,0,0 };
    for (int bar = firstBar; bar < firstBar + numBars; ++bar)
    {
        for (int s = 0; s < 16; ++s)
        {
            const double h = 1.0 + 0.08 * rng.bipolar();
            if (kickPat[static_cast<size_t> (s)])
                kick (b, tm.step16 (bar, s), 0.85 * h, tm.sr);
            if (s == 8)
                snare (b, tm.step16 (bar, s), 0.9 * h, tm.sr, rng);
            hat (b, tm.step16 (bar, s), (s % 2 == 0 ? 0.55 : 0.3) * h, tm.sr, rng, false, -0.35);
        }
    }
}

struct Progression
{
    std::array<std::array<int, 3>, 4> chords;
    std::array<int, 4> roots;
};

// A minor: Am F C G
constexpr Progression kProgA { { { { 57, 60, 64 }, { 57, 60, 65 }, { 55, 60, 64 }, { 55, 59, 62 } } },
                               { 45, 41, 48, 43 } };
// D minor: Dm Bb Gm A
constexpr Progression kProgB { { { { 57, 62, 65 }, { 58, 62, 65 }, { 55, 58, 62 }, { 57, 61, 64 } } },
                               { 38, 46, 43, 45 } };

void renderPad (AudioBuffer& b, const Timing& tm, const Progression& p, int firstBar, int numBars,
                double cutoff, bool stabs)
{
    for (int bar = firstBar; bar < firstBar + numBars; ++bar)
    {
        const auto& chord = p.chords[static_cast<size_t> ((bar - firstBar) % 4)];
        for (size_t v = 0; v < chord.size(); ++v)
        {
            const double pan = (static_cast<double> (v) - 1.0) * 0.5;
            if (stabs)
            {
                for (int s : { 0, 6, 10 })
                    sawVoice (b, tm.step16 (bar, s), static_cast<int> (tm.samplesPerBeat * 0.4),
                              chord[v], 0.22, tm.sr, cutoff * 1.6, pan, 0.003, 0.08, 2);
            }
            else
            {
                sawVoice (b, tm.step16 (bar, 0), tm.spb - static_cast<int> (0.02 * tm.sr),
                          chord[v], 0.2, tm.sr, cutoff, pan, 0.03, 0.12);
            }
        }
    }
}

void renderBass (AudioBuffer& b, const Timing& tm, const Progression& p, int firstBar, int numBars)
{
    constexpr std::array<int, 8> octave { 0, 0, 12, 0, 0, 12, 0, 7 };
    for (int bar = firstBar; bar < firstBar + numBars; ++bar)
    {
        const int root = p.roots[static_cast<size_t> ((bar - firstBar) % 4)];
        for (int e = 0; e < 8; ++e)
            sawVoice (b, tm.step16 (bar, e * 2), static_cast<int> (tm.samplesPerBeat * 0.42),
                      root + octave[static_cast<size_t> (e)], 0.5, tm.sr, 700.0, 0.0, 0.004, 0.03, 1);
    }
}

void renderMelody (AudioBuffer& b, const Timing& tm, int firstBar, int numBars)
{
    // (16th-step, length in 16ths, MIDI note)
    struct Note { int step, len, note; };
    const std::vector<Note> motif     { { 0, 2, 76 }, { 2, 2, 72 }, { 4, 1, 74 }, { 5, 3, 76 },
                                        { 8, 2, 79 }, { 10, 2, 76 }, { 12, 4, 72 } };
    const std::vector<Note> variation { { 0, 2, 76 }, { 2, 2, 72 }, { 4, 1, 74 }, { 5, 3, 77 },
                                        { 8, 2, 81 }, { 10, 2, 79 }, { 12, 4, 76 } };
    const std::vector<Note> contrast  { { 0, 4, 84 }, { 4, 2, 83 }, { 6, 2, 81 }, { 8, 1, 79 },
                                        { 9, 1, 81 }, { 10, 2, 79 }, { 12, 2, 77 }, { 14, 2, 76 } };
    for (int bar = firstBar; bar < firstBar + numBars; ++bar)
    {
        const int local = bar - firstBar;
        const auto& pattern = local == 13 ? contrast : ((local == 5 || local == 10) ? variation : motif);
        for (const auto& n : pattern)
            triVoice (b, tm.step16 (bar, n.step), static_cast<int> (n.len * tm.samplesPerBeat / 4.0 * 0.9),
                      n.note, 0.35, tm.sr, 0.15);
    }
}

} // namespace

int samplesPerBar (const Options& o) { return Timing (o).spb; }

AudioBuffer drums (const Options& o)
{
    auto b = makeBuffer (o);
    Rng rng (o.seed * 101u + 1u);
    renderDrumsA (b, Timing (o), rng, 0, o.bars);
    normalise (b);
    return b;
}

AudioBuffer chordsBass (const Options& o)
{
    auto b = makeBuffer (o);
    const Timing tm (o);
    renderPad (b, tm, kProgA, 0, o.bars, 1800.0, false);
    renderBass (b, tm, kProgA, 0, o.bars);
    normalise (b);
    return b;
}

AudioBuffer melody (const Options& o)
{
    auto b = makeBuffer (o);
    renderMelody (b, Timing (o), 0, o.bars);
    normalise (b);
    return b;
}

AudioBuffer impulses (const Options& o)
{
    auto b = makeBuffer (o);
    const Timing tm (o);
    for (int bar = 0; bar < o.bars; ++bar)
        for (int beat = 0; beat < 4; ++beat)
        {
            const auto idx = static_cast<size_t> (tm.step16 (bar, beat * 4));
            const float v = beat == 0 ? 1.0f : 0.5f;
            if (idx < b.channels[0].size())
                b.channels[0][idx] = b.channels[1][idx] = v;
        }
    return b; // not normalised: exact values are part of the test contract
}

AudioBuffer styleChange (const Options& o)
{
    auto b = makeBuffer (o);
    const Timing tm (o);
    Rng rng (o.seed * 131u + 7u);
    const int half = o.bars / 2;
    renderDrumsA (b, tm, rng, 0, half);
    renderPad (b, tm, kProgA, 0, half, 1800.0, false);
    renderBass (b, tm, kProgA, 0, half);
    renderDrumsB (b, tm, rng, half, o.bars - half);
    renderPad (b, tm, kProgB, half, o.bars - half, 3200.0, true);
    normalise (b);
    return b;
}

// A synth plays the C major scale, one note per bar (C D E F G A B C' and
// back to C), then rests: memory holds a sequence in which every note has
// a different successor (Stage 10's pitch addresses).
AudioBuffer scale (const Options& o)
{
    auto b = makeBuffer (o);
    const Timing tm (o);
    constexpr std::array<int, 9> kNotes { 60, 62, 64, 65, 67, 69, 71, 72, 60 };
    for (int bar = 0; bar < std::min (o.bars, static_cast<int> (kNotes.size())); ++bar)
        sawVoice (b, tm.step16 (bar, 0), tm.spb - static_cast<int> (0.06 * tm.sr), kNotes[static_cast<size_t> (bar)], 0.8,
                  tm.sr, 2600.0, 0.0, 0.005, 0.04, 1);
    normalise (b);
    return b;
}

AudioBuffer fullMix (const Options& o)
{
    auto b = makeBuffer (o);
    mixInto (b, drums (o), 0.8f);
    mixInto (b, chordsBass (o), 0.55f);
    mixInto (b, melody (o), 0.45f);
    normalise (b);
    return b;
}

std::vector<Clip> generateAll (const Options& o)
{
    return {
        { "drums",        "Drum groove, fills every 4th bar, open hats in bar 6 of 8", drums (o) },
        { "chords_bass",  "Am-F-C-G saw pad and eighth-note bass line",                chordsBass (o) },
        { "melody",       "1-bar motif; variations in bars 6 and 11, contrast in bar 14", melody (o) },
        { "impulses",     "Single-sample clicks: 1.0 on downbeats, 0.5 on other beats", impulses (o) },
        { "style_change", "Style A (Am, 4/4 groove) then style B (Dm, half-time stabs) at the midpoint", styleChange (o) },
        { "full_mix",     "Drums + chords/bass + melody",                              fullMix (o) },
        { "scale",        "C major scale, one synth note per bar (C to C' and back to C), then rests", scale (o) },
    };
}

} // namespace mse::testgen
