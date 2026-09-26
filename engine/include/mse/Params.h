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
enum class FullPolicy { Oldest, Random, LeastUsed, Weakest, MergeSimilar, Reject };
enum class WriteMode { Auto, Manual };
enum class NoveltyMode { Off, StoreNovel, StoreFamiliar };
enum class RecordSource { Input, Echo, InputAndEcho };

constexpr float kLevelOffDb = -60.0f;
constexpr float kGateOffDb = -100.0f;
constexpr float kCueGateOffDb = kGateOffDb;

// ---- engine parameters (real units) -------------------------------------------

struct EngineParams
{
    // timing
    SyncMode syncMode = SyncMode::Tempo;
    float traceMs = 2000.0f;                           // trace length when syncMode == Free
    TraceDivision traceDivision = TraceDivision::Bar1; // trace length when syncMode == Tempo

    // memory (capacity and budget are structural: applied via MemoryConfig)
    int capacity = 100;
    int memoryBudgetIndex = 3;                         // see memoryBudgetBytes()
    FullPolicy fullPolicy = FullPolicy::Oldest;
    float mergeThreshold = 1.0f;                       // merge into a trace at least this similar (1 = off)
    bool freeze = false;                               // no writes, no decay

    // writing (encoding gate)
    WriteMode writeMode = WriteMode::Auto;
    bool capture = false;                              // rising edge arms a capture of the current segment
    float writeGateDb = -70.0f;                        // quieter segments are not stored
    NoveltyMode noveltyMode = NoveltyMode::Off;
    float noveltyThreshold = 0.9f;                     // compared with the best similarity to memory
    float writeProbability = 1.0f;
    RecordSource recordSource = RecordSource::Input;

    // clamping
    bool clampIncoming = false;
    float clampBudget = 0.5f;                          // max fraction of capacity that may be clamped
    bool clampProtects = true;                         // clamped traces don't decay

    // forgetting
    float encodingFailure = 0.0f;                      // Lf: probability each stored feature is lost
    float contentDropout = 0.0f;                       // probability each 1/32 of a stored trace drops out
    float decayForget = 0.0f;                          // per segment: probability each feature is forgotten
    float decayFadeDb = 0.0f;                          // per segment: strength lost
    float wearTone = 0.0f;                             // older traces play back duller

    // retrieval
    float power = 3.0f;
    Similarity similarity = Similarity::Hintzman;
    FeatureMode featureMode = FeatureMode::Continuous;
    float ternaryThreshold = 0.5f;
    bool selfMatch = true;
    float cueGateDb = -60.0f;
    NegativeMode negativeMode = NegativeMode::Subtract;
    Normalization normalization = Normalization::Sum;
    float levelTracking = 1.0f;

    // output
    float feedback = 0.0f;
    float echoLevelDb = 0.0f;
    float dryLevelDb = 0.0f;
    float edgeFadeMs = 5.0f;
    float outputGainDb = 0.0f;

    // plugin only
    bool embedMemory = false;

    bool operator== (const EngineParams&) const = default;
};

double memoryBudgetBytes (int budgetIndex);

// ---- parameter table ----------------------------------------------------------
// Single source of truth for parameter IDs, ranges and defaults. The plugin
// builds its host parameters from it and the offline renderer parses presets
// with it, so both always agree. Order here is the order shown in the plugin.

enum class ParamType { Float, Int, Bool, Choice };

enum ParamIndex
{
    kSyncMode, kTraceMs, kTraceDivision,
    kCapacity, kMemoryBudget, kFullPolicy, kMergeThreshold, kFreeze,
    kWriteMode, kCapture, kWriteGateDb, kNoveltyMode, kNoveltyThreshold, kWriteProbability, kRecordSource,
    kClampIncoming, kClampBudget, kClampProtects,
    kEncodingFailure, kContentDropout, kDecayForget, kDecayFadeDb, kWearTone,
    kPower, kSimilarity, kFeatureMode, kTernaryThreshold, kSelfMatch, kCueGateDb, kNegativeMode,
    kNormalization, kLevelTracking,
    kFeedback, kEchoLevelDb, kDryLevelDb, kEdgeFadeMs, kOutputGainDb,
    kEmbedMemory,
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
