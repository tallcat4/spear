# App: STD-T98 モニタ (§14 M3)。
#   protocol/  純 C++ プロトコルデコーダ
#   dsp/       App 内 DSP(RRC 設計、Gardner symbol sync、同期語検出)
#   receiver   純 C++ 受信機(30ch)。Qt/UHD 非依存 → headless / テスト可能 (§11)
#   ambe/      AMBE 3600x2450 → PCM 8 kHz(mbelib-neo の AMBE 経路を C++ 化、GPL-2.0-or-later。pyambelib と同一結果)
add_subdirectory(protocol)

add_library(spear_std_t98_rx STATIC
  receiver.cpp
  ambe.cpp
  ambe/decoder.cpp
  dsp/rrc.cpp
  dsp/symbol_sync.cpp
  dsp/sync_correlator.cpp
)
target_include_directories(spear_std_t98_rx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(spear_std_t98_rx PUBLIC spear_std_t98_protocol spear_dsp spear_core)

if(SPEAR_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# ---- GUI App(登録は spear_add_app。受信機ライブラリは Qt 非依存のまま)----
if(COMMAND spear_add_app)
  spear_add_app(
    ID std_t98
    NAME "STD-T98 MONITOR"
    DESCRIPTION "ARIB STD-T98 30ch monitor: band spectrum, eye pattern, frames, AMBE audio"
    DIRECTION RX
    CLASS spear::apps::StdT98App
    HEADER std_t98_app.hpp
    PAGE StdT98Page.qml
    SOURCES std_t98_app.cpp
    LIBS spear_std_t98_rx
  )
endif()
