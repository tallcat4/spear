add_executable(spear_std_t99_tests test_receiver.cpp)
target_link_libraries(spear_std_t99_tests PRIVATE spear_std_t99_rx GTest::gtest GTest::gtest_main)
target_include_directories(spear_std_t99_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_test(NAME spear_std_t99_tests COMMAND spear_std_t99_tests)
