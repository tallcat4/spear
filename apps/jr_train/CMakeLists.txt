# App: JR TRAIN RADIO(JR 在来線デジタル列車無線)。docs/apps/jr_train.md
#   dsp/       受信 DSP(jr-conventional-dsp の C++ 移植、MIT)。標準ライブラリだけに依存する
#   protocol/  フレーム同期(receiver-core frame.rs の移植、MIT)
#   presets    チャネル割当(radio-presets.json の移植、MIT)
#   receiver   純 C++ 受信機(前段 + 全チャネル + worker)。Qt/UHD 非依存 → headless / テスト可能 (§10.4)
#   tests/refgen  参照ベクトルの生成器(原本の Rust を呼ぶ開発用ツール。ビルドには含めない)
add_library(spear_jr_train_rx STATIC
  dsp/demodulator.cpp
  dsp/sequence.cpp
  protocol/frame_sync.cpp
  presets.cpp
  receiver.cpp
)
target_include_directories(spear_jr_train_rx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(spear_jr_train_rx PUBLIC spear_dsp spear_core Threads::Threads)
# 原本(Rust)と同じ浮動小数の結果にするため、移植した DSP では積和の縮約(FMA)をしない
set_source_files_properties(dsp/demodulator.cpp dsp/sequence.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)

if(SPEAR_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# ---- GUI App(登録は spear_add_app。受信機ライブラリは Qt 非依存のまま)----
if(COMMAND spear_add_app)
  spear_add_app(
    ID jr_train
    NAME "JR TRAIN RADIO"
    DESCRIPTION "JR conventional-line digital train radio: 352.6 MHz multi-channel pi/4-DQPSK, frame sync, constellation"
    DIRECTION RX
    CLASS spear::apps::JrTrainApp
    HEADER jr_train_app.hpp
    PAGE JrTrainPage.qml
    SOURCES jr_train_app.cpp
    LIBS spear_jr_train_rx
  )
endif()
