# Shared helpers (WAV I/O, presets) and the deterministic test-signal library.
add_library(mse_tools_common STATIC
    testgen/TestSignals.cpp
)
target_include_directories(mse_tools_common PUBLIC testgen)
target_link_libraries(mse_tools_common PUBLIC mse::engine)

if(MSE_BUILD_TOOLS)
    add_executable(mse-testgen testgen/main.cpp)
    target_link_libraries(mse-testgen PRIVATE mse_tools_common)

    add_executable(mse-render render/main.cpp)
    target_link_libraries(mse-render PRIVATE mse_tools_common)
endif()
