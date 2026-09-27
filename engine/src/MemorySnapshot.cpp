#include "mse/MemorySnapshot.h"

#include "mse/Features.h"
#include "mse/Params.h"
#include "mse/Wav.h"

#include <algorithm>

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace mse {

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

constexpr char kMagic[4] = { 'M', 'S', 'E', 'M' };

std::string traceFileName (size_t index)
{
    char buf[32];
    std::snprintf (buf, sizeof (buf), "trace_%04zu.wav", index + 1);
    return buf;
}

// Address sets are saved under these keys (the Spectrum set is "features").
const char* setKey (int set)
{
    static const char* const keys[kNumSets] = { "spectrum", "pitch_class", "pitch", "timbre", "rhythm" };
    return keys[set];
}

std::vector<float> setValues (const FeatureVector& f, int set)
{
    return { f.begin() + setOffset (set), f.begin() + setOffset (set) + kSetSize };
}

void readSet (const json& values, FeatureVector& f, int set, const char* what)
{
    const auto v = values.get<std::vector<float>>();
    if (v.size() != static_cast<size_t> (kSetSize))
        throw std::runtime_error (std::string ("trace ") + what + " has the wrong size");
    std::copy (v.begin(), v.end(), f.begin() + setOffset (set));
}

json traceMeta (const TraceRecord& t)
{
    json j {
        { "begin", t.begin },
        { "nominal_length", t.nominalLength },
        { "length", t.length() },
        { "serial", t.serial },
        { "rms", t.rms },
        { "strength", t.strength },
        { "use_count", t.useCount },
        { "generation", t.generation },
        { "merge_count", t.mergeCount },
        { "clamped", t.clamped },
        { "features", setValues (t.features, 0) }, // the Spectrum set (the original address)
    };
    json sets = json::object();
    for (int s = 1; s < kNumSets; ++s)
        sets[setKey (s)] = setValues (t.features, s);
    j["sets"] = std::move (sets);
    // Sequential context (optional; older files have none).
    if (std::any_of (t.context.begin(), t.context.end(), [] (float v) { return v != 0.0f; }))
    {
        j["context"] = setValues (t.context, 0);
        json cs = json::object();
        for (int s = 1; s < kNumSets; ++s)
            cs[setKey (s)] = setValues (t.context, s);
        j["context_sets"] = std::move (cs);
    }
    return j;
}

void readTraceMeta (const json& j, TraceRecord& t)
{
    t.begin = j.at ("begin").get<int64_t>();
    t.nominalLength = j.value ("nominal_length", 0.0);
    t.serial = j.value ("serial", uint64_t { 0 });
    t.rms = j.value ("rms", 0.0f);
    t.strength = j.value ("strength", 1.0f);
    t.useCount = j.value ("use_count", 0.0f);
    t.generation = j.value ("generation", 0);
    t.mergeCount = std::max (1, j.value ("merge_count", 1));
    t.clamped = j.value ("clamped", false);
    t.features.fill (0.0f);
    t.context.fill (0.0f);
    readSet (j.at ("features"), t.features, 0, "features");
    if (j.contains ("context"))
        readSet (j.at ("context"), t.context, 0, "context");
    t.hasSets = j.contains ("sets");
    if (t.hasSets)
    {
        const auto& sets = j.at ("sets");
        const json none = json::object();
        const auto& csets = j.contains ("context_sets") ? j.at ("context_sets") : none;
        for (int s = 1; s < kNumSets; ++s)
        {
            if (sets.contains (setKey (s)))
                readSet (sets.at (setKey (s)), t.features, s, setKey (s));
            if (csets.contains (setKey (s)))
                readSet (csets.at (setKey (s)), t.context, s, setKey (s));
        }
    }
}

json manifest (const MemorySnapshot& s, bool withFiles)
{
    json params = json::object();
    for (const auto& [id, value] : s.params)
        params[id] = value;

    json traces = json::array();
    for (size_t i = 0; i < s.traces.size(); ++i)
    {
        auto m = traceMeta (s.traces[i]);
        if (withFiles)
            m["file"] = traceFileName (i);
        traces.push_back (std::move (m));
    }
    return json {
        { "format", "MINERVA Space Echo memory" },
        { "version", MemorySnapshot::kFormatVersion },
        { "sample_rate", s.sampleRate },
        { "channels", s.channels },
        { "capacity", s.capacity },
        { "feature_slots", kSlots },
        { "feature_bands", kBands },
        { "params", params },
        { "traces", traces },
    };
}

MemorySnapshot fromManifest (const json& m)
{
    if (m.value ("version", 0) > MemorySnapshot::kFormatVersion)
        throw std::runtime_error ("memory was saved by a newer version");
    if (m.value ("feature_slots", kSlots) != kSlots || m.value ("feature_bands", kBands) != kBands)
        throw std::runtime_error ("memory uses an incompatible feature layout");

    MemorySnapshot s;
    s.sampleRate = m.at ("sample_rate").get<double>();
    s.channels = std::clamp (m.at ("channels").get<int>(), 1, 2);
    s.capacity = std::max (1, m.value ("capacity", 100));
    const json params = m.value ("params", json::object()); // keep alive: items() doesn't own it
    for (const auto& [id, value] : params.items())
        s.params.emplace_back (id, value.get<float>());
    for (const auto& jt : m.at ("traces"))
    {
        TraceRecord t;
        readTraceMeta (jt, t);
        s.traces.push_back (std::move (t));
    }
    return s;
}

} // namespace

