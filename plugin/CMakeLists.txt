# Sawblade JUCE plugin (SAWBLADE_BUILD_PLUGIN). JUCE 8 is AGPLv3 or commercial: a commercial
# JUCE licence is required before distributing this plugin (docs/THIRD_PARTY.md).
#
# Targets
#   SawbladePlugin         juce_add_plugin: the shared-code static library (our sources + the JUCE
#                          modules, compiled once) plus the format targets SawbladePlugin_VST3,
#                          SawbladePlugin_Standalone (and SawbladePlugin_AU on macOS).
#   sawblade_editor_tests  Catch2 tests of the skinned editor (needs a display: run under xvfb-run).
#   sawblade_plugin_tests  headless Catch2 tests of the processor (no plugin wrapper, no window,
#                          no audio device, no host): our sources + the JUCE modules, compiled
#                          into the test executable.
#
# Sources (src/): Engine (Chain + real-time rate conversion), EngineLoader (background builds +
# lock-free swap), PresetMapping (parameters <-> preset), PlayAlong (backing track), TakeRecorder (DI takes),
# JobRunner (match / export child processes, settings), PresetAudition + MatchGlue (audition / A-B / apply, what MATCH
# starts from), PluginProcessor, PluginEditor + PlayAlongPanel + MatchScreen + ExportPanel (NAM export, phase 12; ExportGlue + ExportSettings behind it) + SawbladeLookAndFeel (the replaceable look),
# PluginEntry (createPluginFilter).

# Sources shared by the plugin and the tests. JUCE's own pattern is followed: JUCE modules are
# compiled into every target that links them (so the plugin and the test executable each compile
# the modules once; format targets share the plugin's "shared code" library).
set(SAWBLADE_PLUGIN_SOURCES
  src/PresetMapping.cpp
  src/pedals/CircuitParams.cpp
  src/pedals/CircuitFaces.cpp
  src/pedals/PedalSwitch.cpp
  src/pedals/PedalFace.cpp
  src/pedals/AdvancedDrawer.cpp
  src/Engine.cpp
  src/EngineLoader.cpp
  src/PlayAlong.cpp
  src/browser/T3kJson.cpp
  src/browser/T3kRunner.cpp
  src/browser/T3kClient.cpp
  src/browser/BrowserSettings.cpp
  src/browser/SlotTarget.cpp
  src/browser/PreviewPlayer.cpp
  src/browser/PreviewRender.cpp
  src/browser/PreviewWorker.cpp
  src/browser/BrowserController.cpp
  src/browser/CaptureBrowser.cpp
  src/TakeRecorder.cpp
  src/JobRunner.cpp
  src/PresetAudition.cpp
  src/MatchGlue.cpp
  src/ExportSettings.cpp
  src/ExportGlue.cpp
  src/SongDecoder.cpp
  src/LadderFetch.cpp
  src/RungPreloader.cpp
  src/PluginProcessor.cpp
  src/PlayAlongPanel.cpp
  src/MatchScreen.cpp
  src/ExportPanel.cpp
  src/PluginEditor.cpp
  src/skin/SkinAssets.cpp
  src/skin/FilmstripKnob.cpp
  src/skin/FootswitchButton.cpp
  src/skin/LedIndicator.cpp
  src/skin/RigView.cpp
  src/settings/Settings.cpp
  src/settings/ToolRunner.cpp
  src/settings/SettingsPanel.cpp
  src/about/CaptureList.cpp
  src/about/AboutBox.cpp
  src/rig/RigModel.cpp
  src/rig/BodyFill.cpp
  src/rig/AmpHead.cpp
  src/rig/RigController.cpp
  src/rig/RigWidgets.cpp
  src/rig/SlotStrip.cpp
  src/rig/EqGraph.cpp
  src/rig/RigEditorPanel.cpp
  src/mic/IrNameParser.cpp
  src/mic/IrPack.cpp
  src/mic/IrResponse.cpp
  src/mic/MicSession.cpp
  src/mic/MicPage.cpp
  src/presets/T3kTool.cpp
  src/presets/PresetLibrary.cpp
  src/presets/PresetLoadFlow.cpp
  src/presets/AbCompare.cpp
  src/presets/PresetInfoPanel.cpp
  src/presets/PresetBrowser.cpp)

