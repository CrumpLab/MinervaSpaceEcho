// Stage 10: address sets. Every trace stores several addresses (Spectrum,
// Pitch Class, Pitch, Timbre, Rhythm); retrieval compares a weighted mix.
#include "TestHelpers.h"

#include "mse/Import.h"
#include "mse/MemorySnapshot.h"
#include "mse/Retrieval.h"

#include <nlohmann/json.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

using namespace mse;
using namespace mse::test;

namespace {

constexpr double kSr = 48000.0;
constexpr int kSeg = 24000; // 500 ms traces
constexpr double kPi = 3.14159265358979323846;

double midiHz (int note) { return 440.0 * std::pow (2.0, (note - 69) / 12.0); }

// A plucked synth note: harmonics with the given roll-off (1 = saw), decaying.
AudioBuffer note (int midi, int samples = kSeg, double rolloff = 1.0, int harmonics = 12)
{
    auto b = silence (2, samples);
    const double f = midiHz (midi);
    for (int i = 0; i < samples; ++i)
    {
        const double t = i / kSr;
        double x = 0.0;
        for (int k = 1; k <= harmonics && f * k < 20000.0; ++k)
            x += std::sin (2.0 * kPi * f * k * t) / std::pow (k, rolloff);
        const double env = std::min (1.0, t / 0.01) * std::exp (-t * 1.5) * std::min (1.0, (samples - i) / 480.0);
        b.channels[0][static_cast<size_t> (i)] = b.channels[1][static_cast<size_t> (i)] = static_cast<float> (0.2 * x * env);
    }
    return b;
}

FeatureVector addressOf (const AudioBuffer& b, FeatureSettings settings = {})
{
    FeatureExtractor fx;
    fx.prepare (kSr);
    fx.beginSegment (static_cast<double> (b.channels[0].size()));
    fx.push (b.channels[0].data(), static_cast<int> (b.channels[0].size()), 0);
    FeatureVector f {};
    fx.finalize (f, settings);
    return f;
}

float setSim (const FeatureVector& a, const FeatureVector& b, AddressSet set)
{
    return setSimilarity (a.data() + setOffset (static_cast<int> (set)), b.data() + setOffset (static_cast<int> (set)),
                          Similarity::Hintzman);
}

// C D E F G A B C' C: one note per 500 ms segment.
const std::vector<int> kScale { 60, 62, 64, 65, 67, 69, 71, 72, 60 };
const char* const kNames[] { "C", "D", "E", "F", "G", "A", "B", "C'" };

int scaleIndex (int midi)
{
    for (int i = 0; i < 8; ++i)
        if (kScale[static_cast<size_t> (i)] == midi)
            return i;
    return -1;
}

// Plays the scale into memory, then lets the echo chain run free from a C
// cue; returns the note (index into kScale[0..7]) heard in each step.
// Memory: [- | C] [C | D] ... [B | C'] [C' | C].
std::vector<int> scaleWalk (int address, int steps, uint64_t seed)
{
    auto p = wetOnly();
    p.syncMode = SyncMode::Free;
    p.traceMs = 500.0f;
    p.power = 6.0f;
    p.sequenceContext = true;
    p.contextCue = ContextCue::PredictNext;
    p.playback = Playback::Sample;
    p.chainStep = ChainStep::Sample;
    p.address = static_cast<AddressMode> (address);
    EchoEngine e;
    e.setSeed (seed);
    prepare (e, p, 40);
    // The scale, then a C that cues memory (it is stored only once the next
    // segment starts, so memory is frozen from there on).
    AudioBuffer learn;
    for (int m : kScale)
        append (learn, note (m));
    append (learn, note (60));
    run (e, learn);
    p.freeze = true;
    p.cueSource = CueSource::EchoChain;
    e.setParams (p);
    const auto out = run (e, silence (2, kSeg * (steps + 1)));

    std::vector<int> heard;
    for (int k = 0; k < steps; ++k)
    {
        int best = -1;
        double bestMatch = 0.5;
        for (int i = 0; i < 8; ++i)
        {
            const double m = correlation (slice (out, k * kSeg + 2000, kSeg - 4000),
                                          slice (note (kScale[static_cast<size_t> (i)]), 2000, kSeg - 4000));
            if (m > bestMatch)
            {
                bestMatch = m;
                best = i;
            }
        }
        heard.push_back (best);
    }
    return heard;
}

// Fraction of steps that follow the scale (C' is followed by C).
double scaleFollowing (const std::vector<int>& heard, bool octaveBlind)
{
    int ok = 0;
    for (size_t k = 1; k < heard.size(); ++k)
    {
        const int prev = heard[k - 1], cur = heard[k];
        if (prev < 0 || cur < 0)
            continue;
        const int next = prev == 7 ? 0 : prev + 1;
        // Octave-blind addresses can't tell C' from C, so after C' either C
        // or D (what follows C) is what memory learned.
        ok += cur == next || (octaveBlind && prev == 7 && cur == 1) ? 1 : 0;
    }
    return static_cast<double> (ok) / static_cast<double> (heard.size() - 1);
}

std::string names (const std::vector<int>& heard)
{
    std::string s;
    for (int i : heard)
        s += (i < 0 ? std::string ("?") : std::string (kNames[i])) + " ";
    return s;
}

} // namespace

