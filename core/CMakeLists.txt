# spear_core: Radio + Stream Bus + buffer + Source/Sink 抽象 (要件 §5.4: 2000行目安)
add_library(spear_core STATIC
  src/block.cpp
  src/stream.cpp
  src/source.cpp
  src/event.cpp
  src/event_log.cpp
  src/tap.cpp
  src/health.cpp
  src/synthetic_source.cpp
  src/recording.cpp
  src/sigmf.cpp
  src/core.cpp
  src/audio_sink.cpp
)
target_include_directories(spear_core PUBLIC include)
target_link_libraries(spear_core PUBLIC Threads::Threads PkgConfig::ALSA)
target_compile_definitions(spear_core PUBLIC
  $<$<BOOL:${SPEAR_TAP}>:SPEAR_TAP_ENABLED=1>
)

if(SPEAR_WITH_UHD)
  # 注意: device 消失からの再接続 (§17.2) には packaging/libuhd のパッチ済み libuhd が必要。
  # 素の UHD 4.9 は device 消失後の最初の USB 送信で std::terminate する(deleter 内の throw)。
  target_sources(spear_core PRIVATE src/radio.cpp src/b210_source.cpp)
  target_include_directories(spear_core PUBLIC ${UHD_INCLUDE_DIRS})
  target_link_libraries(spear_core PUBLIC ${UHD_LIBRARIES})
  target_compile_definitions(spear_core PUBLIC SPEAR_WITH_UHD=1)
endif()