// ---- folder -------------------------------------------------------------------

void writeMemoryFolder (const MemorySnapshot& s, const std::string& dir)
{
    fs::create_directories (dir);
    for (size_t i = 0; i < s.traces.size(); ++i)
    {
        AudioBuffer b;
        b.sampleRate = s.sampleRate;
        b.channels = s.traces[i].audio;
        writeWav ((fs::path (dir) / traceFileName (i)).string(), b, WavFormat::Float32);
    }
    std::ofstream out (fs::path (dir) / "manifest.json");
    if (! out)
        throw std::runtime_error ("cannot write manifest in " + dir);
    out << manifest (s, true).dump (1);
}

MemorySnapshot readMemoryFolder (const std::string& dir)
{
    std::ifstream in (fs::path (dir) / "manifest.json");
    if (! in)
        throw std::runtime_error ("no manifest.json in " + dir);
    json m;
    try
    {
        m = json::parse (in);
    }
    catch (const json::exception& e)
    {
        throw std::runtime_error (std::string ("bad manifest: ") + e.what());
    }

    auto s = fromManifest (m);
    const auto& jt = m.at ("traces");
    for (size_t i = 0; i < s.traces.size(); ++i)
    {
        const auto file = jt[i].value ("file", traceFileName (i));
        auto b = readWav ((fs::path (dir) / file).string());
        if (std::abs (b.sampleRate - s.sampleRate) > 0.5)
            throw std::runtime_error (file + " has a different sample rate from the manifest");
        s.traces[i].audio = std::move (b.channels);
    }
    completeAddressSets (s);
    return s;
}

// ---- blob -----------------------------------------------------------------------

std::vector<uint8_t> serializeMemory (const MemorySnapshot& s)
{
    const std::string meta = manifest (s, false).dump();
    std::vector<uint8_t> out;
    size_t audioBytes = 0;
    for (const auto& t : s.traces)
        for (const auto& ch : t.audio)
            audioBytes += ch.size() * sizeof (float);
    out.reserve (8 + meta.size() + audioBytes);

    out.insert (out.end(), kMagic, kMagic + 4);
    const auto metaLen = static_cast<uint32_t> (meta.size());
    for (int i = 0; i < 4; ++i)
        out.push_back (static_cast<uint8_t> ((metaLen >> (8 * i)) & 0xff));
    out.insert (out.end(), meta.begin(), meta.end());

    for (const auto& t : s.traces)
        for (const auto& ch : t.audio)
        {
            const auto* bytes = reinterpret_cast<const uint8_t*> (ch.data());
            out.insert (out.end(), bytes, bytes + ch.size() * sizeof (float)); // little-endian hosts only (macOS/Linux x86/arm)
        }
    return out;
}

