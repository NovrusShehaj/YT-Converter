#ifndef YT_CONVERTER_JOB_QUEUE_H
#define YT_CONVERTER_JOB_QUEUE_H

#include "converter.h"
#include "error.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace yt::jobs {

enum class JobState { Queued, Running, Succeeded, Failed, Canceled };

// Compact, bounded status record for one client job. It never owns executable request data,
// callbacks, cancellation tokens, children, or cache leases.
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
    ErrorCode code = ErrorCode::Ok;
    std::string message; // truncated to kMaxMessageBytes
    bool reused = false;
    bool attached = false; // subscribed to an operation another job started
    std::uint64_t download_ms = 0;
    std::uint64_t convert_ms = 0;
    std::uint64_t bytes = 0; // compatibility alias of source_bytes
    std::uint64_t source_bytes = 0;
    std::uint64_t output_bytes = 0;
    std::uint64_t queue_ms = 0;
    bool shared_download = false;
    bool source_cache_hit = false;
};

constexpr std::size_t kMaxMessageBytes = 512;

enum class SubmitKind {
    Queued,    // a new operation was queued for this job
    Attached,  // the job subscribed to an equivalent in-flight operation
    Full,      // queued plus running operations are at the limit
    Busy,      // live job records are at the limit
    Duplicate, // the job ID already names a live or retained job
    Stopping   // the queue is shutting down
};

struct SubmitResult {
    SubmitKind kind = SubmitKind::Full;
    JobSnapshot snapshot;
};

enum class CancelResult { Missing, Canceled, AlreadyFinished };

struct Limits {
    int workers = 2;            // bounded executors (YTCONV_MAX_CONCURRENT)
    int max_operations = 8;     // queued + running operations (YTCONV_QUEUE_DEPTH)
    int max_active_jobs = 256;  // live job/subscription records (YTCONV_MAX_ACTIVE_JOBS)
    int history_max = 1024;     // retained terminal snapshots (YTCONV_JOB_HISTORY_MAX)
    int history_ttl_sec = 3600; // terminal snapshot lifetime (YTCONV_JOB_HISTORY_TTL_SEC)
};

// Point-in-time counts for tests and metrics gauges.
struct Stats {
    std::size_t live_jobs = 0;
    std::size_t queued_jobs = 0;
    std::size_t running_jobs = 0;
    std::size_t terminal_jobs = 0;
    std::size_t job_records = 0;
    std::size_t queued_operations = 0;
    std::size_t running_operations = 0;
    std::size_t active_keys = 0;
};

// Invoked exactly once when a job becomes terminal, outside the queue lock.
using TerminalCallback = std::function<void(const JobSnapshot&)>;
using Clock = std::function<std::chrono::steady_clock::time_point()>;

// Client jobs are independently cancelable subscriptions to shared operations. An operation owns
// one execution (download/encode), its cancellation token, and its subscriber list; it is not
// owned by the job that created it. Canceling a job detaches only that job; the operation is
// canceled when its last subscriber leaves or at shutdown. Terminal states are final.
class Queue {
  public:
    Queue() = default;
    ~Queue();
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    void start(const Limits& limits);
    void start(int workers, int depth);
    // Rejects new submissions, cancels every job and operation, resolves terminal callbacks, and
    // joins workers once their children have exited.
    void stop();

    SubmitResult submit(converter::ConversionRequest request, const std::string& videoId,
                        const std::string& jobId, TerminalCallback onTerminal = {});
    std::optional<JobSnapshot> find(const std::string& jobId);
    CancelResult cancel(const std::string& jobId);
    Stats stats() const;

    void setClockForTests(Clock clock);

  private:
    struct Operation {
        std::string key;
        bool refresh = false;
        converter::ConversionRequest request;
        std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
        std::vector<std::string> subscribers; // live job IDs
        bool running = false;
        std::string stage;
        int percent = -1;
    };

    struct Job {
        JobSnapshot snapshot;
        std::shared_ptr<Operation> op; // only while live
        TerminalCallback on_terminal;  // cleared once invoked
        std::chrono::steady_clock::time_point submitted_at;
        std::chrono::steady_clock::time_point terminal_at;
        bool terminal = false;
    };

    using Notification = std::pair<TerminalCallback, JobSnapshot>;

    void workerLoop();
    std::chrono::steady_clock::time_point now() const;
    void finalizeLocked(const std::string& jobId, Job& job, JobState state,
                        const converter::ConversionResult* result, ErrorCode code,
                        const std::string& message, std::vector<Notification>& out);
    void detachLocked(const std::string& jobId, const std::shared_ptr<Operation>& op);
    void releaseKeyLocked(const std::shared_ptr<Operation>& op);
    void pruneLocked();
    JobSnapshot snapshotLocked(const Job& job) const;
    void publishGaugesLocked() const;
    static void notify(std::vector<Notification>& notifications);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    Limits limits_;
    Clock clock_;
    std::map<std::string, Job> jobs_;
    std::deque<std::shared_ptr<Operation>> waiting_;
    std::set<std::shared_ptr<Operation>> running_;
    std::map<std::string, std::shared_ptr<Operation>> activeByKey_;
    std::deque<std::pair<std::chrono::steady_clock::time_point, std::string>> terminalOrder_;
    std::size_t liveJobs_ = 0;
    std::vector<std::thread> workers_;
    bool started_ = false;
    bool stopping_ = false;
};

std::string jobStateString(JobState state);
// Coalescing class for a request: "reuse", "replace" (force or disabled completed-output reuse),
// or "refresh". Only requests of the same class and output may share an operation.
std::string policyClass(const converter::ConversionRequest& request);

} // namespace yt::jobs

#endif // YT_CONVERTER_JOB_QUEUE_H
