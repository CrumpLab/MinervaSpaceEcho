#include "mse/Params.h"

#include <algorithm>
#include <cmath>

namespace mse {

namespace {

const char* const kSyncChoices[] = { "Free", "Tempo" };
const char* const kDivisionChoices[] = { "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars" };
const char* const kBudgetChoices[] = { "128 MB", "256 MB", "512 MB", "1 GB", "2 GB", "4 GB" };
const char* const kPolicyChoices[] = { "Oldest", "Random", "Least Used", "Weakest", "Merge Similar", "Reject" };
const char* const kWriteModeChoices[] = { "Auto", "Manual" };
const char* const kNoveltyChoices[] = { "Off", "Store Novel", "Store Familiar" };
const char* const kSourceChoices[] = { "Input", "Echo", "Input + Echo" };
const char* const kCueModeChoices[] = { "Segment", "Progressive", "Rolling" };
const char* const kSimilarityChoices[] = { "Hintzman", "Cosine" };
const char* const kFeatureChoices[] = { "Continuous", "Ternary" };
const char* const kNegativeChoices[] = { "Subtract", "Ignore", "Absolute" };
const char* const kNormChoices[] = { "Sum", "Max", "Familiarity" };
const char* const kBoolChoices[] = { "Off", "On" };

template <size_t N>
constexpr int count (const char* const (&)[N]) { return static_cast<int> (N); }

#define CHOICE(arr) arr, count (arr)
#define NOCHOICE nullptr, 0

// clang-format off
const std::array<ParamSpec, kNumParams> kSpecs { {
    //  id                    name                     type               min   max     def    skew  unit      choices                        automatable
    // timing
    { "sync_mode",          "Sync",                  ParamType::Choice, 0,    1,      1,     0,    "",       CHOICE (kSyncChoices),         true },
    { "trace_ms",           "Trace Length (Free)",   ParamType::Float,  10,   20000,  2000,  1000, "ms",     NOCHOICE,                      true },
    { "trace_division",     "Trace Length (Sync)",   ParamType::Choice, 0,    6,      4,     0,    "",       CHOICE (kDivisionChoices),     true },
    // memory
    { "capacity",           "Memory Capacity",       ParamType::Int,    1,    1000,   100,   100,  "traces", NOCHOICE,                      false },
    { "memory_budget",      "Memory Budget",         ParamType::Choice, 0,    5,      3,     0,    "",       CHOICE (kBudgetChoices),       false },
    { "full_policy",        "When Full",             ParamType::Choice, 0,    5,      0,     0,    "",       CHOICE (kPolicyChoices),       true },
    { "merge_threshold",    "Consolidate Above",     ParamType::Float,  0,    1,      1,     0,    "",       NOCHOICE,                      true },
    { "freeze",             "Freeze Memory",         ParamType::Bool,   0,    1,      0,     0,    "",       CHOICE (kBoolChoices),         true },
    // writing
    { "write_mode",         "Write Mode",            ParamType::Choice, 0,    1,      0,     0,    "",       CHOICE (kWriteModeChoices),    true },
    { "capture",            "Capture",               ParamType::Bool,   0,    1,      0,     0,    "",       CHOICE (kBoolChoices),         true },
    { "write_gate_db",      "Write Gate",            ParamType::Float,  kGateOffDb, 0, -70,  0,    "dB",     NOCHOICE,                      true },
    { "novelty_mode",       "Novelty Gate",          ParamType::Choice, 0,    2,      0,     0,    "",       CHOICE (kNoveltyChoices),      true },
    { "novelty_threshold",  "Novelty Threshold",     ParamType::Float,  0,    1,      0.9f,  0,    "",       NOCHOICE,                      true },
    { "write_probability",  "Write Probability",     ParamType::Float,  0,    1,      1,     0,    "",       NOCHOICE,                      true },
    { "record_source",      "Record Source",         ParamType::Choice, 0,    2,      0,     0,    "",       CHOICE (kSourceChoices),       true },
    // clamping
    { "clamp_incoming",     "Clamp Incoming",        ParamType::Bool,   0,    1,      0,     0,    "",       CHOICE (kBoolChoices),         true },
    { "clamp_budget",       "Clamp Budget",          ParamType::Float,  0,    1,      0.5f,  0,    "",       NOCHOICE,                      true },
    { "clamp_protects",     "Clamped Don't Decay",   ParamType::Bool,   0,    1,      1,     0,    "",       CHOICE (kBoolChoices),         true },
    // forgetting
    { "encoding_failure",   "Encoding Failure (Lf)", ParamType::Float,  0,    1,      0,     0,    "",       NOCHOICE,                      true },
    { "content_dropout",    "Tape Dropouts",         ParamType::Float,  0,    1,      0,     0,    "",       NOCHOICE,                      true },
    { "decay_forget",       "Forgetting / Segment",  ParamType::Float,  0,    0.2f,   0,     0,    "",       NOCHOICE,                      true },
    { "decay_fade_db",      "Fading / Segment",      ParamType::Float,  0,    6,      0,     0,    "dB",     NOCHOICE,                      true },
    { "wear_tone",          "Wear Tone",             ParamType::Float,  0,    1,      0,     0,    "",       NOCHOICE,                      true },
    // cueing
    { "cue_mode",           "Cue Mode",              ParamType::Choice, 0,    2,      0,     0,    "",       CHOICE (kCueModeChoices),      true },
    { "progressive_start",  "Progressive Start",     ParamType::Int,    1,    15,     2,     0,    "slots",  NOCHOICE,                      true },
    { "rolling_window_ms",  "Rolling Window",        ParamType::Float,  60,   2000,   250,   400,  "ms",     NOCHOICE,                      true },
    { "rolling_interval_ms","Rolling Interval",      ParamType::Float,  20,   1000,   100,   150,  "ms",     NOCHOICE,                      true },
    { "lookahead_ms",       "Prediction",            ParamType::Float,  0,    2000,   0,     250,  "ms",     NOCHOICE,                      true },
    { "cue_smoothing_ms",   "Cue Smoothing",         ParamType::Float,  1,    500,    40,    60,   "ms",     NOCHOICE,                      true },
    // retrieval
    { "power",              "Activation Power",      ParamType::Float,  1,    9,      3,     0,    "",       NOCHOICE,                      true },
    { "similarity",         "Similarity",            ParamType::Choice, 0,    1,      0,     0,    "",       CHOICE (kSimilarityChoices),   true },
    { "feature_mode",       "Features",              ParamType::Choice, 0,    1,      0,     0,    "",       CHOICE (kFeatureChoices),      true },
    { "ternary_threshold",  "Ternary Threshold",     ParamType::Float,  0,    2,      0.5f,  0,    "",       NOCHOICE,                      true },
    { "self_match",         "Self Match",            ParamType::Bool,   0,    1,      1,     0,    "",       CHOICE (kBoolChoices),         true },
    { "cue_gate_db",        "Cue Gate",              ParamType::Float,  kGateOffDb, 0, -60,  0,    "dB",     NOCHOICE,                      true },
    { "negative_mode",      "Negative Activations",  ParamType::Choice, 0,    2,      0,     0,    "",       CHOICE (kNegativeChoices),     true },
    { "normalization",      "Echo Normalization",    ParamType::Choice, 0,    2,      0,     0,    "",       CHOICE (kNormChoices),         true },
    { "level_tracking",     "Echo Level Tracking",   ParamType::Float,  0,    1,      1,     0,    "",       NOCHOICE,                      true },
    // output
    { "feedback",           "Feedback",              ParamType::Float,  0,    1.2f,   0,     0,    "",       NOCHOICE,                      true },
    { "echo_level_db",      "Echo Level",            ParamType::Float,  kLevelOffDb, 6, 0,   0,    "dB",     NOCHOICE,                      true },
    { "dry_level_db",       "Dry Level",             ParamType::Float,  kLevelOffDb, 6, 0,   0,    "dB",     NOCHOICE,                      true },
    { "edge_fade_ms",       "Edge Fade",             ParamType::Float,  0,    50,     5,     0,    "ms",     NOCHOICE,                      true },
    { "output_gain_db",     "Output Gain",           ParamType::Float,  -60,  12,     0,     0,    "dB",     NOCHOICE,                      true },
    // plugin
    { "embed_memory",       "Save Memory With Set",  ParamType::Bool,   0,    1,      0,     0,    "",       CHOICE (kBoolChoices),         false },
} };
// clang-format on

#undef CHOICE
#undef NOCHOICE

float clampTo (int index, float v)
{
    const auto& s = kSpecs[static_cast<size_t> (index)];
    v = std::clamp (v, s.min, s.max);
    if (s.type != ParamType::Float)
        v = std::round (v);
    return v;
}

} // namespace

const std::array<ParamSpec, kNumParams>& paramSpecs() { return kSpecs; }

double memoryBudgetBytes (int budgetIndex)
{
    return 128.0e6 * std::pow (2.0, std::clamp (budgetIndex, 0, 5));
}

ParamValues defaultParamValues()
{
    ParamValues v {};
    for (size_t i = 0; i < kSpecs.size(); ++i)
        v[i] = kSpecs[i].def;
    return v;
}

int findParam (std::string_view id)
{
    for (size_t i = 0; i < kSpecs.size(); ++i)
        if (id == kSpecs[i].id)
            return static_cast<int> (i);
    return -1;
}

EngineParams paramsFromValues (const ParamValues& raw)
{
    auto v = [&] (int i) { return clampTo (i, raw[static_cast<size_t> (i)]); };
    auto idx = [&] (int i) { return static_cast<int> (v (i)); };
    auto on = [&] (int i) { return idx (i) != 0; };

    EngineParams p;
    p.syncMode = static_cast<SyncMode> (idx (kSyncMode));
    p.traceMs = v (kTraceMs);
    p.traceDivision = static_cast<TraceDivision> (idx (kTraceDivision));

    p.capacity = idx (kCapacity);
    p.memoryBudgetIndex = idx (kMemoryBudget);
    p.fullPolicy = static_cast<FullPolicy> (idx (kFullPolicy));
    p.mergeThreshold = v (kMergeThreshold);
    p.freeze = on (kFreeze);

    p.writeMode = static_cast<WriteMode> (idx (kWriteMode));
    p.capture = on (kCapture);
    p.writeGateDb = v (kWriteGateDb);
    p.noveltyMode = static_cast<NoveltyMode> (idx (kNoveltyMode));
    p.noveltyThreshold = v (kNoveltyThreshold);
    p.writeProbability = v (kWriteProbability);
    p.recordSource = static_cast<RecordSource> (idx (kRecordSource));

    p.clampIncoming = on (kClampIncoming);
    p.clampBudget = v (kClampBudget);
    p.clampProtects = on (kClampProtects);

    p.encodingFailure = v (kEncodingFailure);
    p.contentDropout = v (kContentDropout);
    p.decayForget = v (kDecayForget);
    p.decayFadeDb = v (kDecayFadeDb);
    p.wearTone = v (kWearTone);

    p.cueMode = static_cast<CueMode> (idx (kCueMode));
    p.progressiveStart = idx (kProgressiveStart);
    p.rollingWindowMs = v (kRollingWindowMs);
    p.rollingIntervalMs = v (kRollingIntervalMs);
    p.lookaheadMs = v (kLookaheadMs);
    p.cueSmoothingMs = v (kCueSmoothingMs);

    p.power = v (kPower);
    p.similarity = static_cast<Similarity> (idx (kSimilarity));
    p.featureMode = static_cast<FeatureMode> (idx (kFeatureMode));
    p.ternaryThreshold = v (kTernaryThreshold);
    p.selfMatch = on (kSelfMatch);
    p.cueGateDb = v (kCueGateDb);
    p.negativeMode = static_cast<NegativeMode> (idx (kNegativeMode));
    p.normalization = static_cast<Normalization> (idx (kNormalization));
    p.levelTracking = v (kLevelTracking);

    p.feedback = v (kFeedback);
    p.echoLevelDb = v (kEchoLevelDb);
    p.dryLevelDb = v (kDryLevelDb);
    p.edgeFadeMs = v (kEdgeFadeMs);
    p.outputGainDb = v (kOutputGainDb);

    p.embedMemory = on (kEmbedMemory);
    return p;
}

double divisionQuarters (TraceDivision d, int num, int den)
{
    const double bar = 4.0 * std::max (1, num) / std::max (1, den);
    switch (d)
    {
        case TraceDivision::Sixteenth: return 0.25;
        case TraceDivision::Eighth:    return 0.5;
        case TraceDivision::Quarter:   return 1.0;
        case TraceDivision::Half:      return 2.0;
        case TraceDivision::Bar1:      return bar;
        case TraceDivision::Bar2:      return 2.0 * bar;
        case TraceDivision::Bar4:      return 4.0 * bar;
    }
    return bar;
}

} // namespace mse
