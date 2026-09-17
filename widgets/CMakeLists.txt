# Spear.Widgets: 再利用可能な描画部品 (要件 §9.3)。App のページはこれらを静的に組み合わせる。
qt_add_library(spear_widgets STATIC)
qt_add_qml_module(spear_widgets
  URI Spear.Widgets
  VERSION 1.0
  QML_FILES
    qml/Readout.qml
    qml/SpectrumView.qml
    qml/LevelMeter.qml
    qml/EyeDiagram.qml
  RESOURCE_PREFIX /qt/qml
  OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/qml/Spear/Widgets
  NO_PLUGIN_OPTIONAL
)
target_link_libraries(spear_widgets PRIVATE spear_theme spear_input spear_appfw)
