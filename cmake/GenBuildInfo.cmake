# Writes BuildInfo.h for the About box and the Standalone title. Run as a script on EVERY build:
#   cmake -DSRC_DIR=<git work tree> -DTEMPLATE=<BuildInfo.h.in> -DOUT=<BuildInfo.h> -DVERSION=<x.y.z>
#         [-DGIT_EXECUTABLE=<git>] -P GenBuildInfo.cmake
# The header is rendered to a temp file and installed with copy_if_different, so an unchanged stamp
# leaves the real file (and its mtime) alone and nothing downstream recompiles.
foreach(_v SRC_DIR TEMPLATE OUT VERSION)
  if(NOT DEFINED ${_v})
    message(FATAL_ERROR "GenBuildInfo.cmake: -D${_v}= is required")
  endif()
endforeach()

set(SAWBLADE_GIT_SHA "unknown")
set(SAWBLADE_GIT_DIRTY "unknown")
if(NOT DEFINED GIT_EXECUTABLE)
  find_program(GIT_EXECUTABLE git)
endif()
if(GIT_EXECUTABLE)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD WORKING_DIRECTORY ${SRC_DIR}
                  OUTPUT_VARIABLE _sha OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _rc)
  if(_rc EQUAL 0 AND NOT _sha STREQUAL "")
    set(SAWBLADE_GIT_SHA "${_sha}")
    execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no WORKING_DIRECTORY ${SRC_DIR}
                    OUTPUT_VARIABLE _dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_dirty STREQUAL "")
      set(SAWBLADE_GIT_DIRTY "clean")
    else()
      set(SAWBLADE_GIT_DIRTY "dirty")
    endif()
  endif()
endif()
if(SAWBLADE_GIT_DIRTY STREQUAL "dirty")
  set(SAWBLADE_GIT_HASH "${SAWBLADE_GIT_SHA}-dirty")
else()
  set(SAWBLADE_GIT_HASH "${SAWBLADE_GIT_SHA}")
endif()
set(PROJECT_VERSION "${VERSION}")
string(TIMESTAMP SAWBLADE_BUILD_DATE "%Y-%m-%d" UTC)

get_filename_component(_dir "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_dir}")
set(_tmp "${OUT}.tmp")
configure_file("${TEMPLATE}" "${_tmp}" @ONLY)
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_tmp}" "${OUT}")
file(REMOVE "${_tmp}")
