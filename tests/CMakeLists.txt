FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(mse_tests
    EngineTests.cpp
    WavTests.cpp
    TestSignalTests.cpp
    PresetTests.cpp
    ModelTests.cpp
    MemoryTests.cpp
    CueTests.cpp
    HeadsTests.cpp
)
target_link_libraries(mse_tests PRIVATE mse_tools_common Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(mse_tests)
