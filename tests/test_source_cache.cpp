// Source-generation leases, refresh, and eviction (finding 4). Races are driven by marker-file
// barriers in the ffmpeg-copy fake, never by sleeps.
#include "converter.h"
#include "error.h"
#include "process.h"
#include "source_cache.h"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef YTCONV_CLI_PATH
#define YTCONV_CLI_PATH ""
#endif

namespace fs = std::filesystem;

namespace {

constexpr const char* kVideoA = "dQw4w9WgXcQ";
constexpr const char* kVideoB = "jNQXAC9IVRw";

std::vector<fs::path> entries(const fs::path& output, const std::string& video,
                              const std::string& prefix, const std::string& kind = "audio") {
    std::vector<fs::path> found;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(output / "cache" / "src" / video / kind, ec)) {
        if (entry.path().filename().string().rfind(prefix, 0) == 0) {
            found.push_back(entry.path());
        }
    }
    return found;
}

std::vector<fs::path> generations(const fs::path& output, const std::string& video,
                                  const std::string& kind = "audio") {
    return entries(output, video, "gen-", kind);
}

// Waits for the gated fake to signal that an encode has started (bounded, not a race sleep).
bool waitForStarted(const fs::path& gate, int expected = 1) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        int started = 0;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(gate, ec)) {
            if (entry.path().filename().string().rfind("started.", 0) == 0) {
                ++started;
            }
        }
        if (started >= expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

struct Outcome {
    std::optional<yt::converter::ConversionResult> result;
    std::exception_ptr error;
};

// Runs work on a thread. If an assertion returns early, the destructor opens the gate and joins,
// so a failure is reported instead of terminating on a joinable thread.
class GatedWorker {
  public:
    GatedWorker(fs::path gate, std::function<void()> work)
        : gate_(std::move(gate)), thread_(std::move(work)) {}
    GatedWorker(const GatedWorker&) = delete;
    GatedWorker& operator=(const GatedWorker&) = delete;
    ~GatedWorker() { join(); }

    void join() {
        if (thread_.joinable()) {
            writeFile(gate_ / "release", "");
            thread_.join();
        }
    }

  private:
    fs::path gate_;
    std::thread thread_;
};

std::function<void()> conversion(yt::converter::ConversionRequest request, Outcome& outcome) {
    return [request = std::move(request), &outcome] {
        try {
            outcome.result = yt::converter::processVideo(request);
        } catch (...) {
            outcome.error = std::current_exception();
        }
    };
}

void writeGeneration(const fs::path& output, const std::string& video, std::int64_t stamp,
                     const std::string& suffix, const std::string& content) {
    char name[64];
    std::snprintf(name, sizeof(name), "gen-%020lld-%s", static_cast<long long>(stamp),
                  suffix.c_str());
    const fs::path dir = output / "cache" / "src" / video / "audio" / name;
    fs::create_directories(dir);
    writeFile(dir / "source.m4a", content);
}

} // namespace

class SourceCacheTest : public ::testing::Test {
  protected:
    void SetUp() override {
        yt::process::resetShutdownForTests();
        output_ = makeTestDir();
        gate_ = makeTestDir();
        setenv("YTCONV_FAKE_LOG_DIR", output_.string().c_str(), 1);
        setenv("YTCONV_GATE_DIR", gate_.string().c_str(), 1);
        setenv("YTCONV_GATE_FORMAT", "mp3", 1);
    }

    void TearDown() override {
        // Never leave a gated child waiting.
        writeFile(gate_ / "release", "");
        for (const char* name : {"YTCONV_FAKE_LOG_DIR", "YTCONV_GATE_DIR", "YTCONV_GATE_FORMAT",
                                 "YTCONV_FAKE_CONTENT", "YTCONV_FAKE_EXT"}) {
            unsetenv(name);
        }
    }

    yt::converter::ConversionRequest request(const std::string& video, const std::string& format,
                                             const std::string& ytdlp = fakePath("yt-dlp")) const {
        yt::converter::ConversionRequest req;
        req.url = "https://www.youtube.com/watch?v=" + video;
        req.format = format;
        req.config.output_dir = output_.string();
        req.config.yt_dlp_path = ytdlp;
        req.config.ffmpeg_path = fakePath("ffmpeg-copy");
        req.config.download_timeout_sec = 30;
        req.config.convert_timeout_sec = 30;
        req.config.source_cache_ttl_sec = 3600;
        return req;
    }

    fs::path output_;
    fs::path gate_;
};