# BuildInfo.h (version, git sha, dirty flag, UTC date) for the About box and the Standalone title.
# Regenerated on EVERY build by the SawbladeBuildInfo target (cmake/GenBuildInfo.cmake), so it cannot go
# stale when HEAD moves without a reconfigure. The script only rewrites the header when its content changed,
# so an unchanged stamp recompiles nothing.
find_package(Git QUIET)
set(SAWBLADE_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(SAWBLADE_BUILDINFO_HEADER ${SAWBLADE_GENERATED_DIR}/BuildInfo.h)
set(SAWBLADE_BUILDINFO_ARGS
  -DSRC_DIR=${PROJECT_SOURCE_DIR}
  -DTEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/src/about/BuildInfo.h.in
  -DOUT=${SAWBLADE_BUILDINFO_HEADER}
  -DVERSION=${PROJECT_VERSION})
if(GIT_FOUND)
  list(APPEND SAWBLADE_BUILDINFO_ARGS -DGIT_EXECUTABLE=${GIT_EXECUTABLE})
endif()
# Also at configure time, so the header exists before the first build.
execute_process(COMMAND ${CMAKE_COMMAND} ${SAWBLADE_BUILDINFO_ARGS} -P ${PROJECT_SOURCE_DIR}/cmake/GenBuildInfo.cmake)
add_custom_target(SawbladeBuildInfo ALL
  COMMAND ${CMAKE_COMMAND} ${SAWBLADE_BUILDINFO_ARGS} -P ${PROJECT_SOURCE_DIR}/cmake/GenBuildInfo.cmake
  BYPRODUCTS ${SAWBLADE_BUILDINFO_HEADER}
  COMMENT "Stamping BuildInfo.h"
  VERBATIM)


# UI renders embedded into the binary: an explicit list (not a glob) of everything in assets/.
# Nothing renders at build time; design/render/export_ui_assets.py produced these files once.
juce_add_binary_data(SawbladeAssets SOURCES
  assets/amp_saw.png
  assets/amp_body.png
  assets/cab_4x12.png
  assets/cab_4x12_open.png
  assets/cab_4x12_open.json
  assets/cab_2x12_open.png
  assets/cab_2x12_open.json
  assets/pedal_saw.png
  assets/pedal_body.png
  assets/knob_amp.png
  assets/knob_amp.json
  assets/knob_pedal.png
  assets/knob_pedal.json
  assets/footswitch.png
  assets/footswitch.json
  assets/led_orange.png
  assets/led_orange.json
  assets/icon/icon_256.png
  ${PROJECT_SOURCE_DIR}/docs/THIRD_PARTY.md
  assets/preview_riff.wav)

set_source_files_properties(src/browser/BrowserSettings.cpp PROPERTIES COMPILE_DEFINITIONS "SAWBLADE_REPO_DIR=\"${PROJECT_SOURCE_DIR}\"")

# The default sawblade-t3k location (T3kTool): <repo>/match/.venv/bin/sawblade-t3k.
set_source_files_properties(src/presets/T3kTool.cpp PROPERTIES COMPILE_DEFINITIONS "SAWBLADE_REPO_DIR=\"${PROJECT_SOURCE_DIR}\"")

# JUCE module sources are compiled into the same targets: -Werror applies to OUR files only.
set_source_files_properties(${SAWBLADE_PLUGIN_SOURCES} src/PluginEntry.cpp PROPERTIES
  COMPILE_OPTIONS "$<TARGET_PROPERTY:sawblade_warnings,INTERFACE_COMPILE_OPTIONS>")

set(SAWBLADE_JUCE_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_STRICT_REFCOUNTEDPOINTER=1
  SAWBLADE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")  # default location of match/.venv/bin/sawblade-match (JobRunner.cpp)

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
  ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/icon_1024.png"
  ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/icon_256.png"
  COPY_PLUGIN_AFTER_BUILD FALSE)

add_dependencies(SawbladePlugin SawbladeBuildInfo)
target_sources(SawbladePlugin PRIVATE ${SAWBLADE_PLUGIN_SOURCES} src/PluginEntry.cpp)
target_include_directories(SawbladePlugin PUBLIC src ${SAWBLADE_GENERATED_DIR})
target_compile_features(SawbladePlugin PUBLIC cxx_std_20)
target_compile_definitions(SawbladePlugin PUBLIC ${SAWBLADE_JUCE_DEFS})
target_link_libraries(SawbladePlugin
  PRIVATE sawblade_core $<$<BOOL:${SAWBLADE_WITH_SEPARATOR}>:sawblade_separator>
          juce::juce_audio_processors juce::juce_gui_basics juce::juce_data_structures juce::juce_events
          juce::juce_audio_utils juce::juce_audio_formats SawbladeAssets
  PUBLIC  juce::juce_recommended_config_flags)
# NAM registers its architectures from static initializers: the archive must be linked whole into
# every final binary (the shared code is a static library, so the format targets and the tests
# add it themselves).
foreach(_fmt ${SAWBLADE_PLUGIN_FORMATS})
  target_link_libraries(SawbladePlugin_${_fmt} PRIVATE $<$<BOOL:${SAWBLADE_WITH_SEPARATOR}>:sawblade_separator> sawblade_core "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>" pffft_lib)
endforeach()

find_program(SAWBLADE_PYTHON NAMES python3 python)
# ONNX Runtime next to the plugin binary, found through $ORIGIN (Linux) / @loader_path (macOS):
#   Linux  VST3  Sawblade.vst3/Contents/x86_64-linux/   (beside Sawblade.so)      Standalone  beside the executable
#   macOS  VST3 / AU / app  <bundle>/Contents/Frameworks/  (rpath @loader_path/../Frameworks)
# (The copy is made after the link; a macOS release build must (re)sign the bundle afterwards.)
if(SAWBLADE_WITH_SEPARATOR)
  foreach(_fmt ${SAWBLADE_PLUGIN_FORMATS})
    if(APPLE)
      set(_dest "$<TARGET_BUNDLE_CONTENT_DIR:SawbladePlugin_${_fmt}>/Frameworks")
      set_property(TARGET SawbladePlugin_${_fmt} APPEND PROPERTY BUILD_RPATH "@loader_path/../Frameworks")
      set_property(TARGET SawbladePlugin_${_fmt} APPEND PROPERTY INSTALL_RPATH "@loader_path/../Frameworks")
    else()
      set(_dest "$<TARGET_FILE_DIR:SawbladePlugin_${_fmt}>")
      set_property(TARGET SawbladePlugin_${_fmt} APPEND PROPERTY BUILD_RPATH "$ORIGIN")
      set_property(TARGET SawbladePlugin_${_fmt} APPEND PROPERTY INSTALL_RPATH "$ORIGIN")
    endif()
    add_custom_command(TARGET SawbladePlugin_${_fmt} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_dest}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SAWBLADE_ORT_LIBRARY}" "${_dest}/${SAWBLADE_ORT_SONAME}"
      # The licence and third-party notices must accompany the redistributed library.
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SAWBLADE_ORT_ROOT}/LICENSE" "${_dest}/ONNXRUNTIME_LICENSE"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SAWBLADE_ORT_ROOT}/ThirdPartyNotices.txt" "${_dest}/ONNXRUNTIME_ThirdPartyNotices.txt"
      VERBATIM)
  endforeach()
