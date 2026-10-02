// Job/operation separation, subscriber-aware cancellation (finding 5), and bounded subscriptions
// and history (finding 6). Barriers come from gated fakes; no test relies on sleep timing.
#include "config.h"
#include "converter.h"
#include "error.h"
#include "job_queue.h"
#include "process.h"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <signal.h>
#endif

namespace fs = std::filesystem;
using yt::jobs::JobState;
using yt::jobs::SubmitKind;

namespace {

constexpr const char* kVideoA = "dQw4w9WgXcQ";
constexpr const char* kVideoB = "jNQXAC9IVRw";

std::string videoId(int n) {
    char id[16];
    std::snprintf(id, sizeof(id), "vid%08d", n);
    return id;
}

class StopQueue {
  public:
    explicit StopQueue(yt::jobs::Queue& queue) : queue_(queue) {}
    ~StopQueue() { queue_.stop(); }
    StopQueue(const StopQueue&) = delete;
    StopQueue& operator=(const StopQueue&) = delete;

  private:
    yt::jobs::Queue& queue_;
};

// Counts terminal callbacks per job to prove each fires exactly once.
struct Callbacks {
    std::mutex mutex;
    std::map<std::string, int> calls;
    std::map<std::string, JobState> states;

    yt::jobs::TerminalCallback make() {
        return [this](const yt::jobs::JobSnapshot& snapshot) {
            std::lock_guard<std::mutex> lock(mutex);
            ++calls[snapshot.job_id];
            states[snapshot.job_id] = snapshot.state;
        };
    }
    int count(const std::string& id) {
        std::lock_guard<std::mutex> lock(mutex);
        return calls[id];
    }
};

bool processGone(long pid) {
#ifndef _WIN32
    return ::kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH;
#else
    (void)pid;
    return true;
#endif
}

} // namespace

class JobQueueTest : public ::testing::Test {
  protected:
    void SetUp() override {
        yt::process::resetShutdownForTests();
        output_ = makeTestDir();
        downloadGate_ = makeTestDir();
        encodeGate_ = makeTestDir();
        setenv("YTCONV_FAKE_LOG_DIR", output_.string().c_str(), 1);
    }

    void TearDown() override {
        openGate(downloadGate_);
        openGate(encodeGate_);
        for (const char* name : {"YTCONV_FAKE_LOG_DIR", "YTCONV_YTDLP_GATE_DIR", "YTCONV_GATE_DIR",
                                 "YTCONV_GATE_FORMAT", "YTCONV_FAKE_SLEEP"}) {
            unsetenv(name);
        }
    }

    void gateDownloads() { setenv("YTCONV_YTDLP_GATE_DIR", downloadGate_.string().c_str(), 1); }
    void gateEncodes(const char* format) {
        setenv("YTCONV_GATE_DIR", encodeGate_.string().c_str(), 1);
        setenv("YTCONV_GATE_FORMAT", format, 1);
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
        req.config.log_level = "ERROR";
        return req;
    }

    bool waitState(yt::jobs::Queue& queue, const std::string& id, JobState state) {
        return waitUntil([&] {
            const auto snapshot = queue.find(id);
            return snapshot && snapshot->state == state;
        });
    }

    bool waitTerminal(yt::jobs::Queue& queue, const std::string& id) {
        return waitUntil([&] {
            const auto snapshot = queue.find(id);
            return snapshot &&
                   (snapshot->state == JobState::Succeeded || snapshot->state == JobState::Failed ||
                    snapshot->state == JobState::Canceled);
        });
    }

    int lines(const char* name) { return static_cast<int>(readLines(output_ / name).size()); }

    fs::path output_;
    fs::path downloadGate_;
    fs::path encodeGate_;
};

// ---------------------------------------------------------------------------------------------
// Finding 5: cancellation is per job; shared work survives while anyone still needs it.

