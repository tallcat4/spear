# App の回帰テスト: ハードウェアなし(SyntheticSource)で App のライフサイクルと出力を検証する (§10.3)
add_executable(spear_app_tests test_apps.cpp)
target_link_libraries(spear_app_tests PRIVATE spear_apps spear_app_template GTest::gtest Qt6::Core PkgConfig::FFTW3F)
target_compile_definitions(spear_app_tests PRIVATE QT_NO_KEYWORDS)
add_test(NAME spear_app_tests COMMAND spear_app_tests)
# SyntheticSource を実時間で流して Lossless の drop 0 を確かめるテストを含むので、ほかのテストと並列に走らせない
# (2026-11-03: ctest -j4 で CPU の重い spear_std_t99_tests と重なり、STD-T98 のライフサイクルで drop 23。単独では 0)
set_tests_properties(spear_app_tests PROPERTIES RUN_SERIAL TRUE)

# 運転状態の自動保存(SettingsStore)と全 App の persist 宣言の検証
add_executable(spear_settings_tests test_settings.cpp)
set_target_properties(spear_settings_tests PROPERTIES AUTOMOC ON)
target_link_libraries(spear_settings_tests PRIVATE spear_apps spear_app_template GTest::gtest Qt6::Core)
target_compile_definitions(spear_settings_tests PRIVATE QT_NO_KEYWORDS)
add_test(NAME spear_settings_tests COMMAND spear_settings_tests)
