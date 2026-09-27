#include "mse/Preset.h"

#include <sstream>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace mse {

namespace {

std::string trim (const std::string& s)
{
    const auto b = s.find_first_not_of (" \t\r\n");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of (" \t\r\n");
    return s.substr (b, e - b + 1);
}

std::string lower (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    return s;
}

bool parseNumber (const std::string& text, float& out)
{
    try
    {
        size_t used = 0;
        out = std::stof (text, &used);
        return used == text.size();
    }
    catch (const std::exception&)
    {
        return false;
    }
}

} // namespace

void applyParam (ParamValues& values, const std::string& key, const std::string& rawValue)
{
    const int index = findParam (key);
    if (index < 0)
        throw std::runtime_error ("unknown parameter: " + key);

    const auto& spec = paramSpecs()[static_cast<size_t> (index)];
    const std::string value = trim (rawValue);
    const std::string lv = lower (value);
    float v = 0.0f;
    bool ok = false;

    if (spec.type == ParamType::Bool)
    {
        if (lv == "on" || lv == "true" || lv == "1") { v = 1.0f; ok = true; }
        else if (lv == "off" || lv == "false" || lv == "0") { v = 0.0f; ok = true; }
    }
    else if (spec.type == ParamType::Choice)
    {
        for (int c = 0; c < spec.numChoices && ! ok; ++c)
            if (lv == lower (spec.choices[c])) { v = static_cast<float> (c); ok = true; }
        if (! ok && parseNumber (value, v))
            ok = v == static_cast<float> (static_cast<int> (v)) && v >= 0.0f && v < static_cast<float> (spec.numChoices);
    }
    else
    {
        ok = parseNumber (value, v) && v >= spec.min && v <= spec.max
             && (spec.type != ParamType::Int || v == static_cast<float> (static_cast<int> (v)));
    }

    if (! ok)
        throw std::runtime_error ("bad value for " + key + ": '" + value + "'");
    values[static_cast<size_t> (index)] = v;
}

void applyAssignment (ParamValues& values, const std::string& assignment)
{
    const auto eq = assignment.find ('=');
    if (eq == std::string::npos)
        throw std::runtime_error ("expected key=value, got '" + assignment + "'");
    applyParam (values, trim (assignment.substr (0, eq)), assignment.substr (eq + 1));
}

Command parseCommand (const std::string& raw)
{
    const std::string name = lower (trim (raw));
    if (name == "capture") return Command::Capture;
    if (name == "clamp_last") return Command::ClampLast;
    if (name == "clamp_all") return Command::ClampAll;
    if (name == "unclamp_all") return Command::UnclampAll;
    if (name == "clear_unclamped") return Command::ClearUnclamped;
    if (name == "clear_all") return Command::ClearAll;
    throw std::runtime_error ("unknown command: " + raw);
}

void applyPresetText (ParamValues& values, const std::string& text, std::vector<TimedAssignment>* timed,
                      const std::string& source)
{
    std::istringstream in (text);
    std::string line;
    int lineNo = 0;
    while (std::getline (in, line))
    {
        ++lineNo;
        if (const auto hash = line.find ('#'); hash != std::string::npos)
            line.erase (hash);
        line = trim (line);
        if (line.empty())
            continue;
        try
        {
            if (line[0] == '@')
            {
                if (timed == nullptr)
                    throw std::runtime_error ("timed lines are not supported here");
                const auto space = line.find_first_of (" \t");
                const auto eq = line.find ('=');
                if (space == std::string::npos || eq == std::string::npos || eq < space)
                    throw std::runtime_error ("expected '@<bar> key = value'");
                TimedAssignment t;
                t.bar = std::stod (line.substr (1, space - 1));
                t.key = trim (line.substr (space, eq - space));
                t.value = trim (line.substr (eq + 1));
                if (t.key == "command")
                    parseCommand (t.value);
                else
                {
                    auto scratch = values;
                    applyParam (scratch, t.key, t.value); // validate now
                }
                timed->push_back (t);
            }
            else
            {
                applyAssignment (values, line);
            }
        }
        catch (const std::exception& e)
        {
            throw std::runtime_error (source + ":" + std::to_string (lineNo) + ": " + e.what());
        }
    }
}

void applyPresetFile (ParamValues& values, const std::string& path, std::vector<TimedAssignment>* timed)
{
    std::ifstream in (path);
    if (! in)
        throw std::runtime_error ("cannot open preset " + path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    applyPresetText (values, buffer.str(), timed, path);
}

std::string presetDescription (const std::string& text)
{
    std::istringstream in (text);
    std::string line, out;
    while (std::getline (in, line))
    {
        const auto t = trim (line);
        if (t.empty() || t[0] != '#')
            break;
        const auto body = trim (t.substr (1));
        out += (out.empty() ? "" : " ") + body;
    }
    return out;
}

bool isSessionParam (int index)
{
    switch (index)
    {
        case kCapture:
        case kTriggerClampLast: case kTriggerClampAll: case kTriggerUnclampAll:
        case kTriggerClearUnclamped: case kTriggerClearAll:
        case kRunning:
        case kMemoryBudget: case kEmbedMemory: case kMidiControl: case kMidiChannel: case kMidiBaseNote:
            return true;
        default:
            return false;
    }
}

std::string writePreset (const ParamValues& values, const std::string& description)
{
    std::string out;
    std::istringstream desc (description);
    std::string line;
    while (std::getline (desc, line))
        out += "# " + line + "\n";

    const auto defaults = defaultParamValues();
    const auto& specs = paramSpecs();
    char buf[64];
    for (size_t i = 0; i < specs.size(); ++i)
    {
        if (isSessionParam (static_cast<int> (i)) || values[i] == defaults[i])
            continue;
        const auto& s = specs[i];
        std::string value;
        if (s.type == ParamType::Choice || s.type == ParamType::Bool)
            value = s.choices[std::clamp (static_cast<int> (std::lround (values[i])), 0, s.numChoices - 1)];
        else if (s.type == ParamType::Int)
            value = std::to_string (static_cast<int> (std::lround (values[i])));
        else
        {
            std::snprintf (buf, sizeof (buf), "%.6g", static_cast<double> (values[i]));
            value = buf;
        }
        if (s.type == ParamType::Bool)
            value = values[i] > 0.5f ? "on" : "off";
        out += std::string (s.id) + " = " + value + "\n";
    }
    return out;
}

std::string describeParams()
{
    std::string out;
    char buf[256];
    for (const auto& s : paramSpecs())
    {
        if (s.type == ParamType::Choice || s.type == ParamType::Bool)
        {
            std::string choices;
            for (int c = 0; c < s.numChoices; ++c)
                choices += (c ? " | " : "") + std::string (s.choices[c]);
            std::snprintf (buf, sizeof (buf), "  %-18s %s  (default: %s)\n", s.id, choices.c_str(),
                           s.choices[static_cast<int> (s.def)]);
        }
        else
        {
            std::snprintf (buf, sizeof (buf), "  %-18s %g .. %g %s  (default: %g)\n", s.id, static_cast<double> (s.min),
                           static_cast<double> (s.max), s.unit, static_cast<double> (s.def));
        }
        out += buf;
    }
    return out;
}

} // namespace mse
