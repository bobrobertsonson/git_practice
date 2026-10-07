include(Catch)

add_executable(sawblade_tests
  alloc_guard.cpp
  test_alloc_guard.cpp
  test_deps.cpp
  test_eq.cpp
  test_amp_controls.cpp
  test_gain_ladder.cpp
  test_gate.cpp
  test_delay.cpp
  test_wav.cpp
  test_nam_block.cpp
  test_ir.cpp
  test_convolver.cpp
  test_swap_slot.cpp
  test_preset.cpp
  test_bus_comp.cpp
  test_chain.cpp
  test_render.cpp
  test_resample.cpp
  test_rt_resample.cpp
  test_chain_live.cpp
  test_level_match.cpp
  test_auto_trim.cpp
  test_capture_cache.cpp
  test_stem_set.cpp
  test_stem_player.cpp
  test_stem_cli.cpp
  test_loudness.cpp
  test_pedals.cpp
  test_pedals_7b.cpp
  test_ir_mix.cpp
  test_category_cache.cpp
  test_separation_support.cpp
  test_pedals_saw.cpp
  test_pedals_v3.cpp
  test_a2_playback.cpp)
target_link_libraries(sawblade_tests PRIVATE
  sawblade_core sawblade_warnings Catch2::Catch2WithMain
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>" pffft_lib sawblade_json)
target_include_directories(sawblade_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(sawblade_tests)
target_compile_definitions(sawblade_tests PRIVATE SAWBLADE_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_compile_definitions(sawblade_tests PRIVATE
  SAWBLADE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
  SAWBLADE_PRESETS_DIR="${PROJECT_SOURCE_DIR}/presets"
  SAWBLADE_TONERENDER_EXE="$<TARGET_FILE:tonerender>")
# The CLI tests run the real binary.
add_dependencies(sawblade_tests tonerender)

# Deterministic fixture generator (run via scripts/regen_fixtures.sh; not part of ctest).
add_executable(make_fixtures tools/make_fixtures.cpp)
target_link_libraries(make_fixtures PRIVATE sawblade_core sawblade_warnings)

# Dry-run check of scripts/mac_update.sh (bash only; no Mac needed).
find_program(SAWBLADE_BASH bash)
if(SAWBLADE_BASH)
  add_test(NAME mac_update_dry_run
    COMMAND ${SAWBLADE_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/test_mac_update.sh
            ${PROJECT_SOURCE_DIR}/scripts/mac_update.sh)
endif()

# Build stamp (v0.1.3 task C): the BuildInfo.h generator follows HEAD without a reconfigure, is a no-op on an
# unchanged HEAD, and falls back to "unknown" without git. Pure cmake -P; needs git, no plugin build.
find_package(Git QUIET)
if(GIT_FOUND)
  add_test(NAME build_stamp
    COMMAND ${CMAKE_COMMAND} -DGEN=${PROJECT_SOURCE_DIR}/cmake/GenBuildInfo.cmake
            -DTEMPLATE=${PROJECT_SOURCE_DIR}/plugin/src/about/BuildInfo.h.in
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/build_stamp_work -DGIT=${GIT_EXECUTABLE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test_build_stamp.cmake)
endif()

# Developer tool (not installed): small-signal frequency response of the modeled pedals as CSV.
add_executable(pedal_fr tools/pedal_fr.cpp)
target_link_libraries(pedal_fr PRIVATE sawblade_core sawblade_warnings)
target_include_directories(pedal_fr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Phase 5.1b separator tests: a synthetic ONNX model built at test time (tests/onnx_synth.h), no weights,
# no Python. Separate executable so the ONNX Runtime library stays out of the core tests.
if(SAWBLADE_WITH_SEPARATOR)
  add_executable(sawblade_separator_tests test_separator.cpp)
  target_link_libraries(sawblade_separator_tests PRIVATE
    sawblade_separator sawblade_warnings Catch2::Catch2WithMain sawblade_json)
  target_include_directories(sawblade_separator_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(sawblade_separator_tests PRIVATE
    SAWBLADE_SEPARATE_EXE="$<TARGET_FILE:sawblade-stems>"
    SAWBLADE_TONERENDER_EXE="$<TARGET_FILE:tonerender>"
    SAWBLADE_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    SAWBLADE_PRESETS_DIR="${PROJECT_SOURCE_DIR}/presets")
  add_dependencies(sawblade_separator_tests sawblade-stems tonerender)
  sawblade_ort_build_rpath(sawblade_separator_tests)
  catch_discover_tests(sawblade_separator_tests TEST_PREFIX "separator: ")
endif()

# v0.6 Task B: CPU benchmark of A1 / A2 playback and the two-A2-Full rig. Prints a table; fails only if the rig is not
# real-time (RTF < 0.5, optimised builds only). Plain executable (no Catch2): the table is the output.
add_executable(a2_bench a2_bench.cpp)
target_link_libraries(a2_bench PRIVATE sawblade_core sawblade_warnings
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>" pffft_lib sawblade_json)
target_compile_definitions(a2_bench PRIVATE SAWBLADE_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_test(NAME a2_bench COMMAND a2_bench)
set_tests_properties(a2_bench PROPERTIES LABELS "bench" TIMEOUT 300 RUN_SERIAL TRUE)
