# テンプレート App。新しい App はこのディレクトリをコピーして始める(docs/app-development.md)。
# 登録するには apps/CMakeLists.txt に add_subdirectory(<id>) を足す。このテンプレート自体は登録しない。
spear_add_app(
  UNLISTED
  ID template
  NAME "TEMPLATE"
  DESCRIPTION "Copy me: minimal app showing the SDK contract"
  DIRECTION RX
  CLASS spear::apps::TemplateApp
  HEADER template_app.hpp
  PAGE TemplatePage.qml
  SOURCES template_app.cpp
)
