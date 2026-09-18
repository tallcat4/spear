# Spear.Input: 自前の入力部品(タッチ専用、Virtual Keyboard / Quick Controls 非依存)。全 App から再利用。
qt_add_library(spear_input STATIC)
set_source_files_properties(Feedback.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_qml_module(spear_input
  URI Spear.Input
  VERSION 1.0
  QML_FILES
    KeyButton.qml
    Keypad.qml
    StepKeys.qml
    ValueField.qml
    NumericEntry.qml
    Feedback.qml
  RESOURCE_PREFIX /qt/qml
  OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/qml/Spear/Input
  NO_PLUGIN_OPTIONAL
)
target_link_libraries(spear_input PRIVATE spear_theme)
