add_executable(spear_adsb_tests test_protocol.cpp test_receiver.cpp)
target_link_libraries(spear_adsb_tests PRIVATE spear_adsb_rx GTest::gtest GTest::gtest_main)
target_include_directories(spear_adsb_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_test(NAME spear_adsb_tests COMMAND spear_adsb_tests)
