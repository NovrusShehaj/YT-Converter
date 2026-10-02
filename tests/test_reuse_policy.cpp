// Completed-output reuse, force, and refresh (finding 8). Assertions check output content and
// source generations, not only child invocation counts.
#include "converter.h"
#include "error.h"
#include "job_queue.h"
#include "process.h"
#include "source_cache.h"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef YTCONV_CLI_PATH
#define YTCONV_CLI_PATH ""
#endif

namespace fs = std::filesystem;

namespace {

constexpr const char* kVideo = "dQw4w9WgXcQ";

// A per-request fake yt-dlp: exports the given environment, then runs the shared fake. This
// lets two concurrent requests download different content or wait on different gates.
std::string wrapper(const fs::path& dir, const std::string& name,
                    const std::vector<std::pair<std::string, std::string>>& env) {
    const fs::path path = dir / name;
    std::string script = "#!/usr/bin/env bash\n";
    for (const auto& [key, value] : env) {
        script += "export " + key + "='" + value + "'\n";
    }
    script += "exec '" + fakePath("yt-dlp") + "' \"$@\"\n";
    writeFile(path, script);
    fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace);
    return path.string();
}

std::vector<fs::path> generations(const fs::path& output) {
    std::vector<fs::path> found;
    std::error_code ec;
    for (const auto& entry :
         fs::directory_iterator(output / "cache" / "src" / kVideo / "audio", ec)) {
        if (entry.path().filename().string().rfind("gen-", 0) == 0) {
            found.push_back(entry.path());
        }
    }
    return found;
}

int lines(const fs::path& path) {
    return static_cast<int>(readLines(path).size());
}

enum class CacheState { Fresh, Expired, Missing };

// Runs one conversion on a thread, capturing its result or exception. The destructor opens the
// gate and joins, so a failed assertion never leaves a joinable thread behind.
class AsyncConversion {
  public:
    AsyncConversion(yt::converter::ConversionRequest request, fs::path gate)
        : gate_(std::move(gate)), thread_([this, request = std::move(request)] {
              try {
                  result_ = yt::converter::processVideo(request);
              } catch (...) {
                  error_ = std::current_exception();
              }
              done_.store(true);
          }) {}
    AsyncConversion(const AsyncConversion&) = delete;
    AsyncConversion& operator=(const AsyncConversion&) = delete;
    ~AsyncConversion() { join(); }

    bool done() const { return done_.load(); }
    void join() {
        if (thread_.joinable()) {
            openGate(gate_);
            thread_.join();
        }
    }
    const std::optional<yt::converter::ConversionResult>& result() const { return result_; }
    bool failed() const { return static_cast<bool>(error_); }

  private:
    fs::path gate_;
    std::optional<yt::converter::ConversionResult> result_;
    std::exception_ptr error_;
    std::atomic<bool> done_{false};
    std::thread thread_;
};

} // namespace

class ReusePolicyTest : public ::testing::Test {
  protected:
    void SetUp() override {
        yt::process::resetShutdownForTests();
        output_ = makeTestDir();
        scripts_ = makeTestDir();
        gate_ = makeTestDir();
        setenv("YTCONV_FAKE_LOG_DIR", output_.string().c_str(), 1);
    }

    void TearDown() override {
        openGate(gate_);
        unsetenv("YTCONV_FAKE_LOG_DIR");
        unsetenv("YTCONV_FAKE_CONTENT");
    }

    yt::converter::ConversionRequest request(const std::string& format, const std::string& content,
                                             const fs::path& out) const {
        yt::converter::ConversionRequest req;
        req.url = std::string("https://www.youtube.com/watch?v=") + kVideo;
        req.format = format;
        req.config.output_dir = out.string();
        req.config.yt_dlp_path =
            wrapper(scripts_, "ytdlp-" + content, {{"YTCONV_FAKE_CONTENT", content}});
        req.config.ffmpeg_path = fakePath("ffmpeg-copy");
        req.config.source_cache_ttl_sec = 3600;
        req.config.download_timeout_sec = 30;
        req.config.convert_timeout_sec = 30;
        return req;
    }

