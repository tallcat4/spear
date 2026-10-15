# 参照ベクトル(apps/jr_train/tests/vectors/voice)は tests/refgen/src/bin/voice.rs が原本の Rust から生成したもの
add_executable(spear_jr_train_voice_tests test_voice.cpp)
target_link_libraries(spear_jr_train_voice_tests PRIVATE spear_jr_train_voice GTest::gtest GTest::gtest_main)
target_include_directories(spear_jr_train_voice_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_options(spear_jr_train_voice_tests PRIVATE -ffp-contract=off)
target_compile_definitions(spear_jr_train_voice_tests PRIVATE JR_TRAIN_VOICE_VECTORS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/vectors/voice")
add_test(NAME spear_jr_train_voice_tests COMMAND spear_jr_train_voice_tests)
