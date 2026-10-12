# RASP as a desktop plugin: Audio Unit (Logic Pro, GarageBand, Ableton on Mac),
# VST3 (most other hosts) and a standalone app. The DSP is the same
# dsp/rasp.h the pedal runs.
#
#   cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release        # fetches JUCE 8.0.4
#   cmake --build build
#
# On a Mac, add -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" for a universal build.
# Pass -DRASP_JUCE_DIR=/path/to/JUCE to use a local JUCE checkout instead.
cmake_minimum_required(VERSION 3.22)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
# CI passes its run number and commit, so every build shows which one it is
# (in the window header) and Logic sees a new AU version and rescans it.
set(RASP_BUILD_NUMBER 0 CACHE STRING "Build number (CI run number)")
set(RASP_BUILD_ID "local" CACHE STRING "Build id shown in the window (commit)")
project(RASP VERSION 0.2.${RASP_BUILD_NUMBER} LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(RASP_JUCE_DIR "" CACHE PATH "Local JUCE checkout (empty = download JUCE 8.0.4)")
if(RASP_JUCE_DIR)
    add_subdirectory(${RASP_JUCE_DIR} juce)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_plugin(RASP
    PRODUCT_NAME "RASP"
    COMPANY_NAME "RASP"
    BUNDLE_ID "com.rasp.vocal"
    PLUGIN_MANUFACTURER_CODE RspA
    PLUGIN_CODE Rsp1
    DESCRIPTION "Extreme vocal processor"
    FORMATS AU VST3 Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx Dynamics Distortion Reverb
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(RASP PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_include_directories(RASP PRIVATE ../dsp)

target_compile_definitions(RASP PUBLIC
    RASP_SR_SCALE=2                 # delay buffers big enough for 192 kHz sessions
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    RASP_BUILD_NUMBER=${RASP_BUILD_NUMBER}
    RASP_BUILD_ID="${RASP_BUILD_ID}")

target_link_libraries(RASP
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Headless test of the plugin class: cmake -DRASP_PLUGIN_TESTS=ON, then run
# build/plugin_test [snapshot.png]
option(RASP_PLUGIN_TESTS "Build the headless plugin test" OFF)
if(RASP_PLUGIN_TESTS)
    juce_add_console_app(plugin_test PRODUCT_NAME "plugin_test")
    target_sources(plugin_test PRIVATE tests/plugin_test.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
    target_include_directories(plugin_test PRIVATE Source ../dsp)
    target_compile_definitions(plugin_test PRIVATE RASP_SR_SCALE=2 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        RASP_BUILD_NUMBER=${RASP_BUILD_NUMBER} RASP_BUILD_ID="${RASP_BUILD_ID}")
    target_link_libraries(plugin_test PRIVATE juce::juce_audio_utils juce::juce_recommended_config_flags)
    set_target_properties(plugin_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()
