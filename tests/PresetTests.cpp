#include "Preset.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

TEST_CASE ("Assignments set known parameters")
{
    mse::EngineParams p;
    mse::tools::applyAssignment (p, " output_gain_db = -3.5 ");
    REQUIRE (p.outputGainDb == -3.5f);
}

TEST_CASE ("Unknown keys and bad values are rejected")
{
    mse::EngineParams p;
    REQUIRE_THROWS (mse::tools::applyAssignment (p, "nope=1"));
    REQUIRE_THROWS (mse::tools::applyAssignment (p, "output_gain_db=loud"));
    REQUIRE_THROWS (mse::tools::applyAssignment (p, "output_gain_db"));
}

TEST_CASE ("Preset files support comments and blank lines")
{
    const auto path = (std::filesystem::temp_directory_path() / "mse_preset_test.txt").string();
    {
        std::ofstream f (path);
        f << "# a comment\n\noutput_gain_db = -12   # trailing comment\n";
    }
    mse::EngineParams p;
    mse::tools::applyPresetFile (p, path);
    REQUIRE (p.outputGainDb == -12.0f);
    std::filesystem::remove (path);
}
