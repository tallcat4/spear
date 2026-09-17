# App の回帰テスト: ハードウェアなし(SyntheticSource)で App のライフサイクルと出力を検証する (§12.3)
add_executable(spear_app_tests test_apps.cpp)
target_link_libraries(spear_app_tests PRIVATE spear_apps spear_app_template GTest::gtest Qt6::Core PkgConfig::FFTW3F)
target_compile_definitions(spear_app_tests PRIVATE QT_NO_KEYWORDS)
add_test(NAME spear_app_tests COMMAND spear_app_tests)

# 運転状態の自動保存(SettingsStore)と全 App の persist 宣言の検証
add_executable(spear_settings_tests test_settings.cpp)
set_target_properties(spear_settings_tests PROPERTIES AUTOMOC ON)
target_link_libraries(spear_settings_tests PRIVATE spear_apps spear_app_template GTest::gtest Qt6::Core)
target_compile_definitions(spear_settings_tests PRIVATE QT_NO_KEYWORDS)
add_test(NAME spear_settings_tests COMMAND spear_settings_tests)
