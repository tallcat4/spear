# 汎用 DSP ブロック (要件 §8.3): App 間で共有するコード。インフラではない。
add_library(spear_dsp STATIC src/spectrum.cpp src/fir.cpp src/channelizer.cpp src/fftw_planner.cpp)
target_include_directories(spear_dsp PUBLIC include)
target_link_libraries(spear_dsp PUBLIC spear_core PkgConfig::FFTW3F)
