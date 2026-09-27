cmake_minimum_required(VERSION 3.22)

# Universal binaries by default on macOS (must be set before project()).
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")
endif()
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(MinervaSpaceEcho VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MSE_BUILD_PLUGIN "Build the JUCE plugin (fetches JUCE)" ON)
option(MSE_BUILD_TOOLS  "Build the offline render and test-audio tools" ON)
option(MSE_BUILD_TESTS  "Build unit tests (fetches Catch2)" ON)

include(FetchContent)

add_subdirectory(engine)

if(MSE_BUILD_TOOLS OR MSE_BUILD_TESTS)
    add_subdirectory(tools)
endif()

if(MSE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MSE_BUILD_PLUGIN)
    add_subdirectory(plugin)
endif()
