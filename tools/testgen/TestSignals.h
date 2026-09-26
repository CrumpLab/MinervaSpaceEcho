#pragma once

#include "Wav.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mse::testgen {

// Deterministic, seeded synthetic test material. The same options always give
// bit-identical audio on every platform (own RNG, no std distributions).
struct Options
{
    double sampleRate = 48000.0;
    double bpm = 120.0;
    uint32_t seed = 1;
    int bars = 16;
};

struct Clip
{
    std::string name;         // file stem, e.g. "drums"
    std::string description;
    tools::AudioBuffer audio;
};

tools::AudioBuffer drums (const Options& o);        // groove with fills every 4 bars
tools::AudioBuffer chordsBass (const Options& o);   // Am-F-C-G pad + bass line
tools::AudioBuffer melody (const Options& o);       // 1-bar motif with occasional variations
tools::AudioBuffer impulses (const Options& o);     // single-sample clicks on every beat
tools::AudioBuffer styleChange (const Options& o);  // style A for half, then style B
tools::AudioBuffer fullMix (const Options& o);      // drums + chords/bass + melody

std::vector<Clip> generateAll (const Options& o);

// Samples per bar (4/4) for the given options.
int samplesPerBar (const Options& o);

} // namespace mse::testgen
