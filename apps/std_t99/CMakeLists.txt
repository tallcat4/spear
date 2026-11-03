# App: STD-T99 Annex 2(デジタル小電力コミュニティ無線、デジコミ)の選択チャネル受信。docs/apps/std_t99.md
#   原本: std-t99-receiver-sharp(SDR# プラグイン)。受信機は std-t99-receiver、音声は tokuder-neurodec の C++ 移植
#   channels.hpp  チャネル表(表 Annex 2-3-1)— 唯一の真値
#   protocol/     物理層の復号(RICH / SACCH / PICH / FACCH / TCH)とメッセージ
#   dsp/          同期語検出と同期の状態機械(STD-T98 の SyncCorrelator に DC 項を足したもの)、中心チャネルだけの PFB
#   receiver      1 チャネル受信機(100 kHz 入力)。RRC と Gardner は STD-T98 と共有(spear_std_t98_dsp)
#   front         App 側の前段(回転・4 Msps → 100 kHz)
#   session/call  メッセージの集約、送信の区切り、選択呼出の着信判定、受信履歴
#   voice/        TOKUDER: 誤り訂正、tokuder48_causal_v1 の推論、音声の時間軸、復号スレッド
#   storage       .burst / .llr / .jsonl / .wav の保存
#   position_map  発信位置の地図(QQuickPaintedItem。地図データと下地は widgets/map、ADS-B と共有)
#   monitor       上の全体(Qt / UHD 非依存 → ツール・テストと共有 §10.4)
#   models/       学習済みの重み(バイナリに埋め込む。models/README.md)
add_library(spear_std_t99_rx STATIC
  protocol/protocol.cpp
  protocol/messages.cpp
  dsp/frame_sync.cpp
  receiver.cpp
  session.cpp
  call.cpp
  front.cpp
  monitor.cpp
  storage.cpp
  voice/tokuder.cpp
  voice/model_data.cpp
  voice/timeline.cpp
  voice/worker.cpp
)
target_include_directories(spear_std_t99_rx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(spear_std_t99_rx PUBLIC spear_std_t98_dsp spear_dsp spear_core)
# 復号器の内積は並べ替えを許さないとベクトル化されない(自己完結した float の計算なので -ffast-math をこのファイルだけに当てる。
# AVX2 / FMA は実行時に選ぶので、全体の「-march=native を使わない」決定(docs/STATUS.md)はそのまま)
set_source_files_properties(voice/tokuder.cpp PROPERTIES COMPILE_OPTIONS "-O3;-ffast-math")
# 重みの埋め込み(voice/model_data.cpp の .incbin)。重みが変われば再コンパイルされるように依存を張る
set(SPEAR_STD_T99_MODELS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/models)
set_source_files_properties(voice/model_data.cpp PROPERTIES
  COMPILE_DEFINITIONS "SPEAR_STD_T99_MODELS_DIR=${SPEAR_STD_T99_MODELS_DIR}"
  OBJECT_DEPENDS "${SPEAR_STD_T99_MODELS_DIR}/tokuder48_causal_v1.bin")

if(SPEAR_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# ---- GUI App(登録は spear_add_app。受信機ライブラリは Qt 非依存のまま)----
if(COMMAND spear_add_app)
  spear_add_app(
    ID std_t99
    NAME "STD-T99 DIGICOM"
    DESCRIPTION "ARIB STD-T99 Annex 2 digital community radio: one channel, call info, position, TOKUDER audio"
    DIRECTION RX
    CLASS spear::apps::StdT99App
    HEADER std_t99_app.hpp
    PAGE StdT99Page.qml
    SOURCES std_t99_app.cpp position_map.cpp position_map.hpp
    LIBS spear_std_t99_rx spear_map
  )
endif()
