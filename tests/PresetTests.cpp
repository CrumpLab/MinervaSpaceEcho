#include "Preset.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using namespace mse;

TEST_CASE ("Assignments set known parameters")
{
    auto v = defaultParamValues();
    tools::applyAssignment (v, " output_gain_db = -3.5 ");
    REQUIRE (v[kOutputGainDb] == -3.5f);

    tools::applyAssignment (v, "sync_mode = Free");
    tools::applyAssignment (v, "trace_division=2 bars");
    tools::applyAssignment (v, "self_match=off");
    tools::applyAssignment (v, "normalization=2");
    tools::applyAssignment (v, "capacity=8");
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
    REQUIRE_THROWS (tools::applyAssignment (v, "nope=1"));
    REQUIRE_THROWS (tools::applyAssignment (v, "output_gain_db=loud"));
    REQUIRE_THROWS (tools::applyAssignment (v, "output_gain_db"));
    REQUIRE_THROWS (tools::applyAssignment (v, "output_gain_db=100"));   // out of range
    REQUIRE_THROWS (tools::applyAssignment (v, "capacity=2.5"));         // not an integer
    REQUIRE_THROWS (tools::applyAssignment (v, "sync_mode=Sometimes"));
}

TEST_CASE ("Preset files support comments and blank lines")
{
    const auto path = (std::filesystem::temp_directory_path() / "mse_preset_test.txt").string();
    {
        std::ofstream f (path);
        f << "# a comment\n\noutput_gain_db = -12   # trailing comment\npower = 9\n";
    }
    auto v = defaultParamValues();
    tools::applyPresetFile (v, path);
    REQUIRE (v[kOutputGainDb] == -12.0f);
    REQUIRE (v[kPower] == 9.0f);
    std::filesystem::remove (path);
}

TEST_CASE ("Parameter table defaults match EngineParams defaults")
{
    const auto fromTable = paramsFromValues (defaultParamValues());
    const EngineParams fromStruct;
    REQUIRE (fromTable.syncMode == fromStruct.syncMode);
    REQUIRE (fromTable.traceMs == fromStruct.traceMs);
    REQUIRE (fromTable.traceDivision == fromStruct.traceDivision);
    REQUIRE (fromTable.capacity == fromStruct.capacity);
    REQUIRE (fromTable.power == fromStruct.power);
    REQUIRE (fromTable.similarity == fromStruct.similarity);
    REQUIRE (fromTable.featureMode == fromStruct.featureMode);
    REQUIRE (fromTable.ternaryThreshold == fromStruct.ternaryThreshold);
    REQUIRE (fromTable.encodingFailure == fromStruct.encodingFailure);
    REQUIRE (fromTable.selfMatch == fromStruct.selfMatch);
    REQUIRE (fromTable.cueGateDb == fromStruct.cueGateDb);
    REQUIRE (fromTable.negativeMode == fromStruct.negativeMode);
    REQUIRE (fromTable.normalization == fromStruct.normalization);
    REQUIRE (fromTable.levelTracking == fromStruct.levelTracking);
    REQUIRE (fromTable.feedback == fromStruct.feedback);
    REQUIRE (fromTable.echoLevelDb == fromStruct.echoLevelDb);
    REQUIRE (fromTable.dryLevelDb == fromStruct.dryLevelDb);
    REQUIRE (fromTable.edgeFadeMs == fromStruct.edgeFadeMs);
    REQUIRE (fromTable.outputGainDb == fromStruct.outputGainDb);
}
