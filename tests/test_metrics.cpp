// Physical work is counted once per operation; logical outcomes once per job (finding 11).
// Each scenario takes counter snapshots before and after and asserts exact deltas.
#include "converter.h"
#include "error.h"
#include "job_queue.h"
#include "metrics.h"
#include "process.h"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>

namespace fs = std::filesystem;
using yt::jobs::JobState;
using yt::jobs::SubmitKind;

namespace {

constexpr const char* kVideo = "dQw4w9WgXcQ";

struct Snapshot {
    std::map<std::string, std::uint64_t> values;

    static Snapshot take() {
        const auto& c = yt::metrics::global();
        Snapshot s;
        s.values = {
            {"jobs_started", c.jobs_started.load()},
            {"jobs_succeeded", c.jobs_succeeded.load()},
            {"jobs_failed", c.jobs_failed.load()},
            {"jobs_canceled", c.jobs_canceled.load()},
            {"jobs_reused", c.jobs_reused.load()},
            {"jobs_coalesced", c.jobs_coalesced.load()},
            {"downloads_total", c.downloads_total.load()},
            {"download_failures_total", c.download_failures_total.load()},
            {"source_bytes_downloaded", c.source_bytes_downloaded.load()},
            {"source_cache_hits_total", c.source_cache_hits_total.load()},
            {"shared_downloads_total", c.shared_downloads_total.load()},
            {"encodes_total", c.encodes_total.load()},
            {"encode_failures_total", c.encode_failures_total.load()},
            {"outputs_published_total", c.outputs_published_total.load()},
            {"bytes_written", c.bytes_written.load()},
        };
        return s;
    }

    std::map<std::string, std::uint64_t> since(const Snapshot& before) const {
        std::map<std::string, std::uint64_t> delta;
        for (const auto& [key, value] : values) {
            delta[key] = value - before.values.at(key);
        }
        return delta;
    }
};

// Expected deltas; unspecified counters must not move.
void expectDeltas(const std::map<std::string, std::uint64_t>& actual,
                  const std::map<std::string, std::uint64_t>& expected, const char* scenario) {
    for (const auto& [key, value] : actual) {
        const auto it = expected.find(key);
        EXPECT_EQ(value, it == expected.end() ? 0u : it->second) << scenario << ": " << key;
    }
}

void expectQuiescentGauges() {
    const auto& c = yt::metrics::global();
    EXPECT_EQ(c.jobs_queued.load(), 0u);
    EXPECT_EQ(c.jobs_running.load(), 0u);
    EXPECT_EQ(c.operations_queued.load(), 0u);
    EXPECT_EQ(c.operations_running.load(), 0u);
}

} // namespace

class MetricsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        yt::process::resetShutdownForTests();
        output_ = makeTestDir();
        gate_ = makeTestDir();
        setenv("YTCONV_FAKE_LOG_DIR", output_.string().c_str(), 1);
        setenv("YTCONV_FAKE_CONTENT", "SOURCE", 1); // 6 source bytes; ffmpeg-copy adds "OUT:"
    }

    void TearDown() override {
        openGate(gate_);
        unsetenv("YTCONV_FAKE_LOG_DIR");
        unsetenv("YTCONV_FAKE_CONTENT");
        unsetenv("YTCONV_YTDLP_GATE_DIR");
    }

    yt::converter::ConversionRequest request(const std::string& format) const {
        yt::converter::ConversionRequest req;
        req.url = std::string("https://www.youtube.com/watch?v=") + kVideo;
        req.format = format;
        req.config.output_dir = output_.string();
        req.config.yt_dlp_path = fakePath("yt-dlp");
        req.config.ffmpeg_path = fakePath("ffmpeg-copy");
        req.config.download_timeout_sec = 30;
        req.config.convert_timeout_sec = 30;
        return req;
    }

    bool waitTerminal(yt::jobs::Queue& queue, const std::string& id) {
        return waitUntil([&] {
            const auto s = queue.find(id);
            return s && (s->state == JobState::Succeeded || s->state == JobState::Failed ||
                         s->state == JobState::Canceled);
        });
    }

    fs::path output_;
    fs::path gate_;
};

