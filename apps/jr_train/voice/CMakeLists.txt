# 音声合成(jr-voice v1.3.0 の C++ 移植、MIT)。docs/apps/jr_train.md
#   celp/      整数 CELP(mcelp-codec 由来、celp/LICENSE)
#   models/    学習済み係数(原本の src/models/*.bin そのまま)。embed_models.cmake で配列にしてバイナリに埋め込む
# 標準ライブラリだけに依存する(Qt / UHD / spear_core なし)。原本(Rust)と同じ PCM にするため FMA の縮約をしない。
set(_models
  ${CMAKE_CURRENT_SOURCE_DIR}/models/celp_parameters.bin
  ${CMAKE_CURRENT_SOURCE_DIR}/models/correction.bin
  ${CMAKE_CURRENT_SOURCE_DIR}/models/gain.bin
  ${CMAKE_CURRENT_SOURCE_DIR}/models/planner.bin
  ${CMAKE_CURRENT_SOURCE_DIR}/models/predictor.bin
  ${CMAKE_CURRENT_SOURCE_DIR}/models/source.bin
  ${CMAKE_CURRENT_SOURCE_DIR}/models/spectrum.bin
  ${CMAKE_CURRENT_SOURCE_DIR}/models/temporal.bin
)
set(_models_cpp ${CMAKE_CURRENT_BINARY_DIR}/voice_models_data.cpp)
add_custom_command(
  OUTPUT ${_models_cpp}
  COMMAND ${CMAKE_COMMAND} -DOUTPUT=${_models_cpp} "-DINPUTS=${_models}" -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_models.cmake
  DEPENDS ${_models} ${CMAKE_CURRENT_SOURCE_DIR}/embed_models.cmake
  COMMENT "Embedding JR voice models"
  VERBATIM
)

add_library(spear_jr_train_voice STATIC
  voice.cpp
  stream.cpp
  synthesis.cpp
  plan.cpp
  excitation.cpp
  vocoder.cpp
  correction.cpp
  predictor.cpp
  numeric.cpp
  models.cpp
  celp/celp.cpp
  ${_models_cpp}
)
target_include_directories(spear_jr_train_voice PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/.. PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(spear_jr_train_voice PRIVATE -ffp-contract=off)
# 生成した配列(4 MB)は警告の対象外
set_source_files_properties(${_models_cpp} PROPERTIES COMPILE_OPTIONS "-w")
find_package(Threads REQUIRED)
target_link_libraries(spear_jr_train_voice PUBLIC Threads::Threads)

if(SPEAR_BUILD_TESTS)
  add_subdirectory(tests)
endif()
