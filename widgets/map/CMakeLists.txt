# 地図(ADS-B から抽出、2026-11-03。ADS-B と STD-T99 で共有。docs/dsp-boundary.md の昇格ルール)
#   spear_map_data  地図データ(Natural Earth 1:10m + OurAirports、build_map.py で生成した world.spearmap を埋め込む)と表示範囲。Qt 非依存
#   spear_map       下地の描画と表示範囲の状態(Qt Gui、QObject ではない)。重ねて描くものとタッチは各 App の QQuickPaintedItem
add_library(spear_map_data STATIC map_data.cpp)
target_include_directories(spear_map_data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(SPEAR_MAP_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set_source_files_properties(map_data.cpp PROPERTIES
  COMPILE_DEFINITIONS "SPEAR_MAP_DIR=${SPEAR_MAP_DIR}"
  OBJECT_DEPENDS "${SPEAR_MAP_DIR}/world.spearmap")

add_library(spear_map STATIC base_layer.cpp)
target_link_libraries(spear_map PUBLIC spear_map_data Qt6::Gui)
