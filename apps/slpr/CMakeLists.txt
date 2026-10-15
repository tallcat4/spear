# App: 特定小電力 20ch 同時受信(422 MHz 帯、単信、NFM)。docs/apps/slpr.md
#   channels.hpp  チャネル表(ICOM 準拠の 01–20、周波数、PFB の bin)— 唯一の真値
#   receiver      純 C++ 受信機(20ch)。Qt/UHD 非依存 → テスト可能 (§10.4)
add_library(spear_slpr_rx STATIC receiver.cpp)
target_include_directories(spear_slpr_rx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(spear_slpr_rx PUBLIC spear_dsp spear_core)

if(SPEAR_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# ---- GUI App(登録は spear_add_app。受信機ライブラリは Qt 非依存のまま)----
if(COMMAND spear_add_app)
  spear_add_app(
    ID slpr
    NAME "SLPR 20CH RX"
    DESCRIPTION "Specified low power radio (422 MHz simplex 20ch): simultaneous NFM receiver"
    DIRECTION RX
    CLASS spear::apps::SlprApp
    HEADER slpr_app.hpp
    PAGE SlprPage.qml
    SOURCES slpr_app.cpp
    LIBS spear_slpr_rx
  )
endif()