TEST_F(JobQueueTest, CancelingSameFormatFollowerLeavesOriginalRunning) {
    gateEncodes("mp3");
    yt::jobs::Queue queue;
    StopQueue stop(queue);
    queue.start(1, 4);
    ASSERT_EQ(queue.submit(request(kVideoA, "mp3"), kVideoA, "original").kind, SubmitKind::Queued);
    ASSERT_TRUE(waitUntil([&] { return !gateStartedPids(encodeGate_).empty(); }));
    ASSERT_EQ(queue.submit(request(kVideoA, "mp3"), kVideoA, "follower").kind,
              SubmitKind::Attached);

    EXPECT_EQ(queue.cancel("follower"), yt::jobs::CancelResult::Canceled);
    EXPECT_EQ(queue.find("follower")->state, JobState::Canceled);
    EXPECT_EQ(queue.find("original")->state, JobState::Running);

    openGate(encodeGate_);
    ASSERT_TRUE(waitTerminal(queue, "original"));
    EXPECT_EQ(queue.find("original")->state, JobState::Succeeded);
    EXPECT_EQ(queue.find("follower")->state, JobState::Canceled);
    EXPECT_EQ(lines("ffmpeg.count"), 1);
}

TEST_F(JobQueueTest, CancelingOriginalClientLeavesFollowerToFinish) {
    gateEncodes("mp3");
    yt::jobs::Queue queue;
    StopQueue stop(queue);
    queue.start(1, 4);
    ASSERT_EQ(queue.submit(request(kVideoA, "mp3"), kVideoA, "original").kind, SubmitKind::Queued);
    ASSERT_TRUE(waitUntil([&] { return !gateStartedPids(encodeGate_).empty(); }));
    ASSERT_EQ(queue.submit(request(kVideoA, "mp3"), kVideoA, "follower").kind,
              SubmitKind::Attached);

    EXPECT_EQ(queue.cancel("original"), yt::jobs::CancelResult::Canceled);
    EXPECT_EQ(queue.find("follower")->state, JobState::Running);
    openGate(encodeGate_);
    ASSERT_TRUE(waitTerminal(queue, "follower"));
    const auto follower = queue.find("follower");
    EXPECT_EQ(follower->state, JobState::Succeeded);
    EXPECT_TRUE(fs::exists(follower->output_path));
    EXPECT_EQ(queue.find("original")->state, JobState::Canceled);
    EXPECT_EQ(lines("yt-dlp.count"), 1);
    EXPECT_EQ(lines("ffmpeg.count"), 1);
}

TEST_F(JobQueueTest, CancelingMp3KeepsTheDownloadAWavJobShares) {
    gateDownloads();
    yt::jobs::Queue queue;
    StopQueue stop(queue);
    queue.start(2, 4);
    ASSERT_EQ(queue.submit(request(kVideoA, "mp3"), kVideoA, "mp3-job").kind, SubmitKind::Queued);
    ASSERT_TRUE(waitUntil([&] { return gateStartedPids(downloadGate_).size() == 1; }));
    ASSERT_EQ(queue.submit(request(kVideoA, "wav"), kVideoA, "wav-job").kind, SubmitKind::Queued);
    // Running is not enough: wait until the WAV operation has joined the shared download.
    ASSERT_TRUE(waitUntil([] { return yt::converter::sharedDownloadWaitersForTests() == 1; }));

    EXPECT_EQ(queue.cancel("mp3-job"), yt::jobs::CancelResult::Canceled);
    // The download child must keep running for the WAV subscriber.
    const long downloader = gateStartedPids(downloadGate_).front();
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // > one cancel poll interval
    EXPECT_FALSE(processGone(downloader)) << "shared download was killed by one cancellation";

    openGate(downloadGate_);
    ASSERT_TRUE(waitTerminal(queue, "wav-job"));
    EXPECT_EQ(queue.find("wav-job")->state, JobState::Succeeded);
    EXPECT_EQ(queue.find("mp3-job")->state, JobState::Canceled);
    EXPECT_EQ(lines("yt-dlp.count"), 1);
    EXPECT_FALSE(fs::exists(output_ / (std::string(kVideoA) + ".mp3")));
    EXPECT_TRUE(fs::exists(output_ / (std::string(kVideoA) + ".wav")));
}

