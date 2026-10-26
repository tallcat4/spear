# App: STD-T98 モニタ (§12 M3)。
#   protocol/  純 C++ プロトコルデコーダ
#   dsp/       App 内 DSP(RRC 設計、Gardner symbol sync、同期語検出)
#   receiver   純 C++ 受信機(97ch)。Qt/UHD 非依存 → headless / テスト可能 (§10.4)
#   blip25c/   AMBE+2 3600x2450 の FEC と音声合成(blip25-vocoder の C 移植、MIT。手を加えない。blip25c/README.md)
#   ambe/      49 bit の並び(ambe_d ⇔ ThumbDV)。ambe.cpp が blip25c を包む
#   secret/    秘話: PN、鍵探索(ffnn = C++ 再実装 / hybrid = ONNX Runtime、要件 §2.5 道 1・2)、要求ポリシー、ワーカースレッド
#   models/    学習済みモデル(バイナリに埋め込む。models/README.md)
add_subdirectory(protocol)

# blip25c: Rust 版とビット一致の移植なので手を加えない。警告は全体の -Wconversion 等を当てずに消す(原本の Makefile も -Wno-unused-*)
enable_language(C)
add_library(spear_blip25c STATIC
  blip25c/src/b25_dsp_a.c
  blip25c/src/b25_dsp_b.c
  blip25c/src/b25_decode_c.c
  blip25c/src/b25_decode_d.c
  blip25c/src/b25_tables.c
  blip25c/src/b25_golay.c
  blip25c/src/blip25.c
)
set_target_properties(spear_blip25c PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
target_include_directories(spear_blip25c PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/blip25c/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/blip25c/src)
target_compile_options(spear_blip25c PRIVATE -w)

add_library(spear_std_t98_rx STATIC
  receiver.cpp
  ambe.cpp
  dsp/rrc.cpp
  dsp/symbol_sync.cpp
  dsp/sync_correlator.cpp
  secret/pn.cpp
  secret/guided.cpp
  secret/silence.cpp
  secret/models.cpp
  secret/safetensors.cpp
  secret/ffnn.cpp
  secret/hybrid.cpp
  secret/cracker.cpp
  secret/tracker.cpp
  secret/worker.cpp
)
target_include_directories(spear_std_t98_rx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(spear_std_t98_rx PUBLIC spear_std_t98_protocol spear_dsp spear_core PRIVATE spear_blip25c onnxruntime::onnxruntime)
# モデルの埋め込み(secret/models.cpp の .incbin)。モデルファイルが変われば再コンパイルされるように依存を張る
set(SPEAR_STD_T98_MODELS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/models)
set_source_files_properties(secret/models.cpp PROPERTIES
  COMPILE_DEFINITIONS "SPEAR_STD_T98_MODELS_DIR=${SPEAR_STD_T98_MODELS_DIR}"
  OBJECT_DEPENDS "${SPEAR_STD_T98_MODELS_DIR}/ambe2_ffnn.safetensors;${SPEAR_STD_T98_MODELS_DIR}/ambe2_hybrid.onnx")

if(SPEAR_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# ---- GUI App(登録は spear_add_app。受信機ライブラリは Qt 非依存のまま)----
if(COMMAND spear_add_app)
  spear_add_app(
    ID std_t98
    NAME "STD-T98 MONITOR"
    DESCRIPTION "ARIB STD-T98 97ch monitor: band spectrum, eye pattern, frames, AMBE audio"
    DIRECTION RX
    CLASS spear::apps::StdT98App
    HEADER std_t98_app.hpp
    PAGE StdT98Page.qml
    SOURCES std_t98_app.cpp
    LIBS spear_std_t98_rx
  )
endif()
