# dr_wav implementation lives in its own library so -Werror never sees third-party code.
add_library(sawblade_dr_wav STATIC src/dr_wav_impl.cpp)
target_link_libraries(sawblade_dr_wav PUBLIC dr_wav_headers)
if(NOT MSVC)
  target_compile_options(sawblade_dr_wav PRIVATE -w)
endif()

add_library(sawblade_core STATIC
  src/eq.cpp
  src/gate.cpp
  src/convolver.cpp
  src/ir.cpp
  src/nam_block.cpp
  src/delay.cpp
  src/wav_io.cpp
  src/sha256.cpp
  src/bus_comp.cpp
  src/preset_reader.cpp
  src/preset.cpp
  src/block_registry.cpp
  src/chain.cpp
  src/render.cpp)
add_library(sawblade::core ALIAS sawblade_core)
target_include_directories(sawblade_core PUBLIC include)
target_compile_features(sawblade_core PUBLIC cxx_std_20)
target_link_libraries(sawblade_core
  PUBLIC  sawblade_dr_wav sawblade_json
  PRIVATE sawblade_warnings pffft_lib
          # NAM registers its architectures (WaveNet, LSTM, Linear, ...) from static initializers in
          # otherwise-unreferenced objects, so the archive must be linked whole or they are dropped.
          "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>")
