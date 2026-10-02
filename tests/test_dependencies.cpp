// Readiness cache invalidation and recovery (finding 12).
#include "converter.h"
#include "dependencies.h"
#include "error.h"
#include "process.h"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

int versionProbes(const fs::path& logDir) {
    return static_cast<int>(readLines(logDir / "yt-dlp.version.count").size());
}

} // namespace

class ReadinessTest : public ::testing::Test {
  protected:
    void SetUp() override {
        yt::process::resetShutdownForTests();
        yt::deps::clearReadyCacheForTests();
        logs_ = makeTestDir();
        tools_ = makeTestDir();
        setenv("YTCONV_FAKE_LOG_DIR", logs_.string().c_str(), 1);
        installTool();
        config_.yt_dlp_path = (tools_ / "yt-dlp").string();
        config_.ffmpeg_path = fakePath("ffmpeg");
        config_.ready_ttl_sec = 3600;
        config_.output_dir = (logs_ / "out").string();
    }

    void TearDown() override {
        unsetenv("YTCONV_FAKE_LOG_DIR");
        unsetenv("YTCONV_FAKE_VERSION_SLEEP");
        yt::deps::clearReadyCacheForTests();
    }

    // A private copy of the fake yt-dlp that a test can remove and restore.
    void installTool() {
        fs::copy_file(fakePath("yt-dlp"), tools_ / "yt-dlp", fs::copy_options::overwrite_existing);
        fs::permissions(tools_ / "yt-dlp", fs::perms::owner_all, fs::perm_options::replace);
    }

    fs::path logs_;
    fs::path tools_;
    yt::Config config_;
};

TEST_F(ReadinessTest, LaunchFailureForcesAReprobeInsideTheTtlAndRestoreRecovers) {
    ASSERT_TRUE(yt::deps::checkToolsCached(config_).ok);
    ASSERT_TRUE(yt::deps::checkToolsCached(config_).ok);
    EXPECT_EQ(versionProbes(logs_), 1); // cached

    fs::remove(tools_ / "yt-dlp");
    // Still "ready" from the cache: nothing has noticed the missing tool yet.
    EXPECT_TRUE(yt::deps::checkToolsCached(config_).ok);

    yt::converter::ConversionRequest request;
    request.url = "https://www.youtube.com/watch?v=dQw4w9WgXcQ";
    request.format = "mp3";
    request.config = config_;
    EXPECT_THROW(
        {
            try {
                yt::converter::processVideo(request);
            } catch (const yt::Error& error) {
                EXPECT_EQ(error.code(), yt::ErrorCode::BinaryNotFound);
                throw;
            }
        },
        yt::Error);

    // Inside the old TTL, readiness probes again and reports the missing tool.
    const auto missing = yt::deps::checkToolsCached(config_);
    EXPECT_FALSE(missing.ok);
    EXPECT_NE(missing.message.find("not found"), std::string::npos) << missing.message;

    installTool();
    EXPECT_TRUE(yt::deps::checkToolsCached(config_).ok);
}

TEST_F(ReadinessTest, ConfigurationsAreCachedSeparately) {
    ASSERT_TRUE(yt::deps::checkToolsCached(config_).ok);
    yt::Config other = config_;
    other.yt_dlp_path = (tools_ / "missing-yt-dlp").string();
    EXPECT_FALSE(yt::deps::checkToolsCached(other).ok);
    EXPECT_TRUE(yt::deps::checkToolsCached(config_).ok);
}

TEST_F(ReadinessTest, FailedProbesAreNotCached) {
    yt::Config missing = config_;
    missing.yt_dlp_path = (tools_ / "missing-yt-dlp").string();
    EXPECT_FALSE(yt::deps::checkToolsCached(missing).ok);
    fs::copy_file(tools_ / "yt-dlp", tools_ / "missing-yt-dlp");
    EXPECT_TRUE(yt::deps::checkToolsCached(missing).ok);
}

TEST_F(ReadinessTest, ZeroTtlProbesEveryTime) {
    config_.ready_ttl_sec = 0;
    yt::deps::checkToolsCached(config_);
    yt::deps::checkToolsCached(config_);
    EXPECT_EQ(versionProbes(logs_), 2);
}

TEST_F(ReadinessTest, SimultaneousChecksShareOneProbe) {
    setenv("YTCONV_FAKE_VERSION_SLEEP", "0.5", 1);
    std::vector<std::thread> threads;
    std::vector<int> ok(6, 0);
    for (std::size_t i = 0; i < ok.size(); ++i) {
        threads.emplace_back([&, i] { ok[i] = yt::deps::checkToolsCached(config_).ok ? 1 : 0; });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (int value : ok) {
        EXPECT_EQ(value, 1);
    }
    EXPECT_EQ(versionProbes(logs_), 1);
}

TEST_F(ReadinessTest, InvalidationDuringABlockedProbeIsNotUndone) {
    setenv("YTCONV_FAKE_VERSION_SLEEP", "1", 1);
    std::thread slow([&] { EXPECT_TRUE(yt::deps::checkToolsCached(config_).ok); });
    // The fake records the probe before sleeping: wait until it is in flight.
    ASSERT_TRUE(waitUntil([&] { return versionProbes(logs_) == 1; }));
    yt::deps::invalidateReadiness();
    slow.join();
    unsetenv("YTCONV_FAKE_VERSION_SLEEP");
    // The in-flight success must not have been published: this check probes again.
    EXPECT_TRUE(yt::deps::checkToolsCached(config_).ok);
    EXPECT_EQ(versionProbes(logs_), 2);
}
