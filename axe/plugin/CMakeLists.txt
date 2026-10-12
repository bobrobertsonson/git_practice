# AxeElevator as a plugin. The DSP is the same dsp/axe.h the pedal will run.
#
#   macOS (Logic):   cmake -S axe/plugin -B build -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
#                    -> AU, VST3, Standalone
#   Linux:           cmake -S axe/plugin -B build -G Ninja
#                    -> LV2 (Darkglass juce-anagram-lv2 wrapper: control ports, lv2:enabled,
#                       kx:Reset, dg:abbreviation, block images) and VST3
#   Anagram / MOD:   inside Darkglass's Plugin-Dev-Setup docker image (see ../BUILD.md)
#                    -> LV2 only, cross-compiled for ARMv8.2 64-bit
#
#   cmake --build build
#
# Options: -DAXE_FORMATS="LV2"  override the format list
#          -DAXE_PLUGIN_TESTS=ON build the headless plugin test
#          -DAXE_JUCE_DIR=...   use a local JUCE checkout (apply the Darkglass
#                               cross-compilation patch yourself if you cross-compile)
cmake_minimum_required(VERSION 3.22...3.31)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
project(AxeElevator VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

if(APPLE)
    set(AXE_DEFAULT_FORMATS AU VST3 Standalone)
elseif(CMAKE_CROSSCOMPILING)
    set(AXE_DEFAULT_FORMATS LV2)
else()
    set(AXE_DEFAULT_FORMATS LV2 VST3)
endif()
set(AXE_FORMATS "${AXE_DEFAULT_FORMATS}" CACHE STRING "Plugin formats to build")
set(AXE_JUCE_DIR "" CACHE PATH "Local JUCE checkout (empty = download JUCE 8.0.9)")
option(AXE_PLUGIN_TESTS "Build the headless plugin test" OFF)

list(FIND AXE_FORMATS LV2 _lv2_index)
if(_lv2_index GREATER -1)
    set(AXE_LV2 ON)
else()
    set(AXE_LV2 OFF)
endif()

include(FetchContent)
if(AXE_LV2)
    # Darkglass's LV2 wrapper for JUCE (control ports instead of patch
    # parameters). Pinned to a known commit; bump deliberately.
    FetchContent_Declare(juce-anagram-lv2
        GIT_REPOSITORY https://github.com/Darkglass-Electronics/juce-anagram-lv2.git
        GIT_TAG abee7b448bd2b7d0f969dc241b02c7220b0ba111
        UPDATE_DISCONNECTED TRUE)
    FetchContent_MakeAvailable(juce-anagram-lv2)
endif()

if(AXE_JUCE_DIR)
    add_subdirectory(${AXE_JUCE_DIR} juce)
else()
    if(AXE_LV2)
        # The same JUCE version and cross-compilation patch as Darkglass's JUCE example.
        FetchContent_Declare(JUCE
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG 8.0.9
            GIT_SHALLOW TRUE
            PATCH_COMMAND patch -p1 -N -i "${JUCE_ANAGRAM_LV2_DIR}/cross-compilation-patches/v8.0.9.patch" || true
            UPDATE_DISCONNECTED TRUE)
    else()
        FetchContent_Declare(JUCE
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG 8.0.9
            GIT_SHALLOW TRUE)
    endif()
    FetchContent_MakeAvailable(JUCE)
endif()

# Placeholder identifiers (Snotch / Studio Notsch): change them before a
# release, and never after one (hosts key saved sessions on them).
juce_add_plugin(AxeElevator
    PRODUCT_NAME "AxeElevator"
    COMPANY_NAME "Snotch"
    COMPANY_WEBSITE "https://example.com/snotch"
    COMPANY_EMAIL "mailto:snotch@example.com"
    BUNDLE_ID "com.snotch.axeelevator"
    PLUGIN_MANUFACTURER_CODE Snch
    PLUGIN_CODE AxEv
    DESCRIPTION "Pre-amp DI enhancer for guitar and bass"
    FORMATS ${AXE_FORMATS}
    LV2URI "urn:snotch:axeelevator"     # (JUCE keyword is LV2URI; LV2_URI is silently ignored)
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx Distortion Dynamics
    COPY_PLUGIN_AFTER_BUILD FALSE)

if(AXE_LV2)
    juce_anagram_lv2_setup(AxeElevator
        CATEGORY "lv2:DistortionPlugin"
        BLOCK_IMAGE_OFF "images/block-off.png"
        BLOCK_IMAGE_ON "images/block-on.png"
        IS_FREEWARE)
endif()

target_sources(AxeElevator PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)
target_include_directories(AxeElevator PRIVATE ../dsp)

target_compile_definitions(AxeElevator PUBLIC
    AXE_SR_SCALE=2                  # delay buffers big enough for 192 kHz sessions
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_FLAC=0
    JUCE_USE_OGGVORBIS=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

list(FIND AXE_FORMATS Standalone _sa_index)
if(_sa_index GREATER -1)
    set(AXE_JUCE_LIBS juce::juce_audio_utils)
else()
    set(AXE_JUCE_LIBS juce::juce_audio_processors juce::juce_gui_basics)
endif()

target_link_libraries(AxeElevator
    PRIVATE
        ${AXE_JUCE_LIBS}
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if(UNIX AND NOT APPLE)
    # One self-contained .so: no libstdc++ version requirements on the target
    # (the MOD Dwarf ships GCC 7.5's runtime, the Anagram its own).
    target_link_options(AxeElevator PUBLIC -static-libstdc++ -static-libgcc)
endif()

if(AXE_PLUGIN_TESTS)
    juce_add_console_app(plugin_test PRODUCT_NAME "plugin_test")
    target_sources(plugin_test PRIVATE tests/plugin_test.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
    target_include_directories(plugin_test PRIVATE Source ../dsp)
    target_compile_definitions(plugin_test PRIVATE AXE_SR_SCALE=2 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1
        JucePlugin_Name="AxeElevator" JucePlugin_VersionString="0.1.0")
    target_link_libraries(plugin_test PRIVATE juce::juce_audio_processors juce::juce_gui_basics juce::juce_recommended_config_flags)
    set_target_properties(plugin_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()