TEST_F(MetricsTest, ColdMp3WarmWavAndCompletedReuse) {
    yt::jobs::Queue queue;
    queue.start(1, 4);
    auto before = Snapshot::take();
    queue.submit(request("mp3"), kVideo, "cold");
    ASSERT_TRUE(waitTerminal(queue, "cold"));
    expectDeltas(Snapshot::take().since(before),
                 {{"jobs_started", 1},
                  {"jobs_succeeded", 1},
                  {"downloads_total", 1},
                  {"source_bytes_downloaded", 6},
                  {"encodes_total", 1},
                  {"outputs_published_total", 1},
                  {"bytes_written", 10}},
                 "cold mp3");

    before = Snapshot::take();
    queue.submit(request("wav"), kVideo, "warm");
    ASSERT_TRUE(waitTerminal(queue, "warm"));
    expectDeltas(Snapshot::take().since(before),
                 {{"jobs_started", 1},
                  {"jobs_succeeded", 1},
                  {"source_cache_hits_total", 1},
                  {"encodes_total", 1},
                  {"outputs_published_total", 1},
                  {"bytes_written", 10}},
                 "warm wav");

    // Completed-output reuse: one logical success, no physical work, no bytes written.
    before = Snapshot::take();
    queue.submit(request("mp3"), kVideo, "reuse");
    ASSERT_TRUE(waitTerminal(queue, "reuse"));
    expectDeltas(Snapshot::take().since(before),
                 {{"jobs_started", 1}, {"jobs_succeeded", 1}, {"jobs_reused", 1}},
                 "completed reuse");
    EXPECT_TRUE(queue.find("reuse")->reused);
    queue.stop();
    expectQuiescentGauges();
}

TEST_F(MetricsTest, SameFormatSubscribersShareOneMeasurement) {
    setenv("YTCONV_YTDLP_GATE_DIR", gate_.string().c_str(), 1);
    yt::jobs::Queue queue;
    queue.start(1, 4);
    const auto before = Snapshot::take();
    ASSERT_EQ(queue.submit(request("mp3"), kVideo, "a").kind, SubmitKind::Queued);
    ASSERT_EQ(queue.submit(request("mp3"), kVideo, "b").kind, SubmitKind::Attached);
    openGate(gate_);
    ASSERT_TRUE(waitTerminal(queue, "a"));
    ASSERT_TRUE(waitTerminal(queue, "b"));
    expectDeltas(Snapshot::take().since(before),
                 {{"jobs_started", 2},
                  {"jobs_coalesced", 1},
                  {"jobs_succeeded", 2},
                  {"downloads_total", 1},
                  {"source_bytes_downloaded", 6},
                  {"encodes_total", 1},
                  {"outputs_published_total", 1},
                  {"bytes_written", 10}},
                 "same-format sharing");
    // Each job may report the shared operation's duration, but totals counted it once.
    EXPECT_EQ(queue.find("a")->download_ms, queue.find("b")->download_ms);
    queue.stop();
    expectQuiescentGauges();
}

TEST_F(MetricsTest, Mp3AndWavShareOneDownloadMeasurement) {
    setenv("YTCONV_YTDLP_GATE_DIR", gate_.string().c_str(), 1);
    yt::jobs::Queue queue;
    queue.start(2, 4);
    const auto before = Snapshot::take();
    queue.submit(request("mp3"), kVideo, "mp3");
    ASSERT_TRUE(waitUntil([&] { return !gateStartedPids(gate_).empty(); }));
    queue.submit(request("wav"), kVideo, "wav");
    // The WAV operation must have joined the download before it is released.
    ASSERT_TRUE(waitUntil([] { return yt::converter::sharedDownloadWaitersForTests() == 1; }));
    openGate(gate_);
    ASSERT_TRUE(waitTerminal(queue, "mp3"));
    ASSERT_TRUE(waitTerminal(queue, "wav"));
    expectDeltas(Snapshot::take().since(before),
                 {{"jobs_started", 2},
                  {"jobs_succeeded", 2},
                  {"downloads_total", 1},
                  {"source_bytes_downloaded", 6},
                  {"shared_downloads_total", 1},
                  {"encodes_total", 2},
                  {"outputs_published_total", 2},
                  {"bytes_written", 20}},
                 "mp3/wav sharing");
    queue.stop();
    expectQuiescentGauges();
}