    fs::path output_;
    fs::path scripts_;
    fs::path gate_;
};

TEST_F(ReusePolicyTest, FullPolicyMatrix) {
    int cases = 0;
    for (const bool reuse : {true, false}) {
        for (const bool force : {false, true}) {
            for (const bool refresh : {false, true}) {
                for (const bool outputPresent : {true, false}) {
                    for (const CacheState cache :
                         {CacheState::Fresh, CacheState::Expired, CacheState::Missing}) {
                        const fs::path out = makeTestDir();
                        setenv("YTCONV_FAKE_LOG_DIR", out.string().c_str(), 1);
                        const std::string label = std::string("reuse=") + (reuse ? "1" : "0") +
                                                  " force=" + (force ? "1" : "0") +
                                                  " refresh=" + (refresh ? "1" : "0") +
                                                  " output=" + (outputPresent ? "1" : "0") +
                                                  " cache=" + std::to_string(int(cache));
                        // Prime the cache with an "OLD" source via a WAV conversion.
                        if (cache != CacheState::Missing) {
                            yt::converter::processVideo(request("wav", "OLD", out));
                            if (cache == CacheState::Expired) {
                                for (const auto& gen : [&] {
                                         std::vector<fs::path> found;
                                         for (const auto& e : fs::directory_iterator(
                                                  out / "cache" / "src" / kVideo / "audio")) {
                                             found.push_back(e.path());
                                         }
                                         return found;
                                     }()) {
                                    const std::string name = gen.filename().string();
                                    char aged[64];
                                    std::snprintf(
                                        aged, sizeof(aged), "gen-%020lld-%s",
                                        static_cast<long long>(std::stoll(name.substr(4, 20)) -
                                                               7200LL * 1000000000LL),
                                        name.substr(25).c_str());
                                    fs::rename(gen, gen.parent_path() / aged);
                                }
                            }
                        }
                        const fs::path finalPath = out / (std::string(kVideo) + ".mp3");
                        if (outputPresent) {
                            writeFile(finalPath, "PREVIOUS");
                        }
                        const int downloadsBefore = lines(out / "yt-dlp.count");
                        const int encodesBefore = lines(out / "ffmpeg.count");

                        auto req = request("mp3", "NEW", out);
                        req.config.reuse_completed = reuse;
                        req.config.force = force;
                        req.refresh = refresh;
                        const auto result = yt::converter::processVideo(req);

                        const bool expectReuse = reuse && !force && !refresh && outputPresent;
                        const bool expectDownload =
                            !expectReuse && (refresh || cache != CacheState::Fresh);
                        const std::string expectContent =
                            expectReuse ? "PREVIOUS" : (expectDownload ? "OUT:NEW" : "OUT:OLD");
                        EXPECT_EQ(readFile(finalPath), expectContent) << label;
                        EXPECT_EQ(result.reused, expectReuse) << label;
                        EXPECT_EQ(lines(out / "yt-dlp.count") - downloadsBefore,
                                  expectDownload ? 1 : 0)
                            << label;
                        EXPECT_EQ(lines(out / "ffmpeg.count") - encodesBefore, expectReuse ? 0 : 1)
                            << label;
                        EXPECT_EQ(result.downloaded, expectDownload) << label;
                        EXPECT_EQ(result.source_cache_hit, !expectReuse && !expectDownload)
                            << label;
                        ++cases;
                    }
                }
            }
        }
    }
    EXPECT_EQ(cases, 48);
    setenv("YTCONV_FAKE_LOG_DIR", output_.string().c_str(), 1);
}

TEST_F(ReusePolicyTest, SameFormatRefreshReplacesContentAndGeneration) {
    const auto first = yt::converter::processVideo(request("mp3", "ONE", output_));
    ASSERT_EQ(readFile(first.output_path), "OUT:ONE");
    auto refresh = request("mp3", "TWO", output_);
    refresh.refresh = true;
    const auto second = yt::converter::processVideo(refresh);
    EXPECT_FALSE(second.reused);
    EXPECT_TRUE(second.downloaded);
    EXPECT_EQ(readFile(second.output_path), "OUT:TWO");
    EXPECT_NE(second.source_generation, first.source_generation);
    const auto remaining = generations(output_);
    ASSERT_EQ(remaining.size(), 1u);
    EXPECT_EQ(remaining.front().filename().string(), second.source_generation);
}