TEST_CASE ("A single weighted set is exactly that set's similarity; a mix is the weighted mean")
{
    const auto c = addressOf (note (60)), g = addressOf (note (67));
    REQUIRE (addressSimilarity (c, g, Similarity::Hintzman, kSpectrumOnly) == similarity (c, g, Similarity::Hintzman));
    AddressWeights pitch {};
    pitch[static_cast<size_t> (AddressSet::Pitch)] = 0.3f;
    REQUIRE (addressSimilarity (c, g, Similarity::Hintzman, pitch) == setSim (c, g, AddressSet::Pitch));

    AddressWeights mix { 1.0f, 0.0f, 3.0f, 0.0f, 0.0f };
    const float expected = (setSim (c, g, AddressSet::Spectrum) + 3.0f * setSim (c, g, AddressSet::Pitch)) / 4.0f;
    REQUIRE (addressSimilarity (c, g, Similarity::Hintzman, mix) == Catch::Approx (expected).margin (1e-5));

    // Unweighted: the spectrum.
    REQUIRE (addressSimilarity (c, g, Similarity::Hintzman, AddressWeights {}) == similarity (c, g, Similarity::Hintzman));

    // The parameters: a one-hot vector unless Custom, where all-zero means Spectrum.
    EngineParams p;
    p.address = AddressMode::Timbre;
    REQUIRE (effectiveAddressWeights (p)[static_cast<size_t> (AddressSet::Timbre)] == 1.0f);
    p.address = AddressMode::Custom;
    p.addressWeights = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    REQUIRE (effectiveAddressWeights (p)[0] == 1.0f);
    p.addressWeights = { 0.0f, 0.5f, 0.25f, 0.0f, 0.0f };
    REQUIRE (effectiveAddressWeights (p)[1] == 0.5f);
    REQUIRE (effectiveAddressWeights (p)[2] == 0.25f);
    REQUIRE (dominantSet (effectiveAddressWeights (p)) == 1);
}

TEST_CASE ("Pitch sets tell neighbouring notes apart where the spectrum can't")
{
    const auto c = addressOf (note (60)), d = addressOf (note (62)), c5 = addressOf (note (72));
    INFO ("spectrum C-D " << setSim (c, d, AddressSet::Spectrum) << ", pitch class C-D " << setSim (c, d, AddressSet::PitchClass)
                          << ", pitch C-D " << setSim (c, d, AddressSet::Pitch));
    // The 1/3-octave spectrum barely sees a whole tone.
    REQUIRE (setSim (c, d, AddressSet::Spectrum) > 0.9f);
    REQUIRE (setSim (c, d, AddressSet::PitchClass) < 0.3f);
    REQUIRE (setSim (c, d, AddressSet::Pitch) < 0.2f);

    // Pitch Class is octave-blind; Pitch is not.
    INFO ("C4-C5: pitch class " << setSim (c, c5, AddressSet::PitchClass) << ", pitch " << setSim (c, c5, AddressSet::Pitch));
    REQUIRE (setSim (c, c5, AddressSet::PitchClass) > 0.6f);
    REQUIRE (setSim (c, c5, AddressSet::Pitch) < 0.45f);
    REQUIRE (setSim (c, c5, AddressSet::PitchClass) > setSim (c, c5, AddressSet::Pitch) + 0.25f);

    // Every set of a note matches itself.
    for (int s = 0; s < kNumSets; ++s)
        REQUIRE (setSim (c, c, static_cast<AddressSet> (s)) == Catch::Approx (1.0f).margin (0.02f));
}

