# Python extension module `sawblade_core` (pybind11). Built only with -DSAWBLADE_BUILD_PYTHON=ON.
# The CMake target is `sawblade_py` because `sawblade_core` is already the static library; the
# file produced is <build>/python/sawblade_core.<abi-tag>.so|pyd.
pybind11_add_module(sawblade_py MODULE sawblade_core_module.cpp)
set_target_properties(sawblade_py PROPERTIES
  OUTPUT_NAME sawblade_core
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/python"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/python"
  CXX_VISIBILITY_PRESET hidden)
target_link_libraries(sawblade_py PRIVATE
  sawblade_core sawblade_warnings pffft_lib
  # NAM architectures register from static initializers (see core/CMakeLists.txt).
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>")

# ctest entry (only exists when the Python build is on): the pytest suite of the bindings, run
# with the same interpreter the module was built for.
if(SAWBLADE_BUILD_TESTS)
  add_test(NAME python_bindings
    COMMAND "${Python_EXECUTABLE}" -m pytest -q "${PROJECT_SOURCE_DIR}/match/tests/test_core_bindings.py"
            "${PROJECT_SOURCE_DIR}/match/tests/test_stem_player_bindings.py"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/match")
  set_tests_properties(python_bindings PROPERTIES
    ENVIRONMENT "SAWBLADE_CORE_DIR=${CMAKE_BINARY_DIR}/python;SAWBLADE_TONERENDER=$<TARGET_FILE:tonerender>")
endif()
