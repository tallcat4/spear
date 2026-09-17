add_executable(spear_std_t98_tests test_protocol.cpp test_receiver.cpp test_ambe.cpp test_secret.cpp)
target_link_libraries(spear_std_t98_tests PRIVATE spear_std_t98_rx GTest::gtest GTest::gtest_main)
target_include_directories(spear_std_t98_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_test(NAME spear_std_t98_tests COMMAND spear_std_t98_tests)
