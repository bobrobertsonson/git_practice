include(Catch)

add_executable(sawblade_tests
  alloc_guard.cpp
  test_alloc_guard.cpp
  test_deps.cpp
  test_eq.cpp
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
  test_capture_cache.cpp)
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