TEST_CASE ("Timbre hears the sound, not the note")
{
    // A bright saw and a dark one (steep roll-off) playing the same C, and
    // the bright saw on another note.
    const auto brightC = addressOf (note (60, kSeg, 1.0, 24)), darkC = addressOf (note (60, kSeg, 3.0, 24));
    const auto brightG = addressOf (note (67, kSeg, 1.0, 24));
    INFO ("timbre: same sound, other note " << setSim (brightC, brightG, AddressSet::Timbre)
                                            << "; same note, other sound " << setSim (brightC, darkC, AddressSet::Timbre));
    REQUIRE (setSim (brightC, brightG, AddressSet::Timbre) > setSim (brightC, darkC, AddressSet::Timbre) + 0.2f);
    // Pitch does the opposite.
    REQUIRE (setSim (brightC, darkC, AddressSet::Pitch) > setSim (brightC, brightG, AddressSet::Pitch) + 0.2f);
}

TEST_CASE ("Rhythm hears where the attacks are, not what is played")
{
    // Four short notes per segment: at 0, 1/4, 1/2, 3/4 (pattern 1) or at
    // 0, 3/8, 1/2, 7/8 (pattern 2), on two different pitches.
    auto pattern = [] (const std::vector<double>& onsets, int midi) {
        auto b = silence (2, kSeg);
        for (double o : onsets)
        {
            const auto n = note (midi, kSeg / 8);
            const int at = static_cast<int> (o * kSeg);
            for (int i = 0; i < kSeg / 8 && at + i < kSeg; ++i)
                b.channels[0][static_cast<size_t> (at + i)] = b.channels[1][static_cast<size_t> (at + i)] = n.channels[0][static_cast<size_t> (i)];
        }
        return b;
    };
    const auto a1 = addressOf (pattern ({ 0.0, 0.25, 0.5, 0.75 }, 48));
    const auto a2 = addressOf (pattern ({ 0.0, 0.25, 0.5, 0.75 }, 67));
    const auto b1 = addressOf (pattern ({ 0.0, 0.375, 0.5, 0.875 }, 48));
    INFO ("rhythm: same pattern, other pitch " << setSim (a1, a2, AddressSet::Rhythm) << "; other pattern, same pitch "
                                               << setSim (a1, b1, AddressSet::Rhythm));
    REQUIRE (setSim (a1, a2, AddressSet::Rhythm) > setSim (a1, b1, AddressSet::Rhythm) + 0.2f);
}

TEST_CASE ("With a pitch address the echo chain walks the learned scale")
{
    // Every note of a C major scale has a different successor, but to the
    // spectrum neighbouring notes look almost alike, so a walk drifts. With
    // Pitch Class or Pitch the chain plays the scale.
    const int steps = 24;
    const auto pitchClass = scaleWalk (static_cast<int> (AddressMode::PitchClass), steps, 1);
    const auto pitch = scaleWalk (static_cast<int> (AddressMode::Pitch), steps, 2);
    const auto spectrum = scaleWalk (static_cast<int> (AddressMode::Spectrum), steps, 1);
    INFO ("pitch class: " << names (pitchClass));
    INFO ("pitch:       " << names (pitch));
    INFO ("spectrum:    " << names (spectrum));
    REQUIRE (scaleFollowing (pitchClass, true) >= 0.9);
    REQUIRE (scaleFollowing (pitch, false) >= 0.9);
    REQUIRE (scaleFollowing (spectrum, false) < scaleFollowing (pitchClass, true) - 0.3);
}

