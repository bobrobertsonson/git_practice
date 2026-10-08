cmake_minimum_required(VERSION 3.24)
project(Sawblade VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SAWBLADE_BUILD_TESTS "Build the Catch2 test suite" ON)
option(SAWBLADE_BUILD_PLUGIN "Build the JUCE plugin (VST3, AU on macOS, Standalone) and its headless tests. JUCE 8 is AGPLv3 or commercial: a commercial licence is required before distribution" OFF)
# Python bindings (module `sawblade_core`, see bindings/python and match/README.md). Off by default
# so the normal build neither needs Python nor fetches pybind11.
option(SAWBLADE_BUILD_PYTHON "Build the sawblade_core Python extension module" OFF)

# Phase 5.0 spike: C++ htdemucs (demucs.cpp) stem separation. Off by default so the normal build
# fetches and builds nothing extra. See spikes/separator/README.md.
option(SAWBLADE_BUILD_SEPARATOR_SPIKE "Build the demucs.cpp stem-separation spike (spikes/separator)" OFF)

# Phase 5.1b: on-device stem separation (ONNX Runtime CPU EP, htdemucs). Defaults ON when the plugin is
# built (it needs it for "load any mp3") and OFF otherwise. OFF: nothing is downloaded, core / CLI / plugin
# build without ONNX Runtime and a song file cannot be separated ("separation not available in this build").
# ON: the pinned official ONNX Runtime release is fetched at configure time (cmake/Dependencies.cmake).
set(_sawblade_sep_default OFF)
if(SAWBLADE_BUILD_PLUGIN)
  set(_sawblade_sep_default ON)
endif()
option(SAWBLADE_WITH_SEPARATOR "Build the ONNX Runtime stem separator (sawblade_separator, sawblade-stems, tonerender --separate)" ${_sawblade_sep_default})

# Warnings-as-errors apply to Sawblade targets only: link this interface target into them.
# Third-party targets never link it and expose their headers as SYSTEM.
add_library(sawblade_warnings INTERFACE)
if(MSVC)
  target_compile_options(sawblade_warnings INTERFACE /W4 /WX)
else()
  target_compile_options(sawblade_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)
endif()

# Floating-point contraction off for every target (ours, JUCE modules, NAM core, pffft, bindings): Apple clang on
# arm64 fuses a*b+c into FMA by default, gcc on x86-64 does not, so without this the same preset renders a few ulps
# differently per platform (goldens, exact silence, parameter round trips). Determinism per platform is unchanged.
# Explicit FMA intrinsics (Eigen's NEON pmadd, vfmaq_f32) are not affected by the flag.
option(SAWBLADE_FP_CONTRACT_OFF "Compile with -ffp-contract=off (bit-identical float results across compilers / CPUs)" ON)
if(NOT MSVC AND SAWBLADE_FP_CONTRACT_OFF)
  add_compile_options(-ffp-contract=off)
endif()

include(cmake/Dependencies.cmake)

add_subdirectory(core)
add_subdirectory(cli)

# Stock-NAM-core load check (links only nam_core). Built with the tests, or alone for the python-export CI job.
option(SAWBLADE_BUILD_NAM_LOAD_CHECK "Build tests/tools/nam_load_check without the rest of the tests" OFF)
if(SAWBLADE_BUILD_TESTS OR SAWBLADE_BUILD_NAM_LOAD_CHECK)
  add_executable(nam_load_check tests/tools/nam_load_check.cpp)
  # WHOLE_ARCHIVE: the core registers its architectures from static initialisers in the library.
  target_link_libraries(nam_load_check PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>" sawblade_warnings)
endif()

if(SAWBLADE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(SAWBLADE_BUILD_PLUGIN)
  add_subdirectory(plugin)
endif()

# After enable_testing() so the python_bindings ctest entry registers.
if(SAWBLADE_BUILD_PYTHON)
  add_subdirectory(bindings/python)
endif()

if(SAWBLADE_BUILD_SEPARATOR_SPIKE)
  add_subdirectory(spikes/separator)
endif()