TEST_F(JobQueueTest, CancelingAWaitingWavReturnsPromptly) {
    gateDownloads();
    yt::jobs::Queue queue;
    StopQueue stop(queue);
    queue.start(2, 4);
    ASSERT_EQ(queue.submit(request(kVideoA, "mp3"), kVideoA, "mp3-job").kind, SubmitKind::Queued);
    ASSERT_TRUE(waitUntil([&] { return gateStartedPids(downloadGate_).size() == 1; }));
    ASSERT_EQ(queue.submit(request(kVideoA, "wav"), kVideoA, "wav-job").kind, SubmitKind::Queued);
    ASSERT_TRUE(waitUntil([] { return yt::converter::sharedDownloadWaitersForTests() == 1; }));

    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(queue.cancel("wav-job"), yt::jobs::CancelResult::Canceled);
    // The WAV worker stops waiting on the download well before the download timeout.
    ASSERT_TRUE(
        waitUntil([&] { return queue.stats().running_operations == 1; }, std::chrono::seconds(5)));
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(5));

    openGate(downloadGate_);
    ASSERT_TRUE(waitTerminal(queue, "mp3-job"));
    EXPECT_EQ(queue.find("mp3-job")->state, JobState::Succeeded);
    EXPECT_EQ(queue.find("wav-job")->state, JobState::Canceled);
}

TEST_F(JobQueueTest, CancelingEverySubscriberTerminatesTheChild) {
    gateDownloads();
    yt::jobs::Queue queue;
    StopQueue stop(queue);
    queue.start(1, 4);
    ASSERT_EQ(queue.submit(request(kVideoA, "mp3"), kVideoA, "a").kind, SubmitKind::Queued);
    ASSERT_EQ(queue.submit(request(kVideoA, "mp3"), kVideoA, "b").kind, SubmitKind::Attached);
    ASSERT_TRUE(waitUntil([&] { return gateStartedPids(downloadGate_).size() == 1; }));
    const long downloader = gateStartedPids(downloadGate_).front();

    EXPECT_EQ(queue.cancel("a"), yt::jobs::CancelResult::Canceled);
    EXPECT_EQ(queue.cancel("b"), yt::jobs::CancelResult::Canceled);
    // TERM, then KILL after the 2 s grace period; the worker slot frees once the child is reaped.
    ASSERT_TRUE(
        waitUntil([&] { return queue.stats().running_operations == 0; }, std::chrono::seconds(10)));
    EXPECT_TRUE(processGone(downloader));
    EXPECT_EQ(queue.stats().active_keys, 0u);
    EXPECT_FALSE(fs::exists(output_ / (std::string(kVideoA) + ".mp3")));
}

TEST_F(JobQueueTest, CancelingAQueuedJobFreesItsOperationSlot) {
    gateDownloads();
    yt::jobs::Queue queue;
    StopQueue stop(queue);
    queue.start(1, 2);
    ASSERT_EQ(queue.submit(request(kVideoA, "mp3"), kVideoA, "running").kind, SubmitKind::Queued);
    ASSERT_TRUE(waitState(queue, "running", JobState::Running));
    ASSERT_EQ(queue.submit(request(kVideoB, "mp3"), kVideoB, "queued").kind, SubmitKind::Queued);
    EXPECT_EQ(queue.submit(request(videoId(3), "mp3"), videoId(3), "full").kind, SubmitKind::Full);

    EXPECT_EQ(queue.cancel("queued"), yt::jobs::CancelResult::Canceled);
    EXPECT_EQ(queue.stats().queued_operations, 0u);
    EXPECT_EQ(queue.submit(request(videoId(4), "mp3"), videoId(4), "fits").kind,
              SubmitKind::Queued);
}

TEST_F(JobQueueTest, CancelDuringEncodeLeavesNoOutput) {
    gateEncodes("mp3");
    yt::jobs::Queue queue;
    StopQueue stop(queue);
    queue.start(1, 4);
    ASSERT_EQ(queue.submit(request(kVideoA, "mp3"), kVideoA, "job").kind, SubmitKind::Queued);
    ASSERT_TRUE(waitUntil([&] { return gateStartedPids(encodeGate_).size() == 1; }));
    const long encoder = gateStartedPids(encodeGate_).front();
    EXPECT_EQ(queue.cancel("job"), yt::jobs::CancelResult::Canceled);
    ASSERT_TRUE(
        waitUntil([&] { return queue.stats().running_operations == 0; }, std::chrono::seconds(10)));
    EXPECT_TRUE(processGone(encoder));
    EXPECT_FALSE(fs::exists(output_ / (std::string(kVideoA) + ".mp3")));
    EXPECT_EQ(countPartialFiles(output_), 0);
}

