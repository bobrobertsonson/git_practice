# Third-party dependencies, all pinned to exact commits (see docs/THIRD_PARTY.md).
# Headers are exposed as SYSTEM so -Werror only ever applies to our own code.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# SOURCE_SUBDIR points at a directory with no CMakeLists.txt so FetchContent_MakeAvailable
# only downloads; we build these ourselves (header-only or hand-written targets below).
FetchContent_Declare(eigen
  GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
  GIT_TAG        3147391d946bb4b6c68edd901f2add6ac1f31f8c # 3.4.0
  GIT_SHALLOW    OFF
  SOURCE_SUBDIR  _no_cmake)
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03 # v3.11.3
  SOURCE_SUBDIR  _no_cmake)
FetchContent_Declare(nam_core
  GIT_REPOSITORY https://github.com/sdatkinson/NeuralAmpModelerCore.git
  GIT_TAG        0b3d3c97b0859a3a8c92a8628c4dd89a25eb5842
  SOURCE_SUBDIR  _no_cmake)
FetchContent_Declare(pffft
  GIT_REPOSITORY https://github.com/marton78/pffft.git
  GIT_TAG        aa16fd3db58de4ba5dae8b0438440bb9da46b6fa
  SOURCE_SUBDIR  _no_cmake)
FetchContent_Declare(dr_libs
  GIT_REPOSITORY https://github.com/mackron/dr_libs.git
  GIT_TAG        dfe8377631000664666519fdb83da193fd8037f4
  SOURCE_SUBDIR  _no_cmake)

FetchContent_MakeAvailable(eigen nlohmann_json nam_core pffft dr_libs)

# --- nlohmann/json (header-only) ---------------------------------------------------------
# NAM core does `#include "json.hpp"` (flat), so single_include/nlohmann is on the path too.
add_library(sawblade_json INTERFACE)
target_include_directories(sawblade_json SYSTEM INTERFACE
  "${nlohmann_json_SOURCE_DIR}/single_include"
  "${nlohmann_json_SOURCE_DIR}/single_include/nlohmann")
add_library(nlohmann_json::nlohmann_json ALIAS sawblade_json)

# --- Eigen 3.4.0 from gitlab (header-only, MPL2). NAM core's Eigen submodule is NOT used. ---
add_library(sawblade_eigen INTERFACE)
target_include_directories(sawblade_eigen SYSTEM INTERFACE "${eigen_SOURCE_DIR}")

# --- NeuralAmpModelerCore: NAM/*.cpp built into a static lib, float samples. No tools. -----
file(GLOB _nam_sources CONFIGURE_DEPENDS
  "${nam_core_SOURCE_DIR}/NAM/*.cpp" "${nam_core_SOURCE_DIR}/NAM/*/*.cpp")
add_library(nam_core STATIC ${_nam_sources})
target_include_directories(nam_core SYSTEM PUBLIC "${nam_core_SOURCE_DIR}")
target_compile_definitions(nam_core PUBLIC NAM_SAMPLE_FLOAT NAM_ENABLE_A2_FAST)
target_compile_features(nam_core PUBLIC cxx_std_20)
target_link_libraries(nam_core PUBLIC sawblade_eigen sawblade_json)
if(NOT MSVC)
  target_compile_options(nam_core PRIVATE -w) # third-party code: silence, never -Werror
endif()
set_target_properties(nam_core PROPERTIES CXX_VISIBILITY_PRESET hidden)

# --- PFFFT (float only, C) ------------------------------------------------------------------
add_library(pffft_lib STATIC
  "${pffft_SOURCE_DIR}/src/pffft.c"
  "${pffft_SOURCE_DIR}/src/pffft_common.c")
target_include_directories(pffft_lib SYSTEM PUBLIC "${pffft_SOURCE_DIR}/include/pffft")
if(NOT MSVC)
  target_compile_options(pffft_lib PRIVATE -w)
  target_link_libraries(pffft_lib PUBLIC m)
endif()

# --- dr_wav: header-only; the single implementation TU is core/src/dr_wav_impl.cpp -------
add_library(dr_wav_headers INTERFACE)
target_include_directories(dr_wav_headers SYSTEM INTERFACE "${dr_libs_SOURCE_DIR}")

