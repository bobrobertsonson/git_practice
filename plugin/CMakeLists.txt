# Sawblade JUCE plugin (SAWBLADE_BUILD_PLUGIN). JUCE 8 is AGPLv3 or commercial: a commercial
# JUCE licence is required before distributing this plugin (docs/THIRD_PARTY.md).
#
# Targets
#   SawbladePlugin         juce_add_plugin: the shared-code static library (our sources + the JUCE
#                          modules, compiled once) plus the format targets SawbladePlugin_VST3,
#                          SawbladePlugin_Standalone (and SawbladePlugin_AU on macOS).
#   sawblade_plugin_tests  headless Catch2 tests of the processor (no plugin wrapper, no window,
#                          no audio device, no host): our sources + the JUCE modules, compiled
#                          into the test executable.
#
# Sources (src/): Engine (Chain + real-time rate conversion), EngineLoader (background builds +
# lock-free swap), PresetMapping (parameters <-> preset), PluginProcessor, PluginEditor +
# SawbladeLookAndFeel (the replaceable look), PluginEntry (createPluginFilter).

# Sources shared by the plugin and the tests. JUCE's own pattern is followed: JUCE modules are
# compiled into every target that links them (so the plugin and the test executable each compile
# the modules once; format targets share the plugin's "shared code" library).
set(SAWBLADE_PLUGIN_SOURCES
  src/PresetMapping.cpp
  src/Engine.cpp
  src/EngineLoader.cpp
  src/PluginProcessor.cpp
  src/PluginEditor.cpp)

# JUCE module sources are compiled into the same targets: -Werror applies to OUR files only.
set_source_files_properties(${SAWBLADE_PLUGIN_SOURCES} src/PluginEntry.cpp PROPERTIES
  COMPILE_OPTIONS "$<TARGET_PROPERTY:sawblade_warnings,INTERFACE_COMPILE_OPTIONS>")

set(SAWBLADE_JUCE_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_STRICT_REFCOUNTEDPOINTER=1)

# Linux: AU is macOS-only. VST3 + Standalone build everywhere.
set(SAWBLADE_PLUGIN_FORMATS VST3 Standalone)
if(APPLE)
  list(APPEND SAWBLADE_PLUGIN_FORMATS AU)
endif()

juce_add_plugin(SawbladePlugin
  PRODUCT_NAME "Sawblade"
  COMPANY_NAME "Sawblade"
  PLUGIN_MANUFACTURER_CODE Swbl
  PLUGIN_CODE Swb1
  FORMATS ${SAWBLADE_PLUGIN_FORMATS}
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT FALSE
  NEEDS_MIDI_OUTPUT FALSE
  IS_MIDI_EFFECT FALSE
  VST3_CATEGORIES Fx Distortion
  AU_MAIN_TYPE kAudioUnitType_Effect
  COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(SawbladePlugin PRIVATE ${SAWBLADE_PLUGIN_SOURCES} src/PluginEntry.cpp)
target_include_directories(SawbladePlugin PUBLIC src)
target_compile_features(SawbladePlugin PUBLIC cxx_std_20)
target_compile_definitions(SawbladePlugin PUBLIC ${SAWBLADE_JUCE_DEFS})
target_link_libraries(SawbladePlugin
  PRIVATE sawblade_core
          juce::juce_audio_processors juce::juce_gui_basics juce::juce_data_structures juce::juce_events
          juce::juce_audio_utils
  PUBLIC  juce::juce_recommended_config_flags)
# NAM registers its architectures from static initializers: the archive must be linked whole into
# every final binary (the shared code is a static library, so the format targets and the tests
# add it themselves).
foreach(_fmt ${SAWBLADE_PLUGIN_FORMATS})
  target_link_libraries(SawbladePlugin_${_fmt} PRIVATE sawblade_core "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>" pffft_lib)
endforeach()

if(SAWBLADE_BUILD_TESTS)
  include(Catch)
  add_executable(sawblade_plugin_tests
    ${PROJECT_SOURCE_DIR}/tests/alloc_guard.cpp
    tests/lock_guard.cpp
    tests/test_engine.cpp
    tests/test_processor.cpp
    ${SAWBLADE_PLUGIN_SOURCES})
  set_source_files_properties(
    ${PROJECT_SOURCE_DIR}/tests/alloc_guard.cpp tests/lock_guard.cpp tests/test_engine.cpp tests/test_processor.cpp
    PROPERTIES COMPILE_OPTIONS "$<TARGET_PROPERTY:sawblade_warnings,INTERFACE_COMPILE_OPTIONS>")
  target_include_directories(sawblade_plugin_tests PRIVATE src)
  target_compile_definitions(sawblade_plugin_tests PRIVATE ${SAWBLADE_JUCE_DEFS})
  target_link_libraries(sawblade_plugin_tests PRIVATE
    sawblade_core Catch2::Catch2WithMain
    juce::juce_audio_processors juce::juce_gui_basics juce::juce_data_structures juce::juce_events
    juce::juce_recommended_config_flags
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>" pffft_lib)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Count mutex/rwlock acquisitions on the audio thread (tests/lock_guard.cpp).
    target_compile_definitions(sawblade_plugin_tests PRIVATE SAWBLADE_LOCK_WRAP=1)
    target_link_options(sawblade_plugin_tests PRIVATE
      -Wl,--wrap=pthread_mutex_lock -Wl,--wrap=pthread_mutex_trylock
      -Wl,--wrap=pthread_rwlock_rdlock -Wl,--wrap=pthread_rwlock_wrlock)
  endif()
  target_include_directories(sawblade_plugin_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
  target_compile_definitions(sawblade_plugin_tests PRIVATE
    SAWBLADE_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
    SAWBLADE_PRESETS_DIR="${PROJECT_SOURCE_DIR}/presets")
  catch_discover_tests(sawblade_plugin_tests TEST_PREFIX "plugin: ")
endif()

# Optional: validate the built VST3 with pluginval (https://github.com/Tracktion/pluginval).
# Not fetched or built here (it is a large JUCE application; see docs/PLUGIN.md). Point this at a
# pluginval binary to register the ctest; build the SawbladePlugin_VST3 target first. On Linux the
# GUI tests need a display: xvfb-run is used if present.
set(SAWBLADE_PLUGINVAL_EXECUTABLE "" CACHE FILEPATH "pluginval binary; if set, registers a ctest that validates the VST3")
if(SAWBLADE_PLUGINVAL_EXECUTABLE AND SAWBLADE_BUILD_TESTS AND "VST3" IN_LIST SAWBLADE_PLUGIN_FORMATS)
  set(_pv_prefix "")
  find_program(SAWBLADE_XVFB_RUN xvfb-run)
  if(SAWBLADE_XVFB_RUN AND NOT APPLE)
    set(_pv_prefix ${SAWBLADE_XVFB_RUN} -a)
  endif()
  add_test(NAME "plugin: pluginval VST3"
    COMMAND ${_pv_prefix} ${SAWBLADE_PLUGINVAL_EXECUTABLE} --strictness-level 10 --validate-in-process
            --timeout-ms 120000 "$<GENEX_EVAL:$<TARGET_PROPERTY:SawbladePlugin_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>")
endif()
