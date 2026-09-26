#pragma once

#include <array>
#include <string_view>

namespace mse {

// ---- enums ------------------------------------------------------------------

enum class SyncMode { Free, Tempo };
enum class TraceDivision { Sixteenth, Eighth, Quarter, Half, Bar1, Bar2, Bar4 };
enum class Similarity { Hintzman, Cosine };
enum class FeatureMode { Continuous, Ternary };
enum class NegativeMode { Subtract, Ignore, Absolute };
enum class Normalization { Sum, Max, Familiarity };

// ---- engine parameters (real units) -------------------------------------------

struct EngineParams
{
    SyncMode syncMode = SyncMode::Tempo;
    float traceMs = 2000.0f;                        // trace length when syncMode == Free
    TraceDivision traceDivision = TraceDivision::Bar1; // trace length when syncMode == Tempo
    int capacity = 100;                             // structural: applied via MemoryConfig, not per block

    float power = 3.0f;                             // activation = sign(S) * |S|^power
    Similarity similarity = Similarity::Hintzman;
    FeatureMode featureMode = FeatureMode::Continuous;
    float ternaryThreshold = 0.5f;
    float encodingFailure = 0.0f;                   // Lf: probability each stored feature is lost
    bool selfMatch = true;                          // may the just-stored trace answer its own cue?
    float cueGateDb = -60.0f;                       // quieter segments don't cue memory (<= kCueGateOffDb: off)
    NegativeMode negativeMode = NegativeMode::Subtract;
    Normalization normalization = Normalization::Sum;
    float levelTracking = 1.0f;                     // 1: echo level follows the cue's level; 0: memories at their own level

    float feedback = 0.0f;                          // echo -> record path
    float echoLevelDb = 0.0f;                       // <= kLevelOffDb means off
    float dryLevelDb = 0.0f;
    float edgeFadeMs = 5.0f;
    float outputGainDb = 0.0f;
};

constexpr float kLevelOffDb = -60.0f;
constexpr float kCueGateOffDb = -100.0f;

// ---- parameter table ----------------------------------------------------------
// Single source of truth for parameter IDs, ranges and defaults. The plugin
// builds its host parameters from it and the offline renderer parses presets
// with it, so both always agree.

enum class ParamType { Float, Int, Bool, Choice };

enum ParamIndex
{
    kSyncMode,
    kTraceMs,
    kTraceDivision,
    kCapacity,
    kPower,
    kSimilarity,
    kFeatureMode,
    kTernaryThreshold,
    kEncodingFailure,
    kSelfMatch,
    kCueGateDb,
    kNegativeMode,
    kNormalization,
    kLevelTracking,
    kFeedback,
    kEchoLevelDb,
    kDryLevelDb,
    kEdgeFadeMs,
    kOutputGainDb,
    kNumParams
};

struct ParamSpec
{
    const char* id;
    const char* name;
    ParamType type;
    float min, max, def;       // choice/bool: indices
    float skewCentre;          // value at the middle of a slider; 0 = linear
    const char* unit;
    const char* const* choices;
    int numChoices;
    bool automatable;
};

using ParamValues = std::array<float, kNumParams>;

const std::array<ParamSpec, kNumParams>& paramSpecs();
ParamValues defaultParamValues();
int findParam (std::string_view id); // -1 if unknown

// Converts raw values (real units; choice/bool as index) into EngineParams.
// Out-of-range values are clamped.
EngineParams paramsFromValues (const ParamValues& values);

// Length of one trace in quarter notes for a tempo-synced division.
double divisionQuarters (TraceDivision d, int timeSigNumerator, int timeSigDenominator);

} // namespace mse
