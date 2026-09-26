#include "mse/Params.h"

#include <algorithm>
#include <cmath>

namespace mse {

namespace {

const char* const kSyncChoices[] = { "Free", "Tempo" };
const char* const kDivisionChoices[] = { "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars" };
const char* const kSimilarityChoices[] = { "Hintzman", "Cosine" };
const char* const kFeatureChoices[] = { "Continuous", "Ternary" };
const char* const kNegativeChoices[] = { "Subtract", "Ignore", "Absolute" };
const char* const kNormChoices[] = { "Sum", "Max", "Familiarity" };
const char* const kBoolChoices[] = { "Off", "On" };

template <size_t N>
constexpr int count (const char* const (&)[N]) { return static_cast<int> (N); }

// clang-format off
const std::array<ParamSpec, kNumParams> kSpecs { {
    //  id                   name                    type               min      max      def    skew    unit   choices               n                            automatable
    { "sync_mode",         "Sync",                 ParamType::Choice, 0,       1,       1,     0,      "",    kSyncChoices,       count (kSyncChoices),       true },
    { "trace_ms",          "Trace Length (Free)",  ParamType::Float,  10,      20000,   2000,  1000,   "ms",  nullptr,            0,                          true },
    { "trace_division",    "Trace Length (Sync)",  ParamType::Choice, 0,       6,       4,     0,      "",    kDivisionChoices,   count (kDivisionChoices),   true },
    { "capacity",          "Memory Capacity",      ParamType::Int,    1,       1000,    100,   100,    "traces", nullptr,         0,                          false },
    { "power",             "Activation Power",     ParamType::Float,  1,       9,       3,     0,      "",    nullptr,            0,                          true },
    { "similarity",        "Similarity",           ParamType::Choice, 0,       1,       0,     0,      "",    kSimilarityChoices, count (kSimilarityChoices), true },
    { "feature_mode",      "Features",             ParamType::Choice, 0,       1,       0,     0,      "",    kFeatureChoices,    count (kFeatureChoices),    true },
    { "ternary_threshold", "Ternary Threshold",    ParamType::Float,  0,       2,       0.5f,  0,      "",    nullptr,            0,                          true },
    { "encoding_failure",  "Encoding Failure (Lf)",ParamType::Float,  0,       1,       0,     0,      "",    nullptr,            0,                          true },
    { "self_match",        "Self Match",           ParamType::Bool,   0,       1,       1,     0,      "",    kBoolChoices,       count (kBoolChoices),       true },
    { "cue_gate_db",       "Cue Gate",             ParamType::Float,  kCueGateOffDb, 0, -60,   0,      "dB",  nullptr,            0,                          true },
    { "negative_mode",     "Negative Activations", ParamType::Choice, 0,       2,       0,     0,      "",    kNegativeChoices,   count (kNegativeChoices),   true },
    { "normalization",     "Echo Normalization",   ParamType::Choice, 0,       2,       0,     0,      "",    kNormChoices,       count (kNormChoices),       true },
    { "level_tracking",    "Echo Level Tracking",  ParamType::Float,  0,       1,       1,     0,      "",    nullptr,            0,                          true },
    { "feedback",          "Feedback",             ParamType::Float,  0,       1.2f,    0,     0,      "",    nullptr,            0,                          true },
    { "echo_level_db",     "Echo Level",           ParamType::Float,  kLevelOffDb, 6,   0,     0,      "dB",  nullptr,            0,                          true },
    { "dry_level_db",      "Dry Level",            ParamType::Float,  kLevelOffDb, 6,   0,     0,      "dB",  nullptr,            0,                          true },
    { "edge_fade_ms",      "Edge Fade",            ParamType::Float,  0,       50,      5,     0,      "ms",  nullptr,            0,                          true },
    { "output_gain_db",    "Output Gain",          ParamType::Float,  -60,     12,      0,     0,      "dB",  nullptr,            0,                          true },
} };
// clang-format on

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

    EngineParams p;
    p.syncMode = static_cast<SyncMode> (idx (kSyncMode));
    p.traceMs = v (kTraceMs);
    p.traceDivision = static_cast<TraceDivision> (idx (kTraceDivision));
    p.capacity = idx (kCapacity);
    p.power = v (kPower);
    p.similarity = static_cast<Similarity> (idx (kSimilarity));
    p.featureMode = static_cast<FeatureMode> (idx (kFeatureMode));
    p.ternaryThreshold = v (kTernaryThreshold);
    p.encodingFailure = v (kEncodingFailure);
    p.selfMatch = idx (kSelfMatch) != 0;
    p.cueGateDb = v (kCueGateDb);
    p.negativeMode = static_cast<NegativeMode> (idx (kNegativeMode));
    p.normalization = static_cast<Normalization> (idx (kNormalization));
    p.levelTracking = v (kLevelTracking);
    p.feedback = v (kFeedback);
    p.echoLevelDb = v (kEchoLevelDb);
    p.dryLevelDb = v (kDryLevelDb);
    p.edgeFadeMs = v (kEdgeFadeMs);
    p.outputGainDb = v (kOutputGainDb);
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
