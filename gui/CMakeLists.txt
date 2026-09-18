# spear-gui: シェル (要件 §9)。App は apps/、部品は widgets/ input/ theme/、SDK は appfw/。
find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Qml Quick GuiPrivate)
qt_standard_project_setup(REQUIRES 6.6)
set(CMAKE_AUTOMOC ON)

qt_add_executable(spear-gui
  src/main.cpp
  src/system_model.cpp
  src/shell.cpp
  src/ui_audio.cpp
  src/tap_sound.cpp
  src/boot_sound.cpp
)
qt_add_qml_module(spear-gui
  URI Spear
  VERSION 1.0
  RESOURCE_PREFIX /qt/qml
  OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/qml/Spear
  QML_FILES
    qml/Main.qml
    qml/StatusBar.qml
    qml/SoftKeyBar.qml
    qml/MenuPage.qml
    qml/SplashPage.qml
    qml/ConfirmDialog.qml
    qml/DiagnosticsPage.qml
  SOURCES
    src/system_model.hpp
    src/shell.hpp
    src/tap_sound.hpp
    src/boot_sound.hpp
)
target_include_directories(spear-gui PRIVATE src)
target_compile_definitions(spear-gui PRIVATE QT_NO_KEYWORDS)
target_link_libraries(spear-gui PRIVATE spear_core spear_dsp spear_appfw spear_apps
  Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::GuiPrivate
  spear_themeplugin spear_inputplugin spear_widgetsplugin spear_appfwplugin)
