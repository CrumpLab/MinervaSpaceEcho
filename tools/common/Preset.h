#pragma once

#include "mse/EchoEngine.h"

#include <string>
#include <vector>

namespace mse::tools {

// Engine parameters are addressed by the same IDs the plugin uses, so a
// preset file behaves the same offline and in Live.
//
// Preset file format: one `key = value` per line, `#` starts a comment.

// Sets one parameter. Throws std::runtime_error on an unknown key or bad value.
void applyParam (EngineParams& params, const std::string& key, const std::string& value);

// Parses `key=value`. Throws on malformed input.
void applyAssignment (EngineParams& params, const std::string& assignment);

void applyPresetFile (EngineParams& params, const std::string& path);

// All known parameter IDs, for --help output.
std::vector<std::string> paramIds();

} // namespace mse::tools
