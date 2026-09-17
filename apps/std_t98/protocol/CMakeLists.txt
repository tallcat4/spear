# STD-T98 プロトコルデコーダ(純 C++、Qt/SDR 非依存、テスト可能)
add_library(spear_std_t98_protocol STATIC protocol.cpp)
target_include_directories(spear_std_t98_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