TEST_F(ReusePolicyTest, FailedRefreshPreservesExistingOutput) {
    yt::converter::processVideo(request("mp3", "KEEP", output_));
    auto refresh = request("mp3", "IGNORED", output_);
    refresh.config.yt_dlp_path = fakePath("fail");
    refresh.refresh = true;
    EXPECT_THROW(yt::converter::processVideo(refresh), yt::Error);
    EXPECT_EQ(readFile(output_ / (std::string(kVideo) + ".mp3")), "OUT:KEEP");
}

TEST_F(ReusePolicyTest, OlderReplacementCannotOverwriteALaterRefresh) {
    // F (force, admitted first) downloads behind a gate. R (refresh, admitted later) publishes
    // first. F must not overwrite R's output when it finishes.
    auto older = request("mp3", "F-CONTENT", output_);
    older.config.force = true;
    older.config.yt_dlp_path =
        wrapper(scripts_, "ytdlp-gated",
                {{"YTCONV_FAKE_CONTENT", "F-CONTENT"}, {"YTCONV_YTDLP_GATE_DIR", gate_.string()}});
    AsyncConversion slow(older, gate_);
    ASSERT_TRUE(waitUntil([&] { return !gateStartedPids(gate_).empty(); }));

    auto later = request("mp3", "R-CONTENT", output_);
    later.refresh = true;
    const auto laterResult = yt::converter::processVideo(later);
    ASSERT_EQ(readFile(laterResult.output_path), "OUT:R-CONTENT");

    slow.join();
    ASSERT_TRUE(slow.result().has_value());
    EXPECT_TRUE(slow.result()->superseded);
    EXPECT_FALSE(slow.result()->published);
    EXPECT_EQ(readFile(laterResult.output_path), "OUT:R-CONTENT");
}

TEST_F(ReusePolicyTest, OlderRefreshCannotOverwriteALaterRefresh) {
    // Two refreshes: the first one's download started before the second was admitted, so they
    // do not share. The second publishes first; the first carries an older generation and must
    // not replace it.
    auto first = request("mp3", "REFRESH-1", output_);
    first.refresh = true;
    first.config.yt_dlp_path =
        wrapper(scripts_, "ytdlp-refresh1-gated",
                {{"YTCONV_FAKE_CONTENT", "REFRESH-1"}, {"YTCONV_YTDLP_GATE_DIR", gate_.string()}});
    AsyncConversion slow(first, gate_);
    ASSERT_TRUE(waitUntil([&] { return !gateStartedPids(gate_).empty(); }));

    auto second = request("mp3", "REFRESH-2", output_);
    second.refresh = true;
    const auto secondResult = yt::converter::processVideo(second);
    ASSERT_EQ(readFile(secondResult.output_path), "OUT:REFRESH-2");

    slow.join();
    ASSERT_TRUE(slow.result().has_value());
    EXPECT_TRUE(slow.result()->superseded);
    EXPECT_EQ(readFile(secondResult.output_path), "OUT:REFRESH-2");
}

TEST_F(ReusePolicyTest, ReplacementIsSatisfiedByAPublicationAfterItsAdmission) {
    auto replace = request("mp3", "UNUSED", output_);
    replace.config.force = true;
    replace.admitted_at = yt::cache::nowStamp();
    // Another writer publishes after the replacement was admitted.
    yt::converter::processVideo(request("mp3", "FRESH", output_));
    const int encodes = lines(output_ / "ffmpeg.count");
    const auto result = yt::converter::processVideo(replace);
    EXPECT_TRUE(result.superseded);
    EXPECT_EQ(lines(output_ / "ffmpeg.count"), encodes);
    EXPECT_EQ(readFile(result.output_path), "OUT:FRESH");
}

