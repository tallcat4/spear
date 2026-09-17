add_executable(spear-headless headless/main.cpp)
target_link_libraries(spear-headless PRIVATE spear_core spear_dsp)

if(SPEAR_WITH_UHD)
  add_executable(spear-soak soak/main.cpp)
  target_link_libraries(spear-soak PRIVATE spear_core)
  target_link_options(spear-soak PRIVATE -rdynamic)  # terminate 時の backtrace にシンボル名を出す
endif()

add_executable(spear-std-t98-decode std_t98_decode/main.cpp)
target_link_libraries(spear-std-t98-decode PRIVATE spear_std_t98_rx spear_core spear_dsp)
