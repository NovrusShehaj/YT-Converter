#include "converter.h"
#include "job_queue.h"
#include "process.h"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

namespace {

yt::converter::ConversionRequest makeRequest(const std::filesystem::path& output,
                                             const std::string& url, const std::string& format,
                                             const std::string& ytdlp) {
    yt::converter::ConversionRequest request;
    request.url = url;
    request.format = format;
    request.config.output_dir = output.string();
    request.config.yt_dlp_path = ytdlp;
    request.config.ffmpeg_path = fakePath("ffmpeg");
    request.config.download_timeout_sec = 8;
    request.config.convert_timeout_sec = 8;
    request.config.reuse_completed = true;
    request.config.log_level = "ERROR";
    return request;
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

bool waitForState(yt::jobs::Queue& queue, const std::string& jobId, yt::jobs::JobState expected) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const auto snapshot = queue.find(jobId);
        if (snapshot && snapshot->state == expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

} // namespace

TEST(JobQueue, FullQueueRejectsTheThirdJob) {
    yt::process::resetShutdownForTests();
    const auto output = makeTestDir();
    setenv("YTCONV_FAKE_LOG_DIR", output.string().c_str(), 1);

    yt::jobs::Queue queue;
    StopQueue stop(queue);
    queue.start(1, 2);

    const auto first =
        makeRequest(output, "https://www.youtube.com/watch?v=dQw4w9WgXcQ", "mp3", fakePath("hang"));
    const auto second =
        makeRequest(output, "https://www.youtube.com/watch?v=jNQXAC9IVRw", "mp3", fakePath("hang"));
    const auto third =
        makeRequest(output, "https://www.youtube.com/watch?v=9bZkp7q19f0", "mp3", fakePath("hang"));
    EXPECT_NE(queue.submit(first, "dQw4w9WgXcQ", "job-a").kind, yt::jobs::SubmitKind::Full);
    EXPECT_NE(queue.submit(second, "jNQXAC9IVRw", "job-b").kind, yt::jobs::SubmitKind::Full);
    EXPECT_EQ(queue.submit(third, "9bZkp7q19f0", "job-c").kind, yt::jobs::SubmitKind::Full);

    unsetenv("YTCONV_FAKE_LOG_DIR");
}

TEST(JobQueue, CancelOneJobLeavesTheOtherRunning) {
    yt::process::resetShutdownForTests();
    const auto output = makeTestDir();
    setenv("YTCONV_FAKE_LOG_DIR", output.string().c_str(), 1);
    setenv("YTCONV_FAKE_SLEEP", "1.5", 1);

    yt::jobs::Queue queue;
    StopQueue stop(queue);
    queue.start(2, 4);

    auto first = makeRequest(output, "https://www.youtube.com/watch?v=dQw4w9WgXcQ", "mp3",
                             fakePath("yt-dlp"));
    auto second = makeRequest(output, "https://www.youtube.com/watch?v=jNQXAC9IVRw", "wav",
                              fakePath("yt-dlp"));
    ASSERT_EQ(queue.submit(first, "dQw4w9WgXcQ", "job-a").kind, yt::jobs::SubmitKind::Queued);
    ASSERT_EQ(queue.submit(second, "jNQXAC9IVRw", "job-b").kind, yt::jobs::SubmitKind::Queued);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(queue.cancel("job-a"), 1);
    EXPECT_TRUE(waitForState(queue, "job-a", yt::jobs::JobState::Canceled));
    EXPECT_TRUE(waitForState(queue, "job-b", yt::jobs::JobState::Succeeded));

    unsetenv("YTCONV_FAKE_SLEEP");
    unsetenv("YTCONV_FAKE_LOG_DIR");
}
