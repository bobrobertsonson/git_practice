cmake_minimum_required(VERSION 3.22)

project(SubForge VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SUBFORGE_BUILD_PLUGIN "Build the JUCE plugin (AU on macOS, VST3 + Standalone everywhere)" ON)
option(SUBFORGE_BUILD_TESTS "Build the offline DSP tests" ON)

# The sound engine has no JUCE dependency so it can be tested anywhere.
add_library(subforge_dsp INTERFACE)
target_include_directories(subforge_dsp INTERFACE Source/dsp)

if(SUBFORGE_BUILD_TESTS)
    enable_testing()
    add_executable(subforge_tests tests/dsp_tests.cpp)
    target_link_libraries(subforge_tests PRIVATE subforge_dsp)
    add_test(NAME subforge_dsp_tests COMMAND subforge_tests)
endif()

if(SUBFORGE_BUILD_PLUGIN)
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.9
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)

    set(SUBFORGE_FORMATS VST3 Standalone)
    if(APPLE)
        list(PREPEND SUBFORGE_FORMATS AU)
    endif()

    juce_add_plugin(SubForge
        COMPANY_NAME "SubForge"
        BUNDLE_ID com.subforge.subforge
        PRODUCT_NAME "SubForge"
        PLUGIN_MANUFACTURER_CODE Sbfg
        PLUGIN_CODE Sfg1
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        AU_MAIN_TYPE kAudioUnitType_MusicDevice
        VST3_CATEGORIES Instrument Synth
        COPY_PLUGIN_AFTER_BUILD ${APPLE}
        FORMATS ${SUBFORGE_FORMATS})

    target_sources(SubForge PRIVATE
        Source/plugin/PluginProcessor.cpp
        Source/plugin/PluginEditor.cpp)

    target_include_directories(SubForge PRIVATE Source/plugin)

    target_compile_definitions(SubForge PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)

    target_link_libraries(SubForge
        PRIVATE
            subforge_dsp
            juce::juce_audio_utils
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)

    # SubForge Duck: effect for the bass track, keyed by SubForge.
    juce_add_plugin(SubForgeDuck
        COMPANY_NAME "SubForge"
        BUNDLE_ID com.subforge.subforgeduck
        PRODUCT_NAME "SubForge Duck"
        PLUGIN_MANUFACTURER_CODE Sbfg
        PLUGIN_CODE Sfd1
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        AU_MAIN_TYPE kAudioUnitType_Effect
        VST3_CATEGORIES Fx Dynamics
        COPY_PLUGIN_AFTER_BUILD ${APPLE}
        FORMATS ${SUBFORGE_FORMATS})

    target_sources(SubForgeDuck PRIVATE
        Source/duck/DuckProcessor.cpp
        Source/duck/DuckEditor.cpp)

    target_include_directories(SubForgeDuck PRIVATE Source/duck)

    target_compile_definitions(SubForgeDuck PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

    target_link_libraries(SubForgeDuck
        PRIVATE
            subforge_dsp
            juce::juce_audio_utils
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endif()
