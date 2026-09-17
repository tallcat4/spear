# ---- App の登録 (docs/app-development.md) ----
# 各 App は apps/<id>/CMakeLists.txt で spear_add_app() を 1 回呼ぶ。ここで並べた順がメニューの順。
# レジストリ(app_registry.cpp)はビルド時に生成する。実行時の discovery は持たない (§3.1)。

set_property(GLOBAL PROPERTY SPEAR_APP_IDS "")

# spear_add_app(ID spectrum NAME "SPECTRUM" DESCRIPTION "..." DIRECTION RX
#               CLASS spear::apps::SpectrumApp HEADER spectrum_app.hpp PAGE SpectrumPage.qml
#               SOURCES spectrum_app.cpp ... [LIBS ...])
# UNLISTED: ビルドはするがメニューに出さない(テンプレート等)
function(spear_add_app)
  cmake_parse_arguments(A "UNLISTED" "ID;NAME;DESCRIPTION;DIRECTION;CLASS;HEADER;PAGE" "SOURCES;LIBS;QML_FILES" ${ARGN})
  string(SUBSTRING "${A_ID}" 0 1 _first)
  string(TOUPPER "${_first}" _first)
  string(SUBSTRING "${A_ID}" 1 -1 _rest)
  set(_camel "${_first}${_rest}")
  set(_target spear_app_${A_ID})
  qt_add_library(${_target} STATIC ${A_SOURCES} ${A_HEADER})
  set_target_properties(${_target} PROPERTIES AUTOMOC ON)
  target_include_directories(${_target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${_target} PUBLIC spear_appfw ${A_LIBS})
  qt_add_qml_module(${_target}
    URI Spear.Apps.${_camel}
    VERSION 1.0
    QML_FILES ${A_PAGE} ${A_QML_FILES}
    RESOURCE_PREFIX /qt/qml
    OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/qml/Spear/Apps/${_camel}
    NO_PLUGIN_OPTIONAL
  )
  target_link_libraries(${_target} PRIVATE spear_theme spear_input spear_widgets)
  # レジストリ生成用の情報
  if(A_UNLISTED)
    return()
  endif()
  set_property(GLOBAL APPEND PROPERTY SPEAR_APP_IDS ${A_ID})
  set_property(GLOBAL PROPERTY SPEAR_APP_${A_ID}_INFO
    "{\"${A_ID}\", \"${A_NAME}\", \"${A_DESCRIPTION}\", spear::Direction::${A_DIRECTION}, \"qrc:/qt/qml/Spear/Apps/${_camel}/${A_PAGE}\"}")
  set_property(GLOBAL PROPERTY SPEAR_APP_${A_ID}_CLASS "${A_CLASS}")
  set_property(GLOBAL PROPERTY SPEAR_APP_${A_ID}_HEADER "${A_HEADER}")
  set_property(GLOBAL PROPERTY SPEAR_APP_${A_ID}_TARGET "${_target}")
endfunction()

add_subdirectory(spectrum)
add_subdirectory(recorder)
add_subdirectory(demod)
add_subdirectory(std_t98)
add_subdirectory(template)   # UNLISTED: コンパイルだけ(腐らせない)

# ---- レジストリ生成 ----
get_property(_ids GLOBAL PROPERTY SPEAR_APP_IDS)
set(_reg "// 自動生成: apps/CMakeLists.txt。手で編集しない。\n#include \"spear/appfw/app.hpp\"\n")
set(_libs "")
foreach(_id ${_ids})
  get_property(_hdr GLOBAL PROPERTY SPEAR_APP_${_id}_HEADER)
  get_property(_tgt GLOBAL PROPERTY SPEAR_APP_${_id}_TARGET)
  string(APPEND _reg "#include \"${_hdr}\"\n")
  list(APPEND _libs ${_tgt} ${_tgt}plugin)
endforeach()
string(APPEND _reg "\nnamespace spear::appfw {\nconst std::vector<AppEntry>& registered_apps() {\n    static const std::vector<AppEntry> apps = {\n")
foreach(_id ${_ids})
  get_property(_info GLOBAL PROPERTY SPEAR_APP_${_id}_INFO)
  get_property(_cls GLOBAL PROPERTY SPEAR_APP_${_id}_CLASS)
  string(APPEND _reg "        {AppInfo${_info}, [] { return std::static_pointer_cast<GuiApp>(std::make_shared<${_cls}>(AppInfo${_info})); }},\n")
endforeach()
string(APPEND _reg "    };\n    return apps;\n}\n} // namespace spear::appfw\n")
file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/app_registry.cpp CONTENT "${_reg}")
add_library(spear_apps STATIC ${CMAKE_CURRENT_BINARY_DIR}/app_registry.cpp)
target_link_libraries(spear_apps PUBLIC spear_appfw ${_libs})

if(SPEAR_BUILD_TESTS)
  add_subdirectory(tests)
endif()
