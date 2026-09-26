FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.15
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

set(MSE_PLUGIN_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND MSE_PLUGIN_FORMATS AU)
endif()

option(MSE_COPY_PLUGIN_AFTER_BUILD "Install the plugin into the user plug-in folders after building" OFF)

juce_add_plugin(MinervaSpaceEcho
    PRODUCT_NAME "MINERVA Space Echo"
    COMPANY_NAME "CrumpLab"
    BUNDLE_ID "com.crumplab.minervaspaceecho"
    PLUGIN_MANUFACTURER_CODE Crmp
    PLUGIN_CODE MnSE
    FORMATS ${MSE_PLUGIN_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD ${MSE_COPY_PLUGIN_AFTER_BUILD}
    VST3_CATEGORIES Fx Delay
    AU_MAIN_TYPE kAudioUnitType_Effect
)

target_sources(MinervaSpaceEcho PRIVATE
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
)

target_compile_definitions(MinervaSpaceEcho PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(MinervaSpaceEcho
    PRIVATE
        mse::engine
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Headless plugin-state test (parameters + embedded memory round trip).
if(MSE_BUILD_TESTS)
    add_executable(mse_plugin_state_test tests/StateTest.cpp)
    target_link_libraries(mse_plugin_state_test PRIVATE MinervaSpaceEcho)
    # Reuse the plugin's JUCE build (headers and settings) instead of compiling JUCE again.
    target_include_directories(mse_plugin_state_test PRIVATE $<TARGET_PROPERTY:MinervaSpaceEcho,INCLUDE_DIRECTORIES>)
    target_compile_definitions(mse_plugin_state_test PRIVATE $<TARGET_PROPERTY:MinervaSpaceEcho,COMPILE_DEFINITIONS>)
    target_compile_features(mse_plugin_state_test PRIVATE cxx_std_20)
    add_test(NAME plugin_state_round_trip COMMAND mse_plugin_state_test)
endif()