MemorySnapshot deserializeMemory (const uint8_t* data, size_t size)
{
    if (size < 8 || std::memcmp (data, kMagic, 4) != 0)
        throw std::runtime_error ("not a memory blob");
    uint32_t metaLen = 0;
    for (int i = 0; i < 4; ++i)
        metaLen |= static_cast<uint32_t> (data[4 + i]) << (8 * i);
    if (8 + static_cast<size_t> (metaLen) > size)
        throw std::runtime_error ("truncated memory blob");

    json m;
    try
    {
        m = json::parse (data + 8, data + 8 + metaLen);
    }
    catch (const json::exception& e)
    {
        throw std::runtime_error (std::string ("bad memory blob: ") + e.what());
    }
    auto s = fromManifest (m);

    size_t pos = 8 + metaLen;
    const auto& jt = m.at ("traces");
    for (size_t i = 0; i < s.traces.size(); ++i)
    {
        const auto len = static_cast<size_t> (jt[i].at ("length").get<int64_t>());
        auto& t = s.traces[i];
        t.audio.assign (static_cast<size_t> (s.channels), std::vector<float> (len));
        for (auto& ch : t.audio)
        {
            const size_t bytes = len * sizeof (float);
            if (pos + bytes > size)
                throw std::runtime_error ("truncated memory blob");
            std::memcpy (ch.data(), data + pos, bytes);
            pos += bytes;
        }
    }
    completeAddressSets (s);
    return s;
}

void completeAddressSets (MemorySnapshot& s)
{
    FeatureSettings settings;
    for (const auto& [id, value] : s.params)
    {
        if (id == "feature_mode")
            settings.mode = value >= 0.5f ? FeatureMode::Ternary : FeatureMode::Continuous;
        else if (id == "ternary_threshold")
            settings.ternaryThreshold = value;
    }

    FeatureExtractor fx;
    std::vector<float> mono;
    for (size_t i = 0; i < s.traces.size(); ++i)
    {
        auto& t = s.traces[i];
        if (t.hasSets)
            continue;
        fx.prepare (s.sampleRate); // each trace on its own, as import does
        const auto n = static_cast<size_t> (t.length());
        mono.assign (n, 0.0f);
        for (const auto& ch : t.audio)
            for (size_t k = 0; k < n; ++k)
                mono[k] += ch[k] / static_cast<float> (t.audio.size());
        const double nominal = t.nominalLength > 0.0 ? t.nominalLength : static_cast<double> (t.begin + t.length());
        fx.beginSegment (nominal);
        if (n > 0)
            fx.push (mono.data(), static_cast<int> (n), t.begin);
        FeatureVector computed {};
        fx.finalize (computed, settings);
        std::copy (computed.begin() + setOffset (1), computed.end(), t.features.begin() + setOffset (1));

        const bool hasContext = std::any_of (t.context.begin(), t.context.begin() + kSetSize, [] (float v) { return v != 0.0f; });
        if (hasContext && i > 0)
        {
            const auto& prev = s.traces[i - 1].features;
            std::copy (prev.begin() + setOffset (1), prev.end(), t.context.begin() + setOffset (1));
        }
        t.hasSets = true;
    }
}

// ---- resampling -------------------------------------------------------------------

void resampleSnapshot (MemorySnapshot& s, double newRate)
{
    if (newRate <= 0.0 || std::abs (newRate - s.sampleRate) < 0.5)
        return;
    const double ratio = newRate / s.sampleRate;
    for (auto& t : s.traces)
    {
        t.begin = static_cast<int64_t> (std::llround (static_cast<double> (t.begin) * ratio));
        t.nominalLength *= ratio;
        for (auto& ch : t.audio)
        {
            const size_t n = ch.size();
            const auto m = static_cast<size_t> (std::llround (static_cast<double> (n) * ratio));
            std::vector<float> out (m);
            for (size_t i = 0; i < m; ++i)
            {
                const double pos = static_cast<double> (i) / ratio;
                const auto i0 = static_cast<size_t> (pos);
                const double frac = pos - static_cast<double> (i0);
                const float a = i0 < n ? ch[i0] : 0.0f;
                const float b = i0 + 1 < n ? ch[i0 + 1] : 0.0f;
                out[i] = static_cast<float> (a + (b - a) * frac);
            }
            ch = std::move (out);
        }
    }
    s.sampleRate = newRate;
}

} // namespace mse
