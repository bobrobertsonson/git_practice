# dr_wav / dr_flac implementations live in their own library so -Werror never sees third-party code.
add_library(sawblade_dr_wav STATIC src/dr_wav_impl.cpp src/dr_flac_impl.cpp src/dr_mp3_impl.cpp)
target_link_libraries(sawblade_dr_wav PUBLIC dr_wav_headers)
if(NOT MSVC)
  target_compile_options(sawblade_dr_wav PRIVATE -w)
endif()

add_library(sawblade_core STATIC
  src/eq.cpp
  src/amp_controls.cpp
  src/gain_ladder.cpp
  src/oversampler.cpp
  src/adaa_clipper.cpp
  src/pedal_params.cpp
  src/pedal_hm.cpp
  src/pedal_muff.cpp
  src/pedal_ts.cpp
  src/pedal_saw_params.cpp
  src/pedal_hmx.cpp
  src/pedal_eye.cpp
  src/gate.cpp
  src/convolver.cpp
  src/resample.cpp
  src/rt_resample.cpp
  src/ir.cpp
  src/ir_mix.cpp
  src/nam_block.cpp
  src/delay.cpp
  src/wav_io.cpp
  src/loudness.cpp
  src/reference_di.cpp
  src/auto_trim.cpp
  src/stem_set.cpp
  src/stem_player.cpp
  src/sha256.cpp
  src/model_store.cpp
  src/stem_cache.cpp
  src/bus_comp.cpp
  src/preset_reader.cpp
  src/preset.cpp
  src/block_registry.cpp
  src/capture_cache.cpp
  src/chain.cpp
  src/render.cpp)
add_library(sawblade::core ALIAS sawblade_core)
# The reference DI must be bit-identical on every machine: no fused multiply-add contraction in its generator.
if(NOT MSVC)
  set_source_files_properties(src/reference_di.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif()
target_include_directories(sawblade_core PUBLIC include)
target_compile_features(sawblade_core PUBLIC cxx_std_20)
target_link_libraries(sawblade_core
  PUBLIC  sawblade_dr_wav sawblade_json
  PRIVATE sawblade_warnings pffft_lib
          # NAM registers its architectures (WaveNet, LSTM, Linear, ...) from static initializers in
          # otherwise-unreferenced objects, so the archive must be linked whole or they are dropped.
          "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>")

# Phase 5.1b: on-device stem separation (ONNX Runtime). A separate library so core itself (and the Python
# bindings) never link ONNX Runtime. Consumers: tonerender, sawblade-stems, the plugin, the separator tests.
if(SAWBLADE_WITH_SEPARATOR)
  add_library(sawblade_separator STATIC src/separator.cpp src/separate_song.cpp)
  add_library(sawblade::separator ALIAS sawblade_separator)
  target_link_libraries(sawblade_separator
    PUBLIC  sawblade_core sawblade_onnxruntime
    PRIVATE sawblade_warnings pffft_lib)
  target_compile_definitions(sawblade_separator PUBLIC SAWBLADE_WITH_SEPARATOR=1)
endif()
