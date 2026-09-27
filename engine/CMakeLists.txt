# The MINERVA engine: plain C++, no framework dependency, so it can be built,
# tested and used for offline renders anywhere.
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(mse_engine STATIC
    src/EchoEngine.cpp
    src/Features.cpp
    src/MemorySnapshot.cpp
    src/Params.cpp
    src/Retrieval.cpp
    src/Spectral.cpp
    src/Tape.cpp
    src/TraceStore.cpp
    src/Wav.cpp
)
add_library(mse::engine ALIAS mse_engine)

target_include_directories(mse_engine PUBLIC include)
target_link_libraries(mse_engine PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(mse_engine PUBLIC
    MSE_VERSION_STRING="${PROJECT_VERSION}"
)

if(MSVC)
    target_compile_options(mse_engine PRIVATE /W4)
else()
    target_compile_options(mse_engine PRIVATE -Wall -Wextra -Wpedantic)
endif()
