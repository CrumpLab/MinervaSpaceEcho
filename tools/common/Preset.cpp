#include "Preset.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace mse::tools {

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

void applyPresetFile (ParamValues& values, const std::string& path)
{
    std::ifstream in (path);
    if (! in)
        throw std::runtime_error ("cannot open preset " + path);

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
            applyAssignment (values, line);
        }
        catch (const std::exception& e)
        {
            throw std::runtime_error (path + ":" + std::to_string (lineNo) + ": " + e.what());
        }
    }
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

} // namespace mse::tools