TEST_F(ReusePolicyTest, RefreshNeverAttachesToAnOlderInFlightDownload) {
    auto ordinary = request("mp3", "ORDINARY", output_);
    ordinary.config.yt_dlp_path =
        wrapper(scripts_, "ytdlp-ordinary-gated",
                {{"YTCONV_FAKE_CONTENT", "ORDINARY"}, {"YTCONV_YTDLP_GATE_DIR", gate_.string()}});
    AsyncConversion gated(ordinary, gate_);
    ASSERT_TRUE(waitUntil([&] { return !gateStartedPids(gate_).empty(); }));

    // Admitted after the ordinary download started: it must finish on its own download while
    // the older one is still parked.
    auto refresh = request("wav", "REFRESHED", output_);
    refresh.refresh = true;
    AsyncConversion refreshed(refresh, scripts_ / "no-gate");
    ASSERT_TRUE(waitUntil([&] { return refreshed.done(); }, std::chrono::seconds(10)))
        << "refresh attached to the older in-flight download";
    ASSERT_TRUE(refreshed.result().has_value());
    EXPECT_TRUE(refreshed.result()->downloaded);
    EXPECT_FALSE(refreshed.result()->shared_download);
    EXPECT_EQ(readFile(refreshed.result()->output_path), "OUT:REFRESHED");
    gated.join();
    EXPECT_EQ(lines(output_ / "yt-dlp.count"), 2);
}

TEST_F(ReusePolicyTest, RefreshSharesADownloadThatStartedAfterItsAdmission) {
    const std::int64_t admitted = yt::cache::nowStamp();
    auto first = request("mp3", "SHARED", output_);
    first.refresh = true;
    first.config.yt_dlp_path =
        wrapper(scripts_, "ytdlp-shared-gated",
                {{"YTCONV_FAKE_CONTENT", "SHARED"}, {"YTCONV_YTDLP_GATE_DIR", gate_.string()}});
    AsyncConversion leader(first, gate_);
    ASSERT_TRUE(waitUntil([&] { return !gateStartedPids(gate_).empty(); }));

    auto second = request("wav", "NOT-USED", output_);
    second.refresh = true;
    second.admitted_at = admitted; // admitted before the first download started
    AsyncConversion follower(second, gate_);
    // While the download is parked the follower cannot finish unless it started its own.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_FALSE(follower.done());
    follower.join();
    leader.join();
    ASSERT_TRUE(follower.result().has_value() && leader.result().has_value());
    EXPECT_TRUE(follower.result()->shared_download);
    EXPECT_EQ(readFile(follower.result()->output_path), "OUT:SHARED");
    EXPECT_EQ(lines(output_ / "yt-dlp.count"), 1);
}

TEST_F(ReusePolicyTest, QueueCoalescesOnlyCompatiblePolicies) {
    const fs::path blocker = makeTestDir();
    yt::jobs::Queue queue;
    struct Stop {
        yt::jobs::Queue& queue;
        fs::path gate;
        ~Stop() {
            openGate(gate);
            queue.stop();
        }
    } stop{queue, gate_};
    queue.start(1, 8);
    // Occupy the only worker so later submissions stay queued.
    auto busy = request("mp3", "BUSY", output_);
    busy.url = "https://www.youtube.com/watch?v=jNQXAC9IVRw";
    busy.config.yt_dlp_path =
        wrapper(scripts_, "ytdlp-busy", {{"YTCONV_YTDLP_GATE_DIR", gate_.string()}});
    ASSERT_EQ(queue.submit(busy, "jNQXAC9IVRw", "busy").kind, yt::jobs::SubmitKind::Queued);
    ASSERT_TRUE(waitUntil([&] { return !gateStartedPids(gate_).empty(); }));

    auto ordinary = request("mp3", "X", output_);
    auto force = ordinary;
    force.config.force = true;
    auto refresh = ordinary;
    refresh.refresh = true;
    EXPECT_EQ(queue.submit(ordinary, kVideo, "o1").kind, yt::jobs::SubmitKind::Queued);
    EXPECT_EQ(queue.submit(ordinary, kVideo, "o2").kind, yt::jobs::SubmitKind::Attached);
    EXPECT_EQ(queue.submit(force, kVideo, "f1").kind, yt::jobs::SubmitKind::Queued);
    EXPECT_EQ(queue.submit(force, kVideo, "f2").kind, yt::jobs::SubmitKind::Attached);
    EXPECT_EQ(queue.submit(refresh, kVideo, "r1").kind, yt::jobs::SubmitKind::Queued);
    // A refresh shares a refresh that has not started yet.
    EXPECT_EQ(queue.submit(refresh, kVideo, "r2").kind, yt::jobs::SubmitKind::Attached);
    EXPECT_EQ(queue.stats().queued_operations, 3u);
}

