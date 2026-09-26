#include "Preset.h"

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

float parseFloat (const std::string& key, const std::string& value)
{
    try
    {
        size_t used = 0;
        const float f = std::stof (value, &used);
        if (used != value.size())
            throw std::invalid_argument ("trailing characters");
        return f;
    }
    catch (const std::exception&)
    {
        throw std::runtime_error ("bad value for " + key + ": '" + value + "'");
    }
}

} // namespace

void applyParam (EngineParams& params, const std::string& key, const std::string& value)
{
    if (key == "output_gain_db")
        params.outputGainDb = parseFloat (key, value);
    else
        throw std::runtime_error ("unknown parameter: " + key);
}

void applyAssignment (EngineParams& params, const std::string& assignment)
{
    const auto eq = assignment.find ('=');
    if (eq == std::string::npos)
        throw std::runtime_error ("expected key=value, got '" + assignment + "'");
    applyParam (params, trim (assignment.substr (0, eq)), trim (assignment.substr (eq + 1)));
}

void applyPresetFile (EngineParams& params, const std::string& path)
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
            applyAssignment (params, line);
        }
        catch (const std::exception& e)
        {
            throw std::runtime_error (path + ":" + std::to_string (lineNo) + ": " + e.what());
        }
    }
}

std::vector<std::string> paramIds()
{
    return { "output_gain_db" };
}

} // namespace mse::tools
