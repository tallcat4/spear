add_executable(spear_tests
  test_block.cpp
  test_stream_bus.cpp
  test_synthetic.cpp
  test_recording.cpp
  test_tap.cpp
  test_state.cpp
  test_channelizer.cpp
)
target_link_libraries(spear_tests PRIVATE spear_core spear_dsp GTest::gtest GTest::gtest_main PkgConfig::FFTW3F)
include(GoogleTest)
gtest_discover_tests(spear_tests)
