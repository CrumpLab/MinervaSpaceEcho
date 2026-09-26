#pragma once

#include "mse/Params.h"

#include <string>
#include <vector>

namespace mse::tools {

// Parameters are addressed by the plugin's parameter IDs (mse::paramSpecs()),
// so a preset behaves the same offline and in Live.
//
// Preset file format: one `key = value` per line, `#` starts a comment.
// Choice parameters accept a choice name ("Tempo", "1 bar") or an index;
// on/off parameters accept on/off/true/false/1/0.

// Sets one parameter. Throws std::runtime_error on an unknown key or bad value.
void applyParam (ParamValues& values, const std::string& key, const std::string& value);

// Parses `key=value`. Throws on malformed input.
void applyAssignment (ParamValues& values, const std::string& assignment);

void applyPresetFile (ParamValues& values, const std::string& path);

// Human-readable list of parameters, ranges and defaults (for --help).
std::string describeParams();

} // namespace mse::tools
