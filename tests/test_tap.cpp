#include "spear/core/tap.hpp"

#include <gtest/gtest.h>

#include <filesystem>

using namespace spear;

TEST(Tap, WritesRawFileInDevBuild) {
    const auto dir = std::filesystem::temp_directory_path() / "spear_tap_test";
    std::filesystem::remove_all(dir);
    tap::Channel::set_directory(dir.string());
    cf32 buf[16]{};
    TAP("test.channel", buf, 16);
    TAP("test.channel", buf, 16);
#if defined(SPEAR_TAP_ENABLED) && SPEAR_TAP_ENABLED
    // ファイルは Channel の static 寿命中開いたまま。書き込み済みサイズで確認。
    const auto p = dir / "test.channel.cf32.tap";
    ASSERT_TRUE(std::filesystem::exists(p));
#else
    EXPECT_FALSE(std::filesystem::exists(dir));
#endif
}
