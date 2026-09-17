# App: ADS-B(1090 MHz Mode S / Extended Squitter)。docs/apps/adsb.md
#   protocol/  純 C++ プロトコルデコーダ(CRC-24、DF 判別、高度/スコーク、CPR)
#   dsp/       App 内 DSP(プリアンブル検出、PPM ビット判定)
#   receiver   純 C++ 受信機(回転 → 低域 FIR → 振幅 → PPM → CRC)。Qt/UHD 非依存 → headless / テスト可能 (§11)
#   aircraft   航空機表(ICAO ごとの集約、CPR 偶奇ペア、期限切れ)
#   map/       ミニマップ: 地図データ(Natural Earth + OurAirports、build_map.py で生成、バイナリに埋め込む)、表示範囲の自動フィット、QQuickPaintedItem
add_library(spear_adsb_rx STATIC
  protocol/modes.cpp
  dsp/ppm.cpp
  receiver.cpp
  aircraft.cpp
  map/map_data.cpp
)
target_include_directories(spear_adsb_rx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(spear_adsb_rx PUBLIC spear_dsp spear_core)
# 地図の埋め込み(map/map_data.cpp の .incbin)。データが変われば再コンパイルされるように依存を張る
set(SPEAR_ADSB_MAP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/map)
set_source_files_properties(map/map_data.cpp PROPERTIES
  COMPILE_DEFINITIONS "SPEAR_ADSB_MAP_DIR=${SPEAR_ADSB_MAP_DIR}"
  OBJECT_DEPENDS "${SPEAR_ADSB_MAP_DIR}/world.spearmap")

if(SPEAR_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# ---- GUI App(登録は spear_add_app。受信機ライブラリは Qt 非依存のまま)----
if(COMMAND spear_add_app)
  spear_add_app(
    ID adsb
    NAME "ADS-B"
    DESCRIPTION "1090 MHz Mode S / ADS-B: aircraft table, polar plot, pulse trace"
    DIRECTION RX
    CLASS spear::apps::AdsbApp
    HEADER adsb_app.hpp
    PAGE AdsbPage.qml
    SOURCES adsb_app.cpp map/map_item.cpp map/map_item.hpp
    LIBS spear_adsb_rx
  )
  target_include_directories(spear_app_adsb PRIVATE map)   # 生成される QML 型登録が <map_item.hpp> を basename で include する
endif()
