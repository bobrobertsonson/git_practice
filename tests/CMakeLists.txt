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
  test_chain.cpp)
target_link_libraries(sawblade_tests PRIVATE
  sawblade_core sawblade_warnings Catch2::Catch2WithMain
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>" pffft_lib sawblade_json)
target_include_directories(sawblade_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(sawblade_tests)
target_compile_definitions(sawblade_tests PRIVATE SAWBLADE_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
