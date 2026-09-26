#pragma once

#include "mse/EchoEngine.h"
#include "mse/Params.h"

#include <string>
#include <vector>

namespace mse::tools {

// Parameters are addressed by the plugin's parameter IDs (mse::paramSpecs()),
// so a preset behaves the same offline and in Live.
//
// Preset file format: one `key = value` per line, `#` starts a comment.
// A line starting with `@<bar>` applies at that bar (4/4, counted from 0)
// instead of at the start, e.g. `@8 freeze = on` or `@2 command = clamp_all`.
// Commands: capture, clamp_last, clamp_all, unclamp_all, clear_unclamped, clear_all.
// Choice parameters accept a choice name ("Tempo", "1 bar") or an index;
// on/off parameters accept on/off/true/false/1/0.

// Sets one parameter. Throws std::runtime_error on an unknown key or bad value.
void applyParam (ParamValues& values, const std::string& key, const std::string& value);

// Parses `key=value`. Throws on malformed input.
void applyAssignment (ParamValues& values, const std::string& assignment);

struct TimedAssignment
{
    double bar = 0.0;
    std::string key, value;
};

// Applies untimed lines to `values`; timed lines are validated and returned
// in `timed` (or rejected if `timed` is null).
void applyPresetFile (ParamValues& values, const std::string& path, std::vector<TimedAssignment>* timed = nullptr);

// Parses a `command` value. Throws on an unknown command.
Command parseCommand (const std::string& name);

// Human-readable list of parameters, ranges and defaults (for --help).
std::string describeParams();

} // namespace mse::tools