TEST_F(ReusePolicyTest, QueueRefreshDoesNotAttachToAStartedRefresh) {
    yt::jobs::Queue queue;
    struct Stop {
        yt::jobs::Queue& queue;
        fs::path gate;
        ~Stop() {
            openGate(gate);
            queue.stop();
        }
    } stop{queue, gate_};
    queue.start(2, 8);
    auto refresh = request("mp3", "R", output_);
    refresh.refresh = true;
    refresh.config.yt_dlp_path =
        wrapper(scripts_, "ytdlp-refresh-gated", {{"YTCONV_YTDLP_GATE_DIR", gate_.string()}});
    ASSERT_EQ(queue.submit(refresh, kVideo, "r1").kind, yt::jobs::SubmitKind::Queued);
    ASSERT_TRUE(waitUntil([&] { return !gateStartedPids(gate_).empty(); }));
    EXPECT_EQ(queue.submit(refresh, kVideo, "r2").kind, yt::jobs::SubmitKind::Queued);
}

#ifndef _WIN32
TEST_F(ReusePolicyTest, CliRefreshAndForceAreIndependent) {
    const std::string cli = YTCONV_CLI_PATH;
    ASSERT_FALSE(cli.empty());
    setenv("YTCONV_FFMPEG", fakePath("ffmpeg-copy").c_str(), 1);
    setenv("YTCONV_LOG_LEVEL", "ERROR", 1);
    struct EnvGuard {
        ~EnvGuard() {
            unsetenv("YTCONV_FFMPEG");
            unsetenv("YTCONV_LOG_LEVEL");
            unsetenv("YTCONV_YT_DLP");
        }
    } restore;
    auto runCli = [&](const std::string& content, const std::vector<std::string>& flags) {
        setenv("YTCONV_YT_DLP",
               wrapper(scripts_, "cli-" + content, {{"YTCONV_FAKE_CONTENT", content}}).c_str(), 1);
        std::vector<std::string> argv{cli, "--output-dir", output_.string()};
        argv.insert(argv.end(), flags.begin(), flags.end());
        argv.push_back(std::string("https://www.youtube.com/watch?v=") + kVideo);
        argv.push_back("mp3");
        yt::process::RunOptions options;
        options.timeout_ms = 60000;
        return yt::process::run(argv, options);
    };
    const fs::path finalPath = output_ / (std::string(kVideo) + ".mp3");
    ASSERT_EQ(runCli("FIRST", {}).exit_code, 0);
    EXPECT_EQ(readFile(finalPath), "OUT:FIRST");
    ASSERT_EQ(runCli("SECOND", {}).exit_code, 0);
    EXPECT_EQ(readFile(finalPath), "OUT:FIRST"); // completed output reused
    ASSERT_EQ(runCli("THIRD", {"--force"}).exit_code, 0);
    EXPECT_EQ(readFile(finalPath), "OUT:FIRST"); // re-encoded from the cached FIRST source
    EXPECT_EQ(lines(output_ / "yt-dlp.count"), 1);
    EXPECT_EQ(lines(output_ / "ffmpeg.count"), 2);
    ASSERT_EQ(runCli("FOURTH", {"--refresh"}).exit_code, 0);
    EXPECT_EQ(readFile(finalPath), "OUT:FOURTH"); // new source generation
    EXPECT_EQ(lines(output_ / "yt-dlp.count"), 2);
}
#endif