TEST_F(SourceCacheTest, LeasedSourceSurvivesEvictionDuringEncode) {
    setenv("YTCONV_FAKE_CONTENT", "SOURCE-A", 1);
    auto first = request(kVideoA, "mp3");
    first.config.source_cache_max_bytes = 4; // every source is larger than the cap
    Outcome encodeA;
    GatedWorker worker(gate_, conversion(first, encodeA));
    ASSERT_TRUE(waitForStarted(gate_)) << "encode of A never started";

    // While A's encoder is parked before opening its input, B's download forces eviction.
    setenv("YTCONV_FAKE_CONTENT", "SOURCE-B", 1);
    auto second = request(kVideoB, "wav");
    second.config.source_cache_max_bytes = 4;
    const auto resultB = yt::converter::processVideo(second);
    EXPECT_EQ(readFile(resultB.output_path), "OUT:SOURCE-B");
    ASSERT_EQ(generations(output_, kVideoA).size(), 1u) << "leased generation was evicted";

    worker.join();
    ASSERT_FALSE(encodeA.error) << "encode of A failed after eviction pressure";
    EXPECT_EQ(readFile(encodeA.result->output_path), "OUT:SOURCE-A");
    // Idle maintenance restores the cap: nothing is leased and both sources exceed it.
    EXPECT_TRUE(generations(output_, kVideoA).empty());
    EXPECT_TRUE(generations(output_, kVideoB).empty());
}

TEST_F(SourceCacheTest, RefreshDuringEncodePublishesANewGeneration) {
    setenv("YTCONV_FAKE_CONTENT", "GEN-ONE", 1);
    Outcome encodeOld;
    GatedWorker worker(gate_, conversion(request(kVideoA, "mp3"), encodeOld));
    ASSERT_TRUE(waitForStarted(gate_));
    const auto oldGeneration = generations(output_, kVideoA);
    ASSERT_EQ(oldGeneration.size(), 1u);

    setenv("YTCONV_FAKE_CONTENT", "GEN-TWO", 1);
    auto refresh = request(kVideoA, "wav");
    refresh.refresh = true;
    const auto refreshed = yt::converter::processVideo(refresh);
    EXPECT_EQ(readFile(refreshed.output_path), "OUT:GEN-TWO");
    EXPECT_NE(refreshed.source_generation, oldGeneration.front().filename().string());
    // The old generation is retired but still leased by the parked encoder.
    EXPECT_TRUE(fs::exists(oldGeneration.front()));

    worker.join();
    ASSERT_FALSE(encodeOld.error);
    EXPECT_EQ(readFile(encodeOld.result->output_path), "OUT:GEN-ONE");
    EXPECT_EQ(encodeOld.result->source_generation, oldGeneration.front().filename().string());
    // Once released, the retired generation is reclaimed and only the refreshed one remains.
    const auto remaining = generations(output_, kVideoA);
    ASSERT_EQ(remaining.size(), 1u);
    EXPECT_EQ(remaining.front().filename().string(), refreshed.source_generation);
}

TEST_F(SourceCacheTest, ExpiredGenerationsAreRemovedAndNotReused) {
    const std::int64_t now = yt::cache::nowStamp();
    const std::int64_t twoDaysAgo = now - 2LL * 86400LL * 1000000000LL;
    writeGeneration(output_, kVideoA, twoDaysAgo, "0000000000000001", "OLD");
    yt::cache::SourceCache cache(output_);
    yt::cache::Limits limits;
    limits.ttl_sec = 3600;
    EXPECT_FALSE(cache.acquire(kVideoA, "audio", limits).has_value());
    const auto stats = cache.maintain(limits);
    EXPECT_EQ(stats.removed_entries, 1u);
    EXPECT_TRUE(generations(output_, kVideoA).empty());
}

TEST_F(SourceCacheTest, FreshGenerationIsLeasedAndOlderOnesRetire) {
    const std::int64_t now = yt::cache::nowStamp();
    writeGeneration(output_, kVideoA, now - 2000000000LL, "0000000000000001", "OLDER");
    writeGeneration(output_, kVideoA, now - 1000000000LL, "0000000000000002", "NEWER");
    yt::cache::SourceCache cache(output_);
    yt::cache::Limits limits;
    limits.ttl_sec = 3600;
    auto lease = cache.acquire(kVideoA, "audio", limits);
    ASSERT_TRUE(lease.has_value());
    EXPECT_EQ(readFile(lease->source()), "NEWER");
    // minStamp excludes generations that started before it.
    EXPECT_FALSE(cache.acquire(kVideoA, "audio", limits, now).has_value());
    const auto stats = cache.maintain(limits);
    EXPECT_EQ(stats.removed_entries, 1u); // the older, unleased generation
    EXPECT_EQ(stats.leased_entries, 1u);
    EXPECT_EQ(generations(output_, kVideoA).size(), 1u);
}

