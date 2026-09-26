# The MINERVA engine: plain C++, no framework dependency, so it can be built,
# tested and used for offline renders anywhere.
add_library(mse_engine STATIC
    src/EchoEngine.cpp
    src/Features.cpp
    src/Params.cpp
    src/Retrieval.cpp
    src/TraceStore.cpp
)
add_library(mse::engine ALIAS mse_engine)

target_include_directories(mse_engine PUBLIC include)
target_compile_definitions(mse_engine PUBLIC
    MSE_VERSION_STRING="${PROJECT_VERSION}"
)

if(MSVC)
    target_compile_options(mse_engine PRIVATE /W4)
else()
    target_compile_options(mse_engine PRIVATE -Wall -Wextra -Wpedantic)
endif()