TEST_F(MetricsTest, FailedEncodeKeepsItsDownloadMeasurement) {
    yt::jobs::Queue queue;
    queue.start(1, 4);
    auto req = request("mp3");
    req.config.ffmpeg_path = fakePath("fail");
    const auto before = Snapshot::take();
    queue.submit(req, kVideo, "job");
    ASSERT_TRUE(waitTerminal(queue, "job"));
    expectDeltas(Snapshot::take().since(before),
                 {{"jobs_started", 1},
                  {"jobs_failed", 1},
                  {"downloads_total", 1},
                  {"source_bytes_downloaded", 6},
                  {"encode_failures_total", 1}},
                 "failed encode");
    queue.stop();
    expectQuiescentGauges();
}

TEST_F(MetricsTest, CancellationCountsEachJobOnce) {
    setenv("YTCONV_YTDLP_GATE_DIR", gate_.string().c_str(), 1);
    yt::jobs::Queue queue;
    queue.start(1, 4);
    // One of two subscribers canceled: the other succeeds on the same single download.
    auto before = Snapshot::take();
    queue.submit(request("mp3"), kVideo, "keep");
    queue.submit(request("mp3"), kVideo, "drop");
    ASSERT_TRUE(waitUntil([&] { return !gateStartedPids(gate_).empty(); }));
    queue.cancel("drop");
    openGate(gate_);
    ASSERT_TRUE(waitTerminal(queue, "keep"));
    expectDeltas(Snapshot::take().since(before),
                 {{"jobs_started", 2},
                  {"jobs_coalesced", 1},
                  {"jobs_succeeded", 1},
                  {"jobs_canceled", 1},
                  {"downloads_total", 1},
                  {"source_bytes_downloaded", 6},
                  {"encodes_total", 1},
                  {"outputs_published_total", 1},
                  {"bytes_written", 10}},
                 "cancel one subscriber");

    // Every subscriber canceled: the download is killed and counted as one failed attempt.
    const fs::path secondGate = makeTestDir();
    setenv("YTCONV_YTDLP_GATE_DIR", secondGate.string().c_str(), 1);
    auto other = request("wav");
    other.url = "https://www.youtube.com/watch?v=jNQXAC9IVRw";
    before = Snapshot::take();
    queue.submit(other, "jNQXAC9IVRw", "x");
    queue.submit(other, "jNQXAC9IVRw", "y");
    ASSERT_TRUE(waitUntil([&] { return !gateStartedPids(secondGate).empty(); }));
    queue.cancel("x");
    queue.cancel("y");
    ASSERT_TRUE(
        waitUntil([&] { return queue.stats().running_operations == 0; }, std::chrono::seconds(10)));
    openGate(secondGate);
    expectDeltas(Snapshot::take().since(before),
                 {{"jobs_started", 2},
                  {"jobs_coalesced", 1},
                  {"jobs_canceled", 2},
                  {"download_failures_total", 1}},
                 "cancel every subscriber");
    queue.stop();
    expectQuiescentGauges();
}

TEST_F(MetricsTest, RefreshDownloadsAgainEvenWithAFreshCache) {
    yt::converter::processVideo(request("mp3"));
    auto refresh = request("mp3");
    refresh.refresh = true;
    const auto before = Snapshot::take();
    yt::converter::processVideo(refresh);
    // Direct converter calls record physical work only; logical jobs belong to the caller.
    expectDeltas(Snapshot::take().since(before),
                 {{"downloads_total", 1},
                  {"source_bytes_downloaded", 6},
                  {"encodes_total", 1},
                  {"outputs_published_total", 1},
                  {"bytes_written", 10}},
                 "refresh");
}
