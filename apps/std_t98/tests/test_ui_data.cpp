#include "petname.hpp"
#include "secret_cache.hpp"
#include <QTemporaryDir>
#include <gtest/gtest.h>
using namespace spear;
TEST(StdT98Petname, SampleDictionaryAndFullCsm) {
    char name[64];
    std_t98::petname(123456789, name, sizeof(name));
    EXPECT_STREQ(name, "Lonely Soft Seal");
    EXPECT_EQ(std_t98::csm_petname("0075BCD15"), "Lonely Soft Seal");
    EXPECT_NE(std_t98::csm_petname("1075BCD15"), std_t98::csm_petname("0075BCD15"));
    EXPECT_TRUE(std_t98::csm_petname("bad input").empty());
    EXPECT_TRUE(std_t98::csm_petname("").empty());
}
TEST(StdT98CacheFile, SharpFormatRoundTripAndValidation) {
    QTemporaryDir dir;
    const auto path = dir.filePath("secret-cache.json");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(R"({"Version":1,"Codes":["12345","10-12345","15-32767","12345","00-12345","16-00001","00000","32768","abcde"]})");
    file.close();
    apps::SecretCacheFile cache(path);
    ASSERT_TRUE(cache.warning().isEmpty());
    ASSERT_EQ(cache.codes().size(), 3u);
    cache.remember({1, 1});
    apps::SecretCacheFile reloaded(path);
    EXPECT_EQ(reloaded.codes().size(), 4u);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto obj = QJsonDocument::fromJson(file.readAll()).object();
    EXPECT_EQ(obj["Version"].toInt(), 1);
    EXPECT_EQ(obj["Codes"].toArray(), (QJsonArray{"12345", "01-00001", "10-12345", "15-32767"}));
}
TEST(StdT98CacheFile, BrokenFileIsPreservedAndWriteFailureReported) {
    QTemporaryDir dir;
    const auto path = dir.filePath("secret-cache.json");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("broken"); file.close();
    apps::SecretCacheFile cache(path);
    ASSERT_FALSE(cache.warning().isEmpty());
    cache.remember({0, 12345});
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), "broken");
    apps::SecretCacheFile unwritable(dir.filePath("missing/cache.json"));
    unwritable.remember({0, 12345});
    EXPECT_FALSE(unwritable.warning().isEmpty());
}
