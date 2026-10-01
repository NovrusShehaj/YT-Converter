#ifndef YT_CONVERTER_JOB_QUEUE_H
#define YT_CONVERTER_JOB_QUEUE_H

#include "converter.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace yt::jobs {

enum class JobState {
    Queued,
    Running,
    Succeeded,
    Failed,
    Canceled
};

struct JobSnapshot {
    std::string job_id;
    std::string video_id;
    std::string format;
    std::string request_id;
    JobState state = JobState::Queued;
    std::string stage;
    int percent = -1;
    std::string output_path;
    std::string error_code = "ok";
    std::string message;
    bool reused = false;
    std::uint64_t download_ms = 0;
    std::uint64_t convert_ms = 0;
    std::uint64_t bytes = 0;
    std::uint64_t queue_ms = 0;
};

enum class SubmitKind {
    Queued,
    Attached,
    Full
};

struct SubmitResult {
    SubmitKind kind = SubmitKind::Full;
    JobSnapshot snapshot;
};

class Queue {
public:
    Queue() = default;
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    void start(int workers, int depth);
    void stop();

    SubmitResult submit(converter::ConversionRequest request, const std::string& videoId,
                        const std::string& jobId);
    std::optional<JobSnapshot> find(const std::string& jobId) const;
    // 0 missing, 1 canceled, 2 already finished
    int cancel(const std::string& jobId);

private:
    struct Record {
        JobSnapshot snapshot;
        std::string leader_id;
        std::string coalesce_key;
        converter::ConversionRequest request;
        std::chrono::steady_clock::time_point queued_at = std::chrono::steady_clock::now();
        std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
        bool counted = false;
    };

    void workerLoop(int workerIndex);
    JobSnapshot copySnapshot(const Record& record) const;
    void clearActiveLocked(const Record& record);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::string> waiting_;
    std::map<std::string, std::shared_ptr<Record>> jobs_;
    std::map<std::string, std::string> activeByKey_;
    std::size_t capacity_ = 8;
    int running_ = 0;
    int workerCount_ = 1;
    std::vector<std::thread> workers_;
    bool started_ = false;
};

std::string jobStateString(JobState state);

} // namespace yt::jobs

#endif // YT_CONVERTER_JOB_QUEUE_H