endif()

if(SAWBLADE_BUILD_TESTS)
  include(Catch)
  add_executable(sawblade_plugin_tests
    ${PROJECT_SOURCE_DIR}/tests/alloc_guard.cpp
    tests/lock_guard.cpp
    tests/test_engine.cpp
    tests/test_processor.cpp
    tests/test_ladder.cpp
    tests/test_body_fill.cpp
    tests/test_playalong.cpp
    tests/test_browser_json.cpp
    tests/test_browser_preview.cpp
    tests/test_rig_model.cpp
    tests/test_rig_controller.cpp
    tests/test_pedal_face.cpp
    tests/test_mic.cpp
    tests/test_presets.cpp
    tests/test_record_match.cpp
    tests/test_settings.cpp
    ${SAWBLADE_PLUGIN_SOURCES})
  set_source_files_properties(
    ${PROJECT_SOURCE_DIR}/tests/alloc_guard.cpp tests/lock_guard.cpp tests/test_engine.cpp tests/test_processor.cpp tests/test_ladder.cpp tests/test_body_fill.cpp tests/test_playalong.cpp
    tests/test_browser_json.cpp tests/test_browser_preview.cpp tests/test_rig_model.cpp tests/test_rig_controller.cpp tests/test_pedal_face.cpp
    tests/test_mic.cpp tests/test_presets.cpp tests/test_record_match.cpp tests/test_settings.cpp
    PROPERTIES COMPILE_OPTIONS "$<TARGET_PROPERTY:sawblade_warnings,INTERFACE_COMPILE_OPTIONS>")
  add_dependencies(sawblade_plugin_tests SawbladeBuildInfo)
  target_include_directories(sawblade_plugin_tests PRIVATE src ${SAWBLADE_GENERATED_DIR})
  target_compile_definitions(sawblade_plugin_tests PRIVATE ${SAWBLADE_JUCE_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1
    SAWBLADE_TEST_TOOLS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/tools")
  target_link_libraries(sawblade_plugin_tests PRIVATE
    $<$<BOOL:${SAWBLADE_WITH_SEPARATOR}>:sawblade_separator> sawblade_core Catch2::Catch2WithMain SawbladeAssets
    juce::juce_audio_processors juce::juce_gui_basics juce::juce_data_structures juce::juce_events juce::juce_audio_formats
    juce::juce_recommended_config_flags
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>" pffft_lib)
  sawblade_ort_build_rpath(sawblade_plugin_tests)
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
    SAWBLADE_PRESETS_DIR="${PROJECT_SOURCE_DIR}/presets"
    SAWBLADE_PLUGIN_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
    SAWBLADE_RIFF_SCRIPT="${PROJECT_SOURCE_DIR}/design/render/make_preview_riff.py"
    SAWBLADE_PYTHON_EXE="${SAWBLADE_PYTHON}")
  catch_discover_tests(sawblade_plugin_tests TEST_PREFIX "plugin: ")
endif()

# Editor tests (skin, layout, bindings, screenshots): need a display, so they live in their own
# executable and run under xvfb-run when it is available. The editor sources and the JUCE modules are
# compiled into it (as for sawblade_plugin_tests). Screenshots: ${CMAKE_BINARY_DIR}/screenshots/.
if(SAWBLADE_BUILD_TESTS)
  add_executable(sawblade_editor_tests tests/test_editor.cpp tests/test_browser_ui.cpp tests/test_mic_page.cpp tests/test_preset_browser.cpp tests/test_live_controls.cpp tests/test_amp_head.cpp ${SAWBLADE_PLUGIN_SOURCES})
  set_source_files_properties(tests/test_editor.cpp tests/test_browser_ui.cpp tests/test_mic_page.cpp tests/test_preset_browser.cpp tests/test_live_controls.cpp tests/test_amp_head.cpp
    PROPERTIES COMPILE_OPTIONS "$<TARGET_PROPERTY:sawblade_warnings,INTERFACE_COMPILE_OPTIONS>")
  add_dependencies(sawblade_editor_tests SawbladeBuildInfo)
  target_include_directories(sawblade_editor_tests PRIVATE src ${SAWBLADE_GENERATED_DIR})
  # JUCE_MODAL_LOOPS_PERMITTED: the page tests pump the message loop (MessageManager::runDispatchLoopUntil).
  target_compile_definitions(sawblade_editor_tests PRIVATE ${SAWBLADE_JUCE_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1
    SAWBLADE_TEST_TOOLS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/tools"
    SAWBLADE_ICON_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/icon"
    SAWBLADE_PRESETS_DIR="${PROJECT_SOURCE_DIR}/presets"
    SAWBLADE_SCREENSHOT_DIR="${CMAKE_BINARY_DIR}/screenshots"
    SAWBLADE_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
    SAWBLADE_FAKE_T3K="${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_t3k.py")
  target_link_libraries(sawblade_editor_tests PRIVATE
    $<$<BOOL:${SAWBLADE_WITH_SEPARATOR}>:sawblade_separator> sawblade_core Catch2::Catch2WithMain SawbladeAssets
    juce::juce_audio_processors juce::juce_gui_basics juce::juce_data_structures juce::juce_events
    juce::juce_audio_formats juce::juce_recommended_config_flags
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>" pffft_lib)
  sawblade_ort_build_rpath(sawblade_editor_tests)
  find_program(SAWBLADE_XVFB_RUN xvfb-run)
  if(SAWBLADE_XVFB_RUN AND NOT APPLE)
    set_property(TARGET sawblade_editor_tests PROPERTY CROSSCOMPILING_EMULATOR ${SAWBLADE_XVFB_RUN} -a)
  endif()
  catch_discover_tests(sawblade_editor_tests TEST_PREFIX "editor: ")

  # plugin/assets/ budget (< 25 MB) and every file the exporter lists exists.
  if(SAWBLADE_PYTHON)
    add_test(NAME "editor: assets budget"
      COMMAND ${CMAKE_COMMAND} -DASSETS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/assets
              -DEXPORTER=${PROJECT_SOURCE_DIR}/design/render/export_ui_assets.py -DPYTHON=${SAWBLADE_PYTHON}
              -P ${CMAKE_CURRENT_SOURCE_DIR}/check_assets.cmake)
    # The committed icon PNGs are exactly what design/render/app_icon.py renders (skipped, exit 77, without Pillow).
    add_test(NAME "plugin: app icon is reproducible"
      COMMAND ${SAWBLADE_PYTHON} ${PROJECT_SOURCE_DIR}/design/render/app_icon.py --check)
    set_tests_properties("plugin: app icon is reproducible" PROPERTIES SKIP_RETURN_CODE 77)
  endif()
endif()

# Integration walk (v0.1): every panel of the Standalone-configured editor, one editor over one processor, screenshots to
# ${CMAKE_BINARY_DIR}/screenshots/v01/. Needs a display (xvfb-run). Tests: ctest prefix "integration: ".
if(SAWBLADE_BUILD_TESTS)
  add_executable(sawblade_integration_tests tests/test_integration.cpp ${SAWBLADE_PLUGIN_SOURCES})
  set_source_files_properties(tests/test_integration.cpp PROPERTIES COMPILE_OPTIONS "$<TARGET_PROPERTY:sawblade_warnings,INTERFACE_COMPILE_OPTIONS>")
  add_dependencies(sawblade_integration_tests SawbladeBuildInfo)
  target_include_directories(sawblade_integration_tests PRIVATE src ${SAWBLADE_GENERATED_DIR} ${PROJECT_SOURCE_DIR}/tests)
  target_compile_definitions(sawblade_integration_tests PRIVATE ${SAWBLADE_JUCE_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1
    SAWBLADE_TEST_TOOLS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/tools"
    SAWBLADE_PRESETS_DIR="${PROJECT_SOURCE_DIR}/presets"
    SAWBLADE_SCREENSHOT_DIR="${CMAKE_BINARY_DIR}/screenshots"
    SAWBLADE_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
    SAWBLADE_FAKE_T3K="${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_t3k.py")
  target_link_libraries(sawblade_integration_tests PRIVATE
    $<$<BOOL:${SAWBLADE_WITH_SEPARATOR}>:sawblade_separator> sawblade_core Catch2::Catch2WithMain SawbladeAssets
    juce::juce_audio_processors juce::juce_gui_basics juce::juce_data_structures juce::juce_events
    juce::juce_audio_formats juce::juce_recommended_config_flags
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>" pffft_lib)
  sawblade_ort_build_rpath(sawblade_integration_tests)
  find_program(SAWBLADE_XVFB_RUN xvfb-run)
  if(SAWBLADE_XVFB_RUN AND NOT APPLE)
    set_property(TARGET sawblade_integration_tests PROPERTY CROSSCOMPILING_EMULATOR ${SAWBLADE_XVFB_RUN} -a)
  endif()
  catch_discover_tests(sawblade_integration_tests TEST_PREFIX "integration: " TEST_SPEC "[integration]")

  # The built Standalone app opens its window under xvfb, stays alive for 6 s, is captured with ImageMagick and ends on SIGTERM.
  # Needs the Standalone format target (Linux / macOS dev machines without a display server skip it).
  find_program(SAWBLADE_IMPORT_EXE import)
  if(SAWBLADE_XVFB_RUN AND SAWBLADE_IMPORT_EXE AND NOT APPLE AND "Standalone" IN_LIST SAWBLADE_PLUGIN_FORMATS)
    add_test(NAME "integration: standalone launches"
      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/standalone_launch.sh ${SAWBLADE_XVFB_RUN} ${SAWBLADE_IMPORT_EXE}
              "$<TARGET_FILE:SawbladePlugin_Standalone>" ${CMAKE_BINARY_DIR}/screenshots/v01/22_standalone.png)
    set_tests_properties("integration: standalone launches" PROPERTIES TIMEOUT 60 DEPENDS SawbladePlugin_Standalone
                          SKIP_RETURN_CODE 77)
  endif()
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
