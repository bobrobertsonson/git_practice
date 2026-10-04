# Idempotent patch step for the demucs.cpp FetchContent checkout:
#   cmake -DGIT=<git> -DPATCH=<file> -P apply_patch.cmake   (cwd = the checkout)
# FetchContent may re-run its patch step when the CMake files change; a plain `git apply` would then
# fail on the already-patched tree.
execute_process(COMMAND "${GIT}" apply --check --reverse "${PATCH}"
                RESULT_VARIABLE _already OUTPUT_QUIET ERROR_QUIET)
if(_already EQUAL 0)
  message(STATUS "demucs.cpp patch already applied")
else()
  execute_process(COMMAND "${GIT}" apply --whitespace=nowarn "${PATCH}" RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "cannot apply ${PATCH}")
  endif()
endif()
