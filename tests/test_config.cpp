#include "config.h"
#include "error.h"

#include <gtest/gtest.h>

#include <cstdlib>

namespace {

void setVar(const char* name, const char* value) { setenv(name, value, 1); }

void clearVar(const char* name) { unsetenv(name); }

} // namespace

TEST(Config, ReadsEnvironmentAndRejectsRemoteWithoutKey) {
    setVar("YTCONV_BIND", "127.0.0.1");
    setVar("YTCONV_PORT", "9090");
    setVar("YTCONV_LOG_LEVEL", "DEBUG");
    setVar("YTCONV_OUTPUT_DIR", "/tmp/ytconv-out");
    setVar("YTCONV_MAX_CONCURRENT", "2");
    setVar("YTCONV_CONCURRENT_FRAGMENTS", "8");
    setVar("YTCONV_FRAGMENT_RETRIES", "20");
    setVar("YTCONV_DOWNLOAD_TIMEOUT_SEC", "300");
    setVar("YTCONV_CONVERT_TIMEOUT_SEC", "120");
    clearVar("YTCONV_API_KEY");
    clearVar("YTCONV_ALLOW_REMOTE");
    clearVar("YTCONV_ALLOW_UNAUTHENTICATED_LOCALHOST");
    struct EnvGuard {
        ~EnvGuard() {
            clearVar("YTCONV_BIND");
            clearVar("YTCONV_PORT");
            clearVar("YTCONV_LOG_LEVEL");
            clearVar("YTCONV_OUTPUT_DIR");
            clearVar("YTCONV_MAX_CONCURRENT");
            clearVar("YTCONV_CONCURRENT_FRAGMENTS");
            clearVar("YTCONV_FRAGMENT_RETRIES");
            clearVar("YTCONV_DOWNLOAD_TIMEOUT_SEC");
            clearVar("YTCONV_CONVERT_TIMEOUT_SEC");
        }
    } restore;

    const yt::Config config = yt::loadConfigFromEnv();
    EXPECT_EQ(config.port, 9090);
    EXPECT_EQ(config.bind, "127.0.0.1");
    EXPECT_EQ(config.max_concurrent, 2);
    EXPECT_EQ(config.concurrent_fragments, 8);
    EXPECT_EQ(config.fragment_retries, 20);
    EXPECT_EQ(config.download_timeout_sec, 300);
    EXPECT_EQ(config.convert_timeout_sec, 120);
    EXPECT_THROW(yt::validateApiConfig(config), yt::Error);

    yt::Config allowed = config;
    allowed.allow_unauthenticated_localhost = true;
    EXPECT_NO_THROW(yt::validateApiConfig(allowed));

    yt::Config remote = config;
    remote.bind = "0.0.0.0";
    remote.allow_remote = true;
    remote.api_key = "short";
    EXPECT_THROW(yt::validateApiConfig(remote), yt::Error);
    remote.api_key = "supersecret";
    EXPECT_NO_THROW(yt::validateApiConfig(remote));

    EXPECT_TRUE(yt::isLoopbackBind("localhost"));
    EXPECT_TRUE(yt::isWildcardBind("::"));
    EXPECT_TRUE(yt::constantTimeEquals("abc", "abc"));
    EXPECT_FALSE(yt::constantTimeEquals("abc", "abd"));
}

TEST(Config, ParsesHumanFilesize) {
    EXPECT_EQ(yt::parseHumanSize("500M"), 500LL * 1024 * 1024);
    EXPECT_EQ(yt::parseHumanSize("1G"), 1024LL * 1024 * 1024);
    EXPECT_EQ(yt::parseHumanSize("100K"), 100LL * 1024);
    EXPECT_EQ(yt::parseHumanSize("100KB"), 100LL * 1000);
    EXPECT_EQ(yt::parseHumanSize("1MB"), 1000LL * 1000);
    EXPECT_EQ(yt::parseHumanSize("123"), 123);
    EXPECT_THROW(yt::parseHumanSize("bad"), yt::Error);
    EXPECT_THROW(yt::parseHumanSize(""), yt::Error);
    EXPECT_THROW(yt::parseHumanSize("0M"), yt::Error);
}

TEST(Config, ParsesHumanFilesizePowersOf1024) {
    // Test that K/M/G without B suffix use 1024-based multipliers
    EXPECT_EQ(yt::parseHumanSize("1K"), 1024);
    EXPECT_EQ(yt::parseHumanSize("1M"), 1024 * 1024);
    EXPECT_EQ(yt::parseHumanSize("1G"), 1024 * 1024 * 1024);
}

TEST(Config, ParsesHumanFilesizePowersOf1000) {
    // Test that KB/MB/GB with B suffix use 1000-based multipliers
    EXPECT_EQ(yt::parseHumanSize("1KB"), 1000);
    EXPECT_EQ(yt::parseHumanSize("1MB"), 1000 * 1000);
    EXPECT_EQ(yt::parseHumanSize("1GB"), 1000 * 1000 * 1000);
}
