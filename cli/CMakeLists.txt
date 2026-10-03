add_executable(tonerender main.cpp)
target_link_libraries(tonerender PRIVATE
  sawblade_core sawblade_warnings pffft_lib
  # Same reason as in core/CMakeLists.txt: NAM architectures register from static initializers.
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>")
