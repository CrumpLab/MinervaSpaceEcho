#pragma once

#include "mse/EchoEngine.h"
#include "mse/Params.h"

#include <string>
#include <vector>

namespace mse {

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

// Same, from preset text; `source` names it in error messages.
void applyPresetText (ParamValues& values, const std::string& text, std::vector<TimedAssignment>* timed = nullptr,
                      const std::string& source = "preset");

// The comment lines at the top of a preset (its description), without '#'.
std::string presetDescription (const std::string& text);

// Writes a preset: the description as comments, then every parameter that
// differs from its default (except memory actions and plugin-only settings).
std::string writePreset (const ParamValues& values, const std::string& description);

// True for parameters a preset never sets or resets: memory actions
// (capture, triggers) and plugin settings (memory budget, embedding, MIDI).
bool isSessionParam (int index);

// Parses a `command` value. Throws on an unknown command.
Command parseCommand (const std::string& name);

// Human-readable list of parameters, ranges and defaults (for --help).
std::string describeParams();

} // namespace mse
