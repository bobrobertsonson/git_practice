include(Catch)

add_executable(sawblade_tests
  alloc_guard.cpp
  test_alloc_guard.cpp
  test_deps.cpp
  test_eq.cpp
  test_gate.cpp
  test_delay.cpp
  test_wav.cpp)
target_link_libraries(sawblade_tests PRIVATE
  sawblade_core sawblade_warnings Catch2::Catch2WithMain
  nam_core pffft_lib sawblade_json)
target_include_directories(sawblade_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(sawblade_tests)
