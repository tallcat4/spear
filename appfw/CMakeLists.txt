# spear_appfw: App SDK (Qt 依存)。Core と GUI シェルの間に立ち、App が使う土台を提供する。
find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Qml Quick GuiPrivate)
set(CMAKE_AUTOMOC ON)

add_library(spear_appfw STATIC
  src/view_processor.cpp
  src/spectrum_item.cpp
  src/waterfall_item.cpp
  src/eye_diagram_item.cpp
  include/spear/appfw/app.hpp
  include/spear/appfw/view_source.hpp
  include/spear/appfw/spectrum_item.hpp
  include/spear/appfw/waterfall_item.hpp
  include/spear/appfw/trace_source.hpp
  include/spear/appfw/eye_diagram_item.hpp
)
target_include_directories(spear_appfw PUBLIC include PRIVATE include/spear/appfw)  # PRIVATE: qmltyperegistrations が basename で include する
target_link_libraries(spear_appfw PUBLIC spear_core spear_dsp Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::GuiPrivate)
target_compile_definitions(spear_appfw PUBLIC QT_NO_KEYWORDS)

# QML 型(SpectrumItem / WaterfallItem / ViewSource / TraceSource / EyeDiagramItem)を Spear.AppFw モジュールとして登録
qt_add_qml_module(spear_appfw
  URI Spear.AppFw
  VERSION 1.0
  RESOURCE_PREFIX /qt/qml
  OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/qml/Spear/AppFw
  NO_PLUGIN_OPTIONAL
)
