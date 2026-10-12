# Phase 5.0 spike (docs/specs/phase5_0_separator_spike.md) + phase 5.1a engine bake-off
# (docs/specs/phase5_1a_separator_engine.md). Only added with -DSAWBLADE_BUILD_SEPARATOR_SPIKE=ON.
# Nothing here is linked into core/, cli/ or the plugin.
option(SAWBLADE_SEPARATOR_NATIVE "Compile demucs.cpp with -march=native (not shippable)" ON)
# Explicit -march value (e.g. x86-64-v3 = AVX2/FMA, a realistic shippable baseline). Overrides NATIVE.
set(SAWBLADE_SEPARATOR_ARCH "" CACHE STRING "-march value for demucs.cpp and the driver (empty: use NATIVE)")
option(SAWBLADE_SEPARATOR_OPENMP "Build demucs.cpp with OpenMP (Eigen GEMM threading)" ON)
# 5.1a candidate (a): route Eigen GEMM to a system OpenBLAS (EIGEN_USE_BLAS). OFF = Eigen's own GEMM.
option(SAWBLADE_SEPARATOR_BLAS "Build demucs.cpp with EIGEN_USE_BLAS against OpenBLAS (apt libopenblas-dev)" OFF)
# Explicit OpenBLAS library. Default: the OpenMP variant (libopenblas0-openmp), so OpenBLAS workers are
# libgomp threads and get FTZ/DAZ from the same OpenMP team the library sets it on.
set(SAWBLADE_OPENBLAS_LIBRARY "" CACHE FILEPATH "OpenBLAS shared library (empty: auto-detect the OpenMP variant)")
# 5.1a candidate (b): ONNX Runtime CPU EP (official release tarball, pinned by URL + sha256).
option(SAWBLADE_BUILD_SEPARATOR_ONNX "Build the ONNX Runtime separator driver (separator_onnx)" OFF)
find_package(Git REQUIRED)

# demucs.cpp pinned to the spec's commit. Its git submodules (eigen, demucs, libnyquist,
# googletest) are NOT fetched: GIT_SUBMODULES "" skips them, we reuse the project's Eigen 3.4.0
# (cmake/Dependencies.cmake) and our own dr_wav driver I/O.
include(FetchContent)
FetchContent_Declare(demucs_cpp
  GIT_REPOSITORY https://github.com/sevagh/demucs.cpp.git
  GIT_TAG        f1206e9adeea103aef4a636b9e62297cf1f8e34e
  GIT_SUBMODULES ""
  SOURCE_SUBDIR  _no_cmake
  # 5.1a: shift fixed 0, Python context padding, cancel flag, no stdout/stderr, FTZ/DAZ.
  # A reviewable diff against the pinned commit (see the header of the patch).
  PATCH_COMMAND  ${CMAKE_COMMAND} -DGIT=${GIT_EXECUTABLE}
                 -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/patches/demucscpp-f1206e9a-sawblade.patch
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/patches/apply_patch.cmake)
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

if(SAWBLADE_SEPARATOR_BLAS)
  set(_sb_blas "${SAWBLADE_OPENBLAS_LIBRARY}")
  if(NOT _sb_blas)
    unset(_sb_blas_found CACHE)
    find_library(_sb_blas_found NAMES libopenblas.so.0
      PATHS /usr/lib/x86_64-linux-gnu/openblas-openmp /usr/lib/aarch64-linux-gnu/openblas-openmp
      NO_DEFAULT_PATH)
    if(NOT _sb_blas_found)
      find_library(_sb_blas_found NAMES openblas REQUIRED)
    endif()
    set(_sb_blas "${_sb_blas_found}")
  endif()
  message(STATUS "separator spike: Eigen GEMM -> OpenBLAS ${_sb_blas}")
  # PUBLIC: EIGEN_USE_BLAS changes Eigen's product dispatch, every TU that sees Eigen must agree.
  target_compile_definitions(demucs_cpp_lib PUBLIC EIGEN_USE_BLAS)
  target_link_libraries(demucs_cpp_lib PUBLIC "${_sb_blas}")
endif()

add_executable(separator_spike separator_spike.cpp)
target_include_directories(separator_spike PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(separator_spike PRIVATE sawblade_core sawblade_warnings demucs_cpp_lib dr_wav_headers)
if(SAWBLADE_SEPARATOR_BLAS)
  target_compile_definitions(separator_spike PRIVATE SAWBLADE_SEPARATOR_BLAS=1)
endif()

if(SAWBLADE_BUILD_SEPARATOR_ONNX)
  # ONNX Runtime 1.30.0 (latest stable on PyPI/GitHub on 2026-10-04; the official osx-arm64 tarball of
  # the same version ships the CoreML EP). Official linux-x64 CPU tarball, pinned by sha256.
  FetchContent_Declare(onnxruntime_prebuilt
    URL      https://github.com/microsoft/onnxruntime/releases/download/v1.30.0/onnxruntime-linux-x64-1.30.0.tgz
    URL_HASH SHA256=a5ed5a3cac51fbb2e90da632ae43d19212faaa20e76484e62bcb7c23ddb3b3fd)
  FetchContent_MakeAvailable(onnxruntime_prebuilt)
  add_library(sawblade_onnxruntime SHARED IMPORTED)
  set_target_properties(sawblade_onnxruntime PROPERTIES
    IMPORTED_LOCATION "${onnxruntime_prebuilt_SOURCE_DIR}/lib/libonnxruntime.so.1.30.0"
    IMPORTED_SONAME   "libonnxruntime.so.1")
  # SYSTEM: the vendor headers are not ours.
  target_include_directories(sawblade_onnxruntime SYSTEM INTERFACE "${onnxruntime_prebuilt_SOURCE_DIR}/include")
  add_executable(separator_onnx separator_onnx.cpp)
  target_include_directories(separator_onnx PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(separator_onnx PRIVATE sawblade_core sawblade_warnings sawblade_onnxruntime
                        pffft_lib dr_wav_headers)
  set_target_properties(separator_onnx PROPERTIES BUILD_RPATH "${onnxruntime_prebuilt_SOURCE_DIR}/lib")
  # No -march for this driver: ORT dispatches its own kernels at run time; our STFT is scalar + PFFFT.
endif()