TEST_F(JobQueueTest, RepeatedDeleteReportsAlreadyFinished) {
    gateDownloads();
    yt::jobs::Queue queue;
    StopQueue stop(queue);
    queue.start(1, 4);
    queue.submit(request(kVideoA, "mp3"), kVideoA, "job");
    EXPECT_EQ(queue.cancel("job"), yt::jobs::CancelResult::Canceled);
    EXPECT_EQ(queue.cancel("job"), yt::jobs::CancelResult::AlreadyFinished);
    EXPECT_EQ(queue.cancel("missing"), yt::jobs::CancelResult::Missing);
}

TEST_F(JobQueueTest, TerminalStatesNeverChangeUnderCompletionRaces) {
    Callbacks callbacks;
    yt::jobs::Queue queue;
    StopQueue stop(queue);
    queue.start(2, 64);
    std::vector<std::string> ids;
    for (int i = 0; i < 30; ++i) {
        const std::string id = "race-" + std::to_string(i);
        const std::string video = videoId(100 + i);
        ASSERT_NE(queue.submit(request(video, "mp3"), video, id, callbacks.make()).kind,
                  SubmitKind::Full);
        ids.push_back(id);
        if (i % 2 == 0) {
            queue.cancel(id); // races with the worker picking it up or finishing it
        }
    }
    for (const auto& id : ids) {
        ASSERT_TRUE(waitTerminal(queue, id)) << id;
    }
    for (const auto& id : ids) {
        const auto first = queue.find(id)->state;
        EXPECT_EQ(callbacks.count(id), 1) << id;
        EXPECT_EQ(callbacks.states[id], first) << id;
        if (std::stoi(id.substr(5)) % 2 == 0) {
            EXPECT_EQ(first, JobState::Canceled) << id;
        }
    }
    // Nothing flips after the fact.
    ASSERT_TRUE(waitUntil([&] { return queue.stats().running_operations == 0; }));
    for (const auto& id : ids) {
        EXPECT_EQ(queue.find(id)->state, callbacks.states[id]) << id;
    }
}

TEST_F(JobQueueTest, ShutdownCancelsEverythingAndJoinsPromptly) {
    gateDownloads();
    Callbacks callbacks;
    yt::jobs::Queue queue;
    queue.start(2, 8);
    queue.submit(request(kVideoA, "mp3"), kVideoA, "a", callbacks.make());
    queue.submit(request(kVideoA, "mp3"), kVideoA, "a2", callbacks.make());
    queue.submit(request(kVideoB, "wav"), kVideoB, "b", callbacks.make());
    queue.submit(request(videoId(7), "mp3"), videoId(7), "queued", callbacks.make());
    ASSERT_TRUE(waitUntil([&] { return gateStartedPids(downloadGate_).size() == 2; }));
    const auto children = gateStartedPids(downloadGate_);

    const auto started = std::chrono::steady_clock::now();
    queue.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(10));
    for (const char* id : {"a", "a2", "b", "queued"}) {
        EXPECT_EQ(callbacks.count(id), 1) << id;
        EXPECT_EQ(callbacks.states[id], JobState::Canceled) << id;
    }
    for (long pid : children) {
        EXPECT_TRUE(processGone(pid)) << pid;
    }
    EXPECT_EQ(queue.submit(request(kVideoA, "mp3"), kVideoA, "late").kind, SubmitKind::Stopping);
}

// ---------------------------------------------------------------------------------------------
// Finding 6: subscriptions and history are bounded independently of request count.

