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

# --- Catch2 (tests only) -------------------------------------------------------------------
if(SAWBLADE_BUILD_TESTS)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        9827c148c397289df17d3967692619a198032a24 # v3.7.1
    SYSTEM)
  FetchContent_MakeAvailable(Catch2)
  list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
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
