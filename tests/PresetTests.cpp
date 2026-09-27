#include "mse/Preset.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using namespace mse;

TEST_CASE ("Assignments set known parameters")
{
    auto v = defaultParamValues();
    applyAssignment (v, " output_gain_db = -3.5 ");
    REQUIRE (v[kOutputGainDb] == -3.5f);

    applyAssignment (v, "sync_mode = Free");
    applyAssignment (v, "trace_division=2 bars");
    applyAssignment (v, "self_match=off");
    applyAssignment (v, "normalization=2");
    applyAssignment (v, "capacity=8");
    const auto p = paramsFromValues (v);
    REQUIRE (p.syncMode == SyncMode::Free);
    REQUIRE (p.traceDivision == TraceDivision::Bar2);
    REQUIRE_FALSE (p.selfMatch);
    REQUIRE (p.normalization == Normalization::Familiarity);
    REQUIRE (p.capacity == 8);
}

TEST_CASE ("Unknown keys and bad values are rejected")
{
    auto v = defaultParamValues();
    REQUIRE_THROWS (applyAssignment (v, "nope=1"));
    REQUIRE_THROWS (applyAssignment (v, "output_gain_db=loud"));
    REQUIRE_THROWS (applyAssignment (v, "output_gain_db"));
    REQUIRE_THROWS (applyAssignment (v, "output_gain_db=100"));   // out of range
    REQUIRE_THROWS (applyAssignment (v, "capacity=2.5"));         // not an integer
    REQUIRE_THROWS (applyAssignment (v, "sync_mode=Sometimes"));
}

TEST_CASE ("Preset files support comments and blank lines")
{
    const auto path = (std::filesystem::temp_directory_path() / "mse_preset_test.txt").string();
    {
        std::ofstream f (path);
        f << "# a comment\n\noutput_gain_db = -12   # trailing comment\npower = 9\n";
    }
    auto v = defaultParamValues();
    applyPresetFile (v, path);
    REQUIRE (v[kOutputGainDb] == -12.0f);
    REQUIRE (v[kPower] == 9.0f);
    std::filesystem::remove (path);
}

TEST_CASE ("Parameter table defaults match EngineParams defaults")
{
    REQUIRE (paramsFromValues (defaultParamValues()) == EngineParams {});
}

TEST_CASE ("Parameter table IDs are unique and every choice default is valid")
{
    const auto& specs = paramSpecs();
    for (size_t i = 0; i < specs.size(); ++i)
    {
        INFO (specs[i].id);
        REQUIRE (findParam (specs[i].id) == static_cast<int> (i));
        REQUIRE (specs[i].def >= specs[i].min);
        REQUIRE (specs[i].def <= specs[i].max);
        if (specs[i].type == ParamType::Choice || specs[i].type == ParamType::Bool)
            REQUIRE (specs[i].max == static_cast<float> (specs[i].numChoices - 1));
    }
}

TEST_CASE ("Preset files can schedule parameter changes and commands at bars")
{
    const auto path = (std::filesystem::temp_directory_path() / "mse_preset_timed.txt").string();
    {
        std::ofstream f (path);
        f << "power = 5\n@8 freeze = on\n@2.5 command = clamp_all\n";
    }
    auto v = defaultParamValues();
    std::vector<TimedAssignment> timed;
    applyPresetFile (v, path, &timed);
    REQUIRE (v[kPower] == 5.0f);
    REQUIRE (v[kFreeze] == 0.0f); // timed lines don't apply immediately
    REQUIRE (timed.size() == 2);
    REQUIRE (timed[0].bar == 8.0);
    REQUIRE (timed[1].bar == 2.5);
    REQUIRE (parseCommand (timed[1].value) == Command::ClampAll);

    REQUIRE_THROWS (applyPresetFile (v, path)); // timed lines need a receiver
    {
        std::ofstream f (path);
        f << "@4 command = dance\n";
    }
    REQUIRE_THROWS (applyPresetFile (v, path, &timed));
    std::filesystem::remove (path);
}