TEST_F(SourceCacheTest, AllLeasedEntriesReportPressureInsteadOfDeleting) {
    const std::int64_t now = yt::cache::nowStamp();
    writeGeneration(output_, kVideoA, now - 1000, "0000000000000001", "AAAAAAAAAA");
    writeGeneration(output_, kVideoB, now - 500, "0000000000000002", "BBBBBBBBBB");
    yt::cache::SourceCache cache(output_);
    yt::cache::Limits limits;
    limits.ttl_sec = 3600;
    limits.max_bytes = 5;
    auto leaseA = cache.acquire(kVideoA, "audio", limits);
    auto leaseB = cache.acquire(kVideoB, "audio", limits);
    ASSERT_TRUE(leaseA && leaseB);

    auto stats = cache.maintain(limits);
    EXPECT_TRUE(stats.pressure);
    EXPECT_EQ(stats.removed_entries, 0u);
    EXPECT_EQ(stats.leased_bytes, 20u);
    EXPECT_EQ(stats.retained_bytes, 0u);
    EXPECT_EQ(generations(output_, kVideoA).size(), 1u);
    EXPECT_EQ(generations(output_, kVideoB).size(), 1u);

    leaseA.reset();
    leaseB.reset();
    stats = cache.maintain(limits);
    EXPECT_FALSE(stats.pressure);
    EXPECT_LE(stats.retained_bytes, 5u);
    EXPECT_TRUE(generations(output_, kVideoA).empty());
    EXPECT_TRUE(generations(output_, kVideoB).empty());
}

TEST_F(SourceCacheTest, CapEvictsOldestUnleasedFirst) {
    const std::int64_t now = yt::cache::nowStamp();
    writeGeneration(output_, kVideoA, now - 2000, "0000000000000001", "AAAA");
    writeGeneration(output_, kVideoB, now - 1000, "0000000000000002", "BBBB");
    yt::cache::SourceCache cache(output_);
    yt::cache::Limits limits;
    limits.ttl_sec = 3600;
    limits.max_bytes = 4;
    const auto stats = cache.maintain(limits);
    EXPECT_EQ(stats.retained_bytes, 4u);
    EXPECT_TRUE(generations(output_, kVideoA).empty());
    EXPECT_EQ(generations(output_, kVideoB).size(), 1u);
}

TEST_F(SourceCacheTest, OversizedSourceIsOnlyATransientInput) {
    setenv("YTCONV_GATE_FORMAT", "none", 1);
    setenv("YTCONV_FAKE_CONTENT", "LARGER-THAN-CAP", 1);
    auto req = request(kVideoA, "mp3");
    req.config.source_cache_max_bytes = 2;
    const auto result = yt::converter::processVideo(req);
    EXPECT_EQ(readFile(result.output_path), "OUT:LARGER-THAN-CAP");
    EXPECT_TRUE(generations(output_, kVideoA).empty());
}

TEST_F(SourceCacheTest, FailedRefreshKeepsPreviousGenerationAndOutput) {
    setenv("YTCONV_GATE_FORMAT", "none", 1);
    setenv("YTCONV_FAKE_CONTENT", "GOOD", 1);
    const auto first = yt::converter::processVideo(request(kVideoA, "mp3"));
    ASSERT_EQ(readFile(first.output_path), "OUT:GOOD");

    auto refresh = request(kVideoA, "mp3", fakePath("fail"));
    refresh.refresh = true;
    refresh.config.force = true;
    EXPECT_THROW(yt::converter::processVideo(refresh), yt::Error);
    EXPECT_EQ(readFile(first.output_path), "OUT:GOOD");
    EXPECT_TRUE(entries(output_, kVideoA, "inc-").empty());
    EXPECT_EQ(countPartialFiles(output_), 0);

    yt::cache::SourceCache cache(output_);
    yt::cache::Limits limits;
    limits.ttl_sec = 3600;
    auto lease = cache.acquire(kVideoA, "audio", limits);
    ASSERT_TRUE(lease.has_value()) << "previous generation was lost by a failed refresh";
    EXPECT_EQ(readFile(lease->source()), "GOOD");
}

