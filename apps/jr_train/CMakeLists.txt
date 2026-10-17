# App: JR TRAIN RADIO(JR 在来線デジタル列車無線)。docs/apps/jr_train.md
#   dsp/       受信 DSP(jr-conventional-dsp の C++ 移植、MIT)。標準ライブラリだけに依存する
#   protocol/  フレーム同期・音声フレームの誤り訂正・データ電文・LH5・運行把握(receiver-core の移植、MIT)
#   pipeline   1 チャネルの受信処理(pipeline.rs)、call_monitor 通話の自動選択(monitor.rs)、storage 保存(recording.rs)
#   presets    チャネル割当(radio-presets.json の移植、MIT)
#   voice/     音声合成(jr-voice の C++ 移植、MIT。CELP + 学習済みモデル。spear_jr_train_voice)
#   receiver   純 C++ 受信機(前段 + 全チャネル + worker)。Qt/UHD 非依存 → headless / テスト可能 (§10.4)
#   tests/refgen  参照ベクトルの生成器(原本の Rust を呼ぶ開発用ツール。ビルドには含めない)
add_subdirectory(voice)

add_library(spear_jr_train_rx STATIC
  dsp/demodulator.cpp
  dsp/sequence.cpp
  protocol/frame_sync.cpp
  protocol/coding.cpp
  protocol/voice_frame.cpp
  protocol/data.cpp
  protocol/text.cpp
  protocol/lha.cpp
  protocol/jis0208_table.cpp
  presets.cpp
  presets_json.cpp
  pipeline.cpp
  receiver.cpp
  call_monitor.cpp
  storage.cpp
)
target_include_directories(spear_jr_train_rx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(spear_jr_train_rx PUBLIC spear_jr_train_voice spear_dsp spear_core Threads::Threads)
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
