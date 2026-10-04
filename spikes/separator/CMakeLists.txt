# Phase 5.0 spike (docs/specs/phase5_0_separator_spike.md). Only added with
# -DSAWBLADE_BUILD_SEPARATOR_SPIKE=ON. Nothing here is linked into core/, cli/ or the plugin.
option(SAWBLADE_SEPARATOR_NATIVE "Compile demucs.cpp with -march=native (not shippable)" ON)
# Explicit -march value (e.g. x86-64-v3 = AVX2/FMA, a realistic shippable baseline). Overrides NATIVE.
set(SAWBLADE_SEPARATOR_ARCH "" CACHE STRING "-march value for demucs.cpp and the driver (empty: use NATIVE)")
option(SAWBLADE_SEPARATOR_OPENMP "Build demucs.cpp with OpenMP (Eigen GEMM threading)" ON)

# demucs.cpp pinned to the spec's commit. Its git submodules (eigen, demucs, libnyquist,
# googletest) are NOT fetched: GIT_SUBMODULES "" skips them, we reuse the project's Eigen 3.4.0
# (cmake/Dependencies.cmake) and our own dr_wav driver I/O.
include(FetchContent)
FetchContent_Declare(demucs_cpp
  GIT_REPOSITORY https://github.com/sevagh/demucs.cpp.git
  GIT_TAG        f1206e9adeea103aef4a636b9e62297cf1f8e34e
  GIT_SUBMODULES ""
  SOURCE_SUBDIR  _no_cmake)
FetchContent_MakeAvailable(demucs_cpp)

file(GLOB _demucs_sources CONFIGURE_DEPENDS "${demucs_cpp_SOURCE_DIR}/src/*.cpp")
add_library(demucs_cpp_lib STATIC ${_demucs_sources})
# Headers are SYSTEM so -Werror never applies; the sources are compiled with -w.
target_include_directories(demucs_cpp_lib SYSTEM PUBLIC "${demucs_cpp_SOURCE_DIR}/src")
target_link_libraries(demucs_cpp_lib PUBLIC sawblade_eigen)
target_compile_features(demucs_cpp_lib PUBLIC cxx_std_17)
# Same arithmetic flags as upstream's Release build, minus -Ofast's blanket unsafe-math
# (upstream passes -Ofast -fno-unsafe-math-optimizations -freciprocal-math -fno-signed-zeros).
target_compile_options(demucs_cpp_lib PRIVATE -w -O3 -DNDEBUG -freciprocal-math -fno-signed-zeros)
if(SAWBLADE_SEPARATOR_ARCH)
  target_compile_options(demucs_cpp_lib PUBLIC -march=${SAWBLADE_SEPARATOR_ARCH})
elseif(SAWBLADE_SEPARATOR_NATIVE)
  # PUBLIC on purpose: Eigen's alignment (EIGEN_MAX_ALIGN_BYTES) follows -march, and the driver
  # constructs demucs_model, so every TU that sees Eigen types must use the same flags
  # (a mismatch heap-corrupts in the first GEMM).
  target_compile_options(demucs_cpp_lib PUBLIC -march=native)
endif()
if(SAWBLADE_SEPARATOR_OPENMP)
  find_package(OpenMP REQUIRED COMPONENTS CXX)
  target_link_libraries(demucs_cpp_lib PUBLIC OpenMP::OpenMP_CXX)
endif()

add_executable(separator_spike separator_spike.cpp)
target_include_directories(separator_spike SYSTEM PRIVATE "${demucs_cpp_SOURCE_DIR}/cli-apps")
target_link_libraries(separator_spike PRIVATE sawblade_core sawblade_warnings demucs_cpp_lib dr_wav_headers)
