#include "process.h"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>

TEST(Process, EchoPreservesMetacharactersAsSeparateArguments) {
    const auto dir = makeTestDir();
    const auto script = fakePath("yt-dlp");
    ASSERT_TRUE(std::filesystem::exists(script));

    setenv("YTCONV_FAKE_LOG_DIR", dir.string().c_str(), 1);
    const auto result = yt::process::run(
        {script, "-o", (dir / "source.%(ext)s").string(), "https://example.test/'$; touch"});
    unsetenv("YTCONV_FAKE_LOG_DIR");

    EXPECT_EQ(result.exit_code, 0);
    EXPECT_FALSE(result.not_found);
    EXPECT_FALSE(result.timed_out);
    const std::string argv = readFile(dir / "yt-dlp.argv");
    EXPECT_NE(argv.find("https://example.test/'$; touch"), std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(dir / "source.mp4"));
}

TEST(Process, MissingBinaryIsNotFound) {
    const auto result = yt::process::run({"/this/binary/does/not/exist-ytconv"});
    EXPECT_TRUE(result.not_found);
    EXPECT_EQ(result.exit_code, 127);
}

TEST(Process, NonZeroExitIsPreserved) {
    const auto result = yt::process::run({fakePath("fail")});
    EXPECT_EQ(result.exit_code, 1);
    EXPECT_NE(result.stderr_text.find("simulated failure"), std::string::npos);
}

TEST(Process, TimeoutKillsHungChild) {
    yt::process::RunOptions options;
    options.timeout_ms = 400;
    const auto started = std::chrono::steady_clock::now();
    const auto result = yt::process::run({fakePath("hang")}, options);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_TRUE(result.timed_out);
    EXPECT_LT(elapsed, std::chrono::seconds(5));
}

TEST(Process, TrueUtilityExitsZero) {
    const auto result = yt::process::run({"/bin/true"});
    EXPECT_EQ(result.exit_code, 0);
}
