# ctest for cmake/GenBuildInfo.cmake (v0.1.3 task C). Run: cmake -DGEN=<script> -DTEMPLATE=<BuildInfo.h.in>
#   -DWORK=<scratch dir> [-DGIT=<git>] -P test_build_stamp.cmake
# Checks, in a throwaway git repo and WITHOUT any reconfigure between steps:
#   1. the header follows HEAD (new commit -> new sha), and a tracked edit flips the dirty flag;
#   2. running the stamp twice on the same HEAD leaves the file's content and mtime untouched;
#   3. without git the stamp falls back to "unknown".
foreach(_v GEN TEMPLATE WORK)
  if(NOT DEFINED ${_v})
    message(FATAL_ERROR "-D${_v}= is required")
  endif()
endforeach()
if(NOT DEFINED GIT)
  find_program(GIT git)
endif()
if(NOT GIT)
  message(FATAL_ERROR "git is required for this test")
endif()

set(repo "${WORK}/repo")
set(out "${WORK}/gen/BuildInfo.h")
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${repo}")

function(git)
  execute_process(COMMAND ${GIT} -c user.name=t -c user.email=t@example.com -c commit.gpgsign=false ${ARGN}
                  WORKING_DIRECTORY "${repo}" RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "git ${ARGN} failed: ${err}")
  endif()
endfunction()

function(stamp)  # stamp(<git exe or empty for "use default">)
  set(extra "")
  if(ARGC GREATER 0)
    set(extra "-DGIT_EXECUTABLE=${ARGV0}")
  endif()
  execute_process(COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${repo} -DTEMPLATE=${TEMPLATE} -DOUT=${out}
                          -DVERSION=9.8.7 -DGIT_EXECUTABLE=${GIT} ${extra} -P ${GEN}
                  RESULT_VARIABLE rc ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "GenBuildInfo failed: ${err}")
  endif()
endfunction()

function(field name outvar)
  file(STRINGS "${out}" line REGEX "k${name} =")
  string(REGEX REPLACE ".*= \"([^\"]*)\".*" "\\1" v "${line}")
  set(${outvar} "${v}" PARENT_SCOPE)
endfunction()

file(WRITE "${repo}/a.txt" "one\n")
git(init -q)
git(add a.txt)
git(commit -q -m one)

stamp()
field(Version v0)
field(GitSha sha1)
field(GitDirty dirty1)
file(READ "${out}" text1)
if(NOT v0 STREQUAL "9.8.7" OR sha1 STREQUAL "" OR sha1 STREQUAL "unknown" OR NOT dirty1 STREQUAL "clean")
  message(FATAL_ERROR "bad first stamp: version=${v0} sha=${sha1} dirty=${dirty1}\n${text1}")
endif()

# Same HEAD, run again: content and mtime untouched (sleep so a rewrite would move the 1 s timestamp).
file(TIMESTAMP "${out}" ts1 "%Y-%m-%dT%H:%M:%S")
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1.2)
stamp()
file(TIMESTAMP "${out}" ts2 "%Y-%m-%dT%H:%M:%S")
file(READ "${out}" text2)
if(NOT text1 STREQUAL text2 OR NOT ts1 STREQUAL ts2)
  message(FATAL_ERROR "unchanged HEAD rewrote the header (${ts1} -> ${ts2})")
endif()
if(EXISTS "${out}.tmp")
  message(FATAL_ERROR "temp file left behind")
endif()

# HEAD moves, no reconfigure: the header changes.
file(WRITE "${repo}/a.txt" "two\n")
git(commit -q -a -m two)
stamp()
field(GitSha sha2)
if(sha2 STREQUAL sha1 OR sha2 STREQUAL "unknown")
  message(FATAL_ERROR "header did not follow HEAD: ${sha1} -> ${sha2}")
endif()

# Tracked edit: dirty flag, and kGitHash gets the suffix.
file(WRITE "${repo}/a.txt" "three\n")
stamp()
field(GitDirty dirty3)
field(GitHash hash3)
if(NOT dirty3 STREQUAL "dirty" OR NOT hash3 STREQUAL "${sha2}-dirty")
  message(FATAL_ERROR "dirty not detected: dirty=${dirty3} hash=${hash3}")
endif()

# No git: fallback, still a valid header.
stamp("")
field(GitSha sha4)
field(GitDirty dirty4)
field(GitHash hash4)
if(NOT sha4 STREQUAL "unknown" OR NOT dirty4 STREQUAL "unknown" OR NOT hash4 STREQUAL "unknown")
  message(FATAL_ERROR "no-git fallback wrong: ${sha4} ${dirty4} ${hash4}")
endif()

message(STATUS "build stamp OK: ${sha1} -> ${sha2} -> dirty -> unknown")
