add_executable(spear_slpr_tests test_receiver.cpp)
target_link_libraries(spear_slpr_tests PRIVATE spear_slpr_rx GTest::gtest GTest::gtest_main PkgConfig::FFTW3F)
target_include_directories(spear_slpr_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_test(NAME spear_slpr_tests COMMAND spear_slpr_tests)
