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
        }
    } restore;

    const yt::Config config = yt::loadConfigFromEnv();
    EXPECT_EQ(config.port, 9090);
    EXPECT_EQ(config.bind, "127.0.0.1");
    EXPECT_EQ(config.max_concurrent, 2);
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
