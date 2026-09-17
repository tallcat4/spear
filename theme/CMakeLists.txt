# Spear.Theme: 共有デザイントークン(singleton)。docs/gui/DESIGN.md
qt_add_library(spear_theme STATIC)
set_source_files_properties(Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_qml_module(spear_theme
  URI Spear.Theme
  VERSION 1.0
  QML_FILES Theme.qml
  RESOURCE_PREFIX /qt/qml
  OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/qml/Spear/Theme
  NO_PLUGIN_OPTIONAL
)