TEST_CASE ("Progressive cueing uses the address sets too")
{
    // Memory holds C and G; a C played in the bar recalls C with a pitch address.
    auto p = wetOnly();
    p.syncMode = SyncMode::Free;
    p.traceMs = 500.0f;
    p.power = 9.0f;
    p.address = AddressMode::Pitch;
    p.selfMatch = false;
    EchoEngine e;
    prepare (e, p, 10);
    AudioBuffer learn = note (60);
    append (learn, note (67));
    run (e, learn);
    p.freeze = true;
    p.cueMode = CueMode::Progressive;
    e.setParams (p);
    AudioBuffer cue = note (60);
    append (cue, note (60));
    const auto out = run (e, cue);
    // Late in the second bar the echo is the stored C.
    const auto late = slice (out, kSeg + kSeg / 2, kSeg / 3);
    REQUIRE (correlation (late, slice (note (60), kSeg / 2, kSeg / 3)) > 0.8);
}

TEST_CASE ("Memory saved before address sets loads with sets computed from its audio")
{
    auto p = wetOnly();
    p.syncMode = SyncMode::Free;
    p.traceMs = 500.0f;
    p.sequenceContext = true;
    EchoEngine e;
    prepare (e, p, 10);
    AudioBuffer learn = note (60);
    append (learn, note (64));
    append (learn, note (67));
    run (e, learn);
    run (e, silence (2, 512));
    MemorySnapshot snap;
    REQUIRE (e.takeSnapshot (snap));
    REQUIRE (snap.traces.size() == 3);

    const auto dir = (std::filesystem::temp_directory_path() / "mse_address_old").string();
    std::filesystem::remove_all (dir);
    writeMemoryFolder (snap, dir);
    // What an older version wrote: no "sets".
    nlohmann::json m;
    {
        std::ifstream in (dir + "/manifest.json");
        m = nlohmann::json::parse (in);
    }
    REQUIRE (m["traces"][0].contains ("sets"));
    for (auto& t : m["traces"])
    {
        t.erase ("sets");
        t.erase ("context_sets");
    }
    {
        std::ofstream out (dir + "/manifest.json");
        out << m.dump();
    }

    const auto loaded = readMemoryFolder (dir);
    REQUIRE (loaded.traces.size() == 3);
    for (size_t i = 0; i < 3; ++i)
    {
        const auto& a = snap.traces[i];
        const auto& b = loaded.traces[i];
        REQUIRE (b.hasSets);
        // The spectrum is kept as saved; the others are recomputed and
        // match what was computed while playing.
        REQUIRE (setSim (a.features, b.features, AddressSet::Spectrum) == Catch::Approx (1.0f).margin (1e-4));
        for (int s = 1; s < kNumSets; ++s)
        {
            INFO ("trace " << i << ", set " << addressSetName (s));
            REQUIRE (setSimilarity (a.features.data() + setOffset (s), b.features.data() + setOffset (s), Similarity::Cosine) > 0.9f);
        }
    }
    // The context's sets come from the trace before.
    for (int s = 1; s < kNumSets; ++s)
        REQUIRE (std::equal (loaded.traces[1].context.begin() + setOffset (s), loaded.traces[1].context.begin() + setOffset (s) + kSetSize,
                             loaded.traces[0].features.begin() + setOffset (s)));

    // A file with sets reads them back exactly.
    writeMemoryFolder (snap, dir);
    const auto again = readMemoryFolder (dir);
    REQUIRE (again.traces[2].features == snap.traces[2].features);
    REQUIRE (again.traces[2].context == snap.traces[2].context);
    std::filesystem::remove_all (dir);
}

TEST_CASE ("Imported audio gets every address set")
{
    AudioBuffer file = note (60);
    append (file, note (62));
    ImportSettings settings;
    settings.traceSeconds = 0.5;
    const auto snap = tracesFromAudio (file.channels, kSr, kSr, settings);
    REQUIRE (snap.traces.size() == 2);
    const auto direct = addressOf (note (62));
    for (int s = 0; s < kNumSets; ++s)
    {
        INFO (addressSetName (s));
        REQUIRE (setSimilarity (snap.traces[1].features.data() + setOffset (s), direct.data() + setOffset (s), Similarity::Cosine) > 0.95f);
    }
    // Context: the first piece, every set.
    REQUIRE (snap.traces[1].context == snap.traces[0].features);
}