# --- ONNX Runtime 1.30.0 (SAWBLADE_WITH_SEPARATOR): official release tarball, pinned by URL + sha256 --------
# MIT. Prebuilt CPU binaries; the macOS tarball (arm64 only: Microsoft publishes no universal2 build of
# 1.30.0) also carries the CoreML EP, which we do not use yet. The shared library is found at run time
# through an rpath: the build tree's copy for the CLI tools and tests, and a copy inside the plugin
# bundle (sawblade_bundle_onnxruntime in plugin/CMakeLists.txt).
if(SAWBLADE_WITH_SEPARATOR)
  set(_ort_ver 1.30.0)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(_ort_url  https://github.com/microsoft/onnxruntime/releases/download/v${_ort_ver}/onnxruntime-linux-x64-${_ort_ver}.tgz)
    set(_ort_hash a5ed5a3cac51fbb2e90da632ae43d19212faaa20e76484e62bcb7c23ddb3b3fd)
    set(_ort_lib  lib/libonnxruntime.so.${_ort_ver})
    set(_ort_soname libonnxruntime.so.1)
  elseif(APPLE)
    # Only an arm64 ONNX Runtime 1.30.0 exists (no universal2, no x86_64): refuse anything else.
    if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64" OR (NOT CMAKE_OSX_ARCHITECTURES AND NOT CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64"))
      message(FATAL_ERROR "SAWBLADE_WITH_SEPARATOR: the pinned ONNX Runtime ${_ort_ver} for macOS is arm64 only, but this build targets "
                          "'${CMAKE_OSX_ARCHITECTURES}' on a ${CMAKE_HOST_SYSTEM_PROCESSOR} host. Configure with -DSAWBLADE_WITH_SEPARATOR=OFF "
                          "(or build arm64 only).")
    endif()
    set(_ort_url  https://github.com/microsoft/onnxruntime/releases/download/v${_ort_ver}/onnxruntime-osx-arm64-${_ort_ver}.tgz)
    set(_ort_hash 6ebb5062a934537c352937821f9fe9718e7de1a2db1122a93dd363ffd53a7012)
    set(_ort_lib  lib/libonnxruntime.${_ort_ver}.dylib)
    set(_ort_soname libonnxruntime.1.dylib)  # the dylib's install name is @rpath/libonnxruntime.1.dylib
  else()
    message(FATAL_ERROR "SAWBLADE_WITH_SEPARATOR: no pinned ONNX Runtime ${_ort_ver} build for ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR} "
                        "(supported: Linux x86_64, macOS arm64). Configure with -DSAWBLADE_WITH_SEPARATOR=OFF.")
  endif()
  FetchContent_Declare(onnxruntime_prebuilt URL ${_ort_url} URL_HASH SHA256=${_ort_hash})
  FetchContent_MakeAvailable(onnxruntime_prebuilt)
  add_library(sawblade_onnxruntime SHARED IMPORTED GLOBAL)
  set_target_properties(sawblade_onnxruntime PROPERTIES
    IMPORTED_LOCATION "${onnxruntime_prebuilt_SOURCE_DIR}/${_ort_lib}")
  if(APPLE)
    set_target_properties(sawblade_onnxruntime PROPERTIES IMPORTED_SONAME "@rpath/${_ort_soname}")
  else()
    set_target_properties(sawblade_onnxruntime PROPERTIES IMPORTED_SONAME "${_ort_soname}")
  endif()
  target_include_directories(sawblade_onnxruntime SYSTEM INTERFACE "${onnxruntime_prebuilt_SOURCE_DIR}/include")
  # Directory and file name to copy into a bundle (the file is renamed to the soname so the loader finds it).
  set(SAWBLADE_ORT_LIBRARY "${onnxruntime_prebuilt_SOURCE_DIR}/${_ort_lib}" CACHE INTERNAL "ONNX Runtime shared library")
  set(SAWBLADE_ORT_SONAME "${_ort_soname}" CACHE INTERNAL "ONNX Runtime soname / install-name leaf")
  set(SAWBLADE_ORT_ROOT "${onnxruntime_prebuilt_SOURCE_DIR}" CACHE INTERNAL "ONNX Runtime tarball root (LICENSE, ThirdPartyNotices.txt)")
  set(SAWBLADE_ORT_LIBDIR "${onnxruntime_prebuilt_SOURCE_DIR}/lib" CACHE INTERNAL "ONNX Runtime lib dir (build rpath)")
endif()

# Executables in the build tree find the ONNX Runtime shared library through their build rpath.
function(sawblade_ort_build_rpath target)
  if(SAWBLADE_WITH_SEPARATOR)
    set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "${SAWBLADE_ORT_LIBDIR}")
  endif()
endfunction()

# --- Catch2 (tests only) -------------------------------------------------------------------
if(SAWBLADE_BUILD_TESTS)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        9827c148c397289df17d3967692619a198032a24 # v3.7.1
    SYSTEM)
  FetchContent_MakeAvailable(Catch2)
  list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()

# --- JUCE 8 (plugin only; SAWBLADE_BUILD_PLUGIN) ---------------------------------------------
# LICENSING GATE: JUCE 8 is AGPLv3 or commercial. Sawblade is commercial, so a JUCE commercial
# licence is required before distributing any plugin binary. Development builds are fine.
# See docs/THIRD_PARTY.md.
if(SAWBLADE_BUILD_PLUGIN)
  FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        91ad83ae34a81e0833b1a2b0866f54846370ae53 # 8.0.15 (release tag)
    GIT_SHALLOW    ON
    SYSTEM)
  FetchContent_MakeAvailable(juce)
endif()

# --- pybind11 (Python bindings only; SAWBLADE_BUILD_PYTHON) ---------------------------------
# Pass -DPython_EXECUTABLE=<python> to pick the interpreter (match/.venv/bin/python).
if(SAWBLADE_BUILD_PYTHON)
  find_package(Python 3.11 REQUIRED COMPONENTS Interpreter Development.Module)
  set(PYBIND11_FINDPYTHON ON)
  FetchContent_Declare(pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG        d03662f0984f652b60e7ddce53d3868002275197 # v3.0.4
    SYSTEM)
  FetchContent_MakeAvailable(pybind11)
endif()