TEST_F(JobQueueTest, SequentialWorkloadRetainsAtMostHistoryMax) {
    yt::jobs::Limits limits;
    limits.workers = 2;
    limits.max_operations = 4;
    limits.max_active_jobs = 8;
    limits.history_max = 10;
    yt::jobs::Queue queue;
    StopQueue stop(queue);
    queue.start(limits);
    std::size_t peak = 0;
    for (int i = 0; i < 60; ++i) {
        const std::string id = "seq-" + std::to_string(i);
        const std::string video = videoId(1000 + i);
        ASSERT_EQ(queue.submit(request(video, "wav"), video, id).kind, SubmitKind::Queued);
        ASSERT_TRUE(waitTerminal(queue, id)) << id;
        peak = std::max(peak, queue.stats().job_records);
    }
    const auto stats = queue.stats();
    EXPECT_EQ(stats.live_jobs, 0u);
    EXPECT_LE(stats.terminal_jobs, 10u);
    EXPECT_LE(peak, 11u); // history_max plus the one live job
    EXPECT_FALSE(queue.find("seq-0").has_value());
    EXPECT_TRUE(queue.find("seq-59").has_value());
}

TEST_F(JobQueueTest, DuplicateStormCannotBypassSubscriptionBound) {
    gateDownloads();
    yt::jobs::Limits limits;
    limits.workers = 1;
    limits.max_operations = 1;
    limits.max_active_jobs = 5;
    yt::jobs::Queue queue;
    StopQueue stop(queue);
    queue.start(limits);
    ASSERT_EQ(queue.submit(request(kVideoA, "mp3"), kVideoA, "leader").kind, SubmitKind::Queued);
    int attached = 0;
    int busy = 0;
    for (int i = 0; i < 50; ++i) {
        const auto kind =
            queue.submit(request(kVideoA, "mp3"), kVideoA, "dup-" + std::to_string(i)).kind;
        attached += kind == SubmitKind::Attached ? 1 : 0;
        busy += kind == SubmitKind::Busy ? 1 : 0;
    }
    EXPECT_EQ(attached, 4);
    EXPECT_EQ(busy, 46);
    EXPECT_EQ(queue.stats().live_jobs, 5u);
    EXPECT_EQ(queue.stats().job_records, 5u);
    // Admission is still bounded for distinct work: the operation limit is reached.
    EXPECT_EQ(queue.submit(request(kVideoB, "mp3"), kVideoB, "other").kind, SubmitKind::Busy);
    EXPECT_EQ(queue.cancel("dup-0"), yt::jobs::CancelResult::Canceled);
    EXPECT_EQ(queue.submit(request(kVideoB, "mp3"), kVideoB, "other").kind, SubmitKind::Full);
}

TEST_F(JobQueueTest, HistoryExpiresByTtlOnAMonotonicClock) {
    auto offset = std::make_shared<std::atomic<long long>>(0);
    yt::jobs::Limits limits;
    limits.history_ttl_sec = 60;
    yt::jobs::Queue queue;
    StopQueue stop(queue);
    queue.setClockForTests([offset] {
        return std::chrono::steady_clock::now() + std::chrono::seconds(offset->load());
    });
    queue.start(limits);
    ASSERT_EQ(queue.submit(request(kVideoA, "wav"), kVideoA, "job").kind, SubmitKind::Queued);
    ASSERT_TRUE(waitTerminal(queue, "job"));
    offset->store(59);
    EXPECT_TRUE(queue.find("job").has_value());
    offset->store(61);
    EXPECT_FALSE(queue.find("job").has_value());
    EXPECT_EQ(queue.cancel("job"), yt::jobs::CancelResult::Missing);
}

TEST_F(JobQueueTest, CountLimitEvictsOldestTerminalFirst) {
    yt::jobs::Limits limits;
    limits.history_max = 2;
    yt::jobs::Queue queue;
    StopQueue stop(queue);
    queue.start(limits);
    for (int i = 0; i < 3; ++i) {
        const std::string video = videoId(200 + i);
        queue.submit(request(video, "wav"), video, "c" + std::to_string(i));
        ASSERT_TRUE(waitTerminal(queue, "c" + std::to_string(i)));
    }
    EXPECT_FALSE(queue.find("c0").has_value());
    EXPECT_TRUE(queue.find("c1").has_value());
    EXPECT_TRUE(queue.find("c2").has_value());
}

