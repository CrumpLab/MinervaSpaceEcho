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
enum class CueMode { Segment, Progressive, Rolling };
enum class HeadMode { Off, Delay, Iterative };
enum class Playback { Blend, Voices, Sample };
enum class CueSource { Input, Sidechain, Random, Frozen };
enum class FeatureFocus { Full, Rhythm, Timbre };
enum class LengthMismatch { Varispeed, Cut, Loop, Stretch };
enum class BlendDomain { Waveform, Spectral };

// RE-201-style head combinations. Custom uses the per-head settings.
enum class ModeSelector { Custom, H1, H2, H3, H2H3, H1H2, H1H3, H1H2H3, Iterative123 };
constexpr int kNumHeads = 3;

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

    // cueing (plan §3)
    CueMode cueMode = CueMode::Segment;
    int progressiveStart = 2;                          // Progressive: slots (of 16) heard before the live cue takes over
    float rollingWindowMs = 250.0f;                    // Rolling: length of the live cue
    float rollingIntervalMs = 100.0f;                  // Rolling: how often memory is searched
    float lookaheadMs = 0.0f;                          // play memory this far ahead (prediction)
    float cueSmoothingMs = 40.0f;                      // crossfade when a live cue changes the echo
    CueSource cueSource = CueSource::Input;
    FeatureFocus featureFocus = FeatureFocus::Full;
    float recency = 0.0f;                              // favour recently stored traces

    // heads (plan §4.3)
    ModeSelector modeSelector = ModeSelector::Custom;
    float headLevelDb[kNumHeads] = { 0.0f, -6.0f, -9.0f };
    float headPan[kNumHeads] = { 0.0f, -0.5f, 0.5f };
    HeadMode headMode[kNumHeads] = { HeadMode::Delay, HeadMode::Off, HeadMode::Off }; // head 1: always the main echo

    // playback (plan §4.2, §4.4)
    Playback playback = Playback::Blend;
    int voices = 4;
    float voiceSpread = 0.7f;
    float voiceDetuneCents = 8.0f;
    float voiceDelayMs = 12.0f;

    // echo tone and familiarity modulation
    float echoToneHz = 20000.0f;                       // echo low-pass (20 kHz = off)
    float intensityToTone = 0.0f;                      // familiar echoes brighter (+) or darker (-)
    float intensityToFeedback = 0.0f;                  // familiar echoes feed back more (+) or less (-)

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

    // tape character (plan §4.5, Stage 5)
    LengthMismatch lengthMismatch = LengthMismatch::Varispeed; // traces recorded at another length
    float wow = 0.0f;                                  // slow speed wobble (0..1)
    float flutter = 0.0f;                              // fast speed wobble (0..1)
    float tapeDrive = 0.0f;                            // echo saturation (0..1)
    float hissDb = kGateOffDb;                         // tape hiss level (-100 = off)
    float feedbackBassDb = 0.0f;                       // low shelf in the feedback path
    float feedbackTrebleDb = 0.0f;                     // high shelf in the feedback path
    float springLevelDb = kLevelOffDb;                 // spring reverb return (-60 = off)
    float springDecaySeconds = 2.0f;
    bool springOnDry = false;                          // spring also on the dry signal (RE-201 modes 8-11)

    // spectral engine (Stage 6)
    BlendDomain blendDomain = BlendDomain::Waveform;
    int spectralVoices = 12;                           // strongest memories rendered per frame
    int maxActive = 64;                                // strongest memories mixed into a blended echo
    bool spectralFreeze = false;                       // hold the echo's spectrum

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
    kCueMode, kProgressiveStart, kRollingWindowMs, kRollingIntervalMs, kLookaheadMs, kCueSmoothingMs,
    kCueSource, kFeatureFocus, kRecency,
    kModeSelector, kHead1LevelDb, kHead1Pan, kHead2Mode, kHead2LevelDb, kHead2Pan, kHead3Mode, kHead3LevelDb, kHead3Pan,
    kPlayback, kVoices, kVoiceSpread, kVoiceDetune, kVoiceDelayMs,
    kEchoToneHz, kIntensityToTone, kIntensityToFeedback,
    kPower, kSimilarity, kFeatureMode, kTernaryThreshold, kSelfMatch, kCueGateDb, kNegativeMode,
    kNormalization, kLevelTracking,
    kLengthMismatch, kWow, kFlutter, kTapeDrive, kHissDb, kFeedbackBassDb, kFeedbackTrebleDb,
    kSpringLevelDb, kSpringDecay, kSpringOnDry,
    kBlendDomain, kSpectralVoices, kSpectralFreeze, kMaxActive,
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