TEST_F(SourceCacheTest, NewExtensionGenerationRetiresTheOldOne) {
    setenv("YTCONV_GATE_FORMAT", "none", 1);
    setenv("YTCONV_FAKE_EXT", "m4a", 1);
    yt::converter::processVideo(request(kVideoA, "mp3"));
    setenv("YTCONV_FAKE_EXT", "webm", 1);
    auto refresh = request(kVideoA, "wav");
    refresh.refresh = true;
    const auto refreshed = yt::converter::processVideo(refresh);
    const auto remaining = generations(output_, kVideoA);
    ASSERT_EQ(remaining.size(), 1u);
    EXPECT_EQ(remaining.front().filename().string(), refreshed.source_generation);
    EXPECT_TRUE(fs::exists(remaining.front() / "source.webm"));
}

TEST_F(SourceCacheTest, FailedEncodeReleasesItsLease) {
    setenv("YTCONV_GATE_FORMAT", "none", 1);
    auto req = request(kVideoA, "mp3");
    req.config.ffmpeg_path = fakePath("fail");
    req.config.source_cache_max_bytes = 1;
    EXPECT_THROW(yt::converter::processVideo(req), yt::Error);
    // The lease was dropped on the exception path, so maintenance could reclaim the source.
    EXPECT_TRUE(generations(output_, kVideoA).empty());
    EXPECT_EQ(countPartialFiles(output_), 0);
}

TEST_F(SourceCacheTest, AbandonedIncomingAndLegacyLayoutAreCleaned) {
    const fs::path kindDir = output_ / "cache" / "src" / kVideoA / "audio";
    // An incoming directory whose owner died: its lock file exists but nobody holds it.
    const fs::path dead = kindDir / "inc-00000000000000000001-00000000000000aa";
    fs::create_directories(dead);
    writeFile(dead / ".lease", "");
    writeFile(dead / "source.m4a.part", "partial");
    // Pre-generation layout from older builds.
    writeFile(kindDir / "source.mp4", "legacy");
    fs::create_directories(kindDir / "incoming");

    yt::cache::SourceCache cache(output_);
    yt::cache::Limits limits;
    limits.ttl_sec = 3600;
    // A live download keeps its incoming directory.
    auto live = cache.beginIncoming(kVideoA, "audio");
    cache.maintain(limits);
    EXPECT_FALSE(fs::exists(dead));
    EXPECT_FALSE(fs::exists(kindDir / "source.mp4"));
    EXPECT_FALSE(fs::exists(kindDir / "incoming"));
    EXPECT_TRUE(fs::exists(live.dir()));
}

#ifndef _WIN32
TEST_F(SourceCacheTest, SeparateCliProcessesHonorEachOthersLeases) {
    const std::string cli = YTCONV_CLI_PATH;
    ASSERT_FALSE(cli.empty());
    setenv("YTCONV_YT_DLP", fakePath("yt-dlp").c_str(), 1);
    setenv("YTCONV_FFMPEG", fakePath("ffmpeg-copy").c_str(), 1);
    setenv("YTCONV_SOURCE_CACHE_MAX_BYTES", "4", 1);
    setenv("YTCONV_LOG_LEVEL", "ERROR", 1);
    setenv("YTCONV_FAKE_CONTENT", "PROCESS-A", 1);
    struct EnvGuard {
        ~EnvGuard() {
            for (const char* name : {"YTCONV_YT_DLP", "YTCONV_FFMPEG",
                                     "YTCONV_SOURCE_CACHE_MAX_BYTES", "YTCONV_LOG_LEVEL"}) {
                unsetenv(name);
            }
        }
    } restore;

    yt::process::RunResult processA;
    GatedWorker first(gate_, [&] {
        yt::process::RunOptions options;
        options.timeout_ms = 60000;
        processA = yt::process::run({cli, "--output-dir", output_.string(),
                                     "https://www.youtube.com/watch?v=dQw4w9WgXcQ", "mp3"},
                                    options);
    });
    ASSERT_TRUE(waitForStarted(gate_));

    setenv("YTCONV_FAKE_CONTENT", "PROCESS-B", 1);
    yt::process::RunOptions options;
    options.timeout_ms = 60000;
    const auto processB = yt::process::run({cli, "--output-dir", output_.string(),
                                            "https://www.youtube.com/watch?v=jNQXAC9IVRw", "wav"},
                                           options);
    EXPECT_EQ(processB.exit_code, 0) << processB.stderr_text;
    EXPECT_EQ(generations(output_, kVideoA).size(), 1u)
        << "another process evicted a leased generation";

    first.join();
    EXPECT_EQ(processA.exit_code, 0) << processA.stderr_text;
    EXPECT_EQ(readFile(output_ / "dQw4w9WgXcQ.mp3"), "OUT:PROCESS-A");
    EXPECT_EQ(readFile(output_ / "jNQXAC9IVRw.wav"), "OUT:PROCESS-B");
}
#endif
