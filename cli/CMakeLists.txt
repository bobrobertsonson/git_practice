add_executable(tonerender main.cpp)
if(SAWBLADE_WITH_SEPARATOR)
  # tonerender --separate and the small sawblade-stems tool (phase 5.1b). The separator library is listed
  # first: it depends on core, which must come after it on the link line (and the whole-archive NAM
  # group must not be repeated).
  target_sources(tonerender PRIVATE separate_cli.cpp)
  target_link_libraries(tonerender PRIVATE sawblade_separator)
  sawblade_ort_build_rpath(tonerender)
endif()
target_link_libraries(tonerender PRIVATE
  sawblade_core sawblade_warnings pffft_lib
  # Same reason as in core/CMakeLists.txt: NAM architectures register from static initializers.
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>")

if(SAWBLADE_WITH_SEPARATOR)
  add_executable(sawblade-stems stems_main.cpp separate_cli.cpp)
  target_link_libraries(sawblade-stems PRIVATE sawblade_separator sawblade_warnings)
  sawblade_ort_build_rpath(sawblade-stems)
endif()