TEST_F(JobQueueTest, OperationSurvivesPruningOfItsCreatorsRecord) {
    gateEncodes("mp3");
    yt::jobs::Limits limits;
    limits.workers = 1;
    limits.history_max = 0; // terminal records are pruned immediately
    yt::jobs::Queue queue;
    StopQueue stop(queue);
    queue.start(limits);
    ASSERT_EQ(queue.submit(request(kVideoA, "mp3"), kVideoA, "creator").kind, SubmitKind::Queued);
    ASSERT_TRUE(waitUntil([&] { return !gateStartedPids(encodeGate_).empty(); }));
    ASSERT_EQ(queue.submit(request(kVideoA, "mp3"), kVideoA, "follower").kind,
              SubmitKind::Attached);
    EXPECT_EQ(queue.cancel("creator"), yt::jobs::CancelResult::Canceled);
    EXPECT_FALSE(queue.find("creator").has_value()); // pruned
    openGate(encodeGate_);
    ASSERT_TRUE(waitUntil([&] { return queue.stats().running_operations == 0; }));
    // The follower's own terminal record was pruned too, but its outcome was published before.
    EXPECT_TRUE(fs::exists(output_ / (std::string(kVideoA) + ".mp3")));
    EXPECT_EQ(queue.stats().job_records, 0u);
}

TEST_F(JobQueueTest, FailureMessagesAreBounded) {
    yt::jobs::Queue queue;
    StopQueue stop(queue);
    queue.start(1, 2);
    auto req = request(kVideoA, "mp3", fakePath("fail"));
    ASSERT_EQ(queue.submit(req, kVideoA, "job").kind, SubmitKind::Queued);
    ASSERT_TRUE(waitTerminal(queue, "job"));
    const auto snapshot = queue.find("job");
    EXPECT_EQ(snapshot->state, JobState::Failed);
    EXPECT_EQ(snapshot->code, yt::ErrorCode::DownloadFailed);
    EXPECT_LE(snapshot->message.size(), yt::jobs::kMaxMessageBytes);
}

TEST_F(JobQueueTest, DuplicateJobIdIsRejectedNotReplaced) {
    gateDownloads();
    yt::jobs::Queue queue;
    StopQueue stop(queue);
    queue.start(1, 4);
    ASSERT_EQ(queue.submit(request(kVideoA, "mp3"), kVideoA, "same").kind, SubmitKind::Queued);
    EXPECT_EQ(queue.submit(request(kVideoB, "wav"), kVideoB, "same").kind, SubmitKind::Duplicate);
    EXPECT_EQ(queue.find("same")->video_id, kVideoA);
}

TEST(JobQueueConfig, ValidatesLimits) {
    struct EnvGuard {
        ~EnvGuard() {
            for (const char* name : {"YTCONV_MAX_ACTIVE_JOBS", "YTCONV_JOB_HISTORY_MAX",
                                     "YTCONV_JOB_HISTORY_TTL_SEC", "YTCONV_QUEUE_DEPTH"}) {
                unsetenv(name);
            }
        }
    } restore;
    const auto defaults = yt::loadConfigFromEnv();
    EXPECT_EQ(defaults.max_active_jobs, 256);
    EXPECT_EQ(defaults.job_history_max, 1024);
    EXPECT_EQ(defaults.job_history_ttl_sec, 3600);

    for (const auto& [name, value] :
         std::vector<std::pair<const char*, const char*>>{{"YTCONV_MAX_ACTIVE_JOBS", "0"},
                                                          {"YTCONV_MAX_ACTIVE_JOBS", "many"},
                                                          {"YTCONV_JOB_HISTORY_MAX", "-1"},
                                                          {"YTCONV_JOB_HISTORY_TTL_SEC", "0"}}) {
        setenv(name, value, 1);
        EXPECT_THROW(yt::loadConfigFromEnv(), yt::Error) << name << "=" << value;
        unsetenv(name);
    }
    setenv("YTCONV_QUEUE_DEPTH", "16", 1);
    setenv("YTCONV_MAX_ACTIVE_JOBS", "8", 1);
    EXPECT_THROW(yt::loadConfigFromEnv(), yt::Error);
}
