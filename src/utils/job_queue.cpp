#include "job_queue.h"
#include "error.h"
#include "logger.h"
#include "metrics.h"
#include "source_cache.h"

#include <algorithm>
#include <utility>

namespace yt::jobs {
namespace {

std::string truncateMessage(std::string message) {
    if (message.size() > kMaxMessageBytes) {
        message.resize(kMaxMessageBytes);
    }
    return message;
}

std::string coalesceKey(const converter::ConversionRequest& request, const std::string& videoId) {
    std::string key = videoId + "\n" + request.format + "\n" + policyClass(request);
    if (request.format == "mp4") {
        key += "\n" + std::to_string(request.config.max_height);
    }
    return key;
}

std::uint64_t millisBetween(std::chrono::steady_clock::time_point from,
                            std::chrono::steady_clock::time_point to) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
    return static_cast<std::uint64_t>(std::max<std::int64_t>(0, ms));
}

} // namespace

std::string jobStateString(JobState state) {
    switch (state) {
    case JobState::Queued:
        return "queued";
    case JobState::Running:
        return "running";
    case JobState::Succeeded:
        return "succeeded";
    case JobState::Failed:
        return "failed";
    case JobState::Canceled:
        return "canceled";
    }
    return "failed";
}

std::string policyClass(const converter::ConversionRequest& request) {
    if (request.refresh) {
        return "refresh";
    }
    if (request.config.force || !request.config.reuse_completed) {
        return "replace";
    }
    return "reuse";
}

Queue::~Queue() {
    stop();
}

void Queue::start(int workers, int depth) {
    Limits limits;
    limits.workers = workers;
    limits.max_operations = depth;
    start(limits);
}

void Queue::start(const Limits& limits) {
    int count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_) {
            return;
        }
        limits_ = limits;
        limits_.workers = std::max(1, limits.workers);
        limits_.max_operations = std::max(1, limits.max_operations);
        limits_.max_active_jobs = std::max(1, limits.max_active_jobs);
        limits_.history_max = std::max(0, limits.history_max);
        limits_.history_ttl_sec = std::max(1, limits.history_ttl_sec);
        started_ = true;
        stopping_ = false;
        count = limits_.workers;
    }
    workers_.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        workers_.emplace_back(&Queue::workerLoop, this);
    }
}

void Queue::stop() {
    std::vector<Notification> notifications;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || stopping_) {
            return;
        }
        stopping_ = true;
        for (auto& [id, job] : jobs_) {
            if (!job.terminal) {
                finalizeLocked(id, job, JobState::Canceled, nullptr, ErrorCode::Canceled,
                               "Canceled: server shutting down", notifications);
            }
        }
        for (const auto& op : waiting_) {
            op->cancel->store(true);
            op->subscribers.clear();
        }
        waiting_.clear();
        for (const auto& op : running_) {
            op->cancel->store(true);
            op->subscribers.clear();
        }
        activeByKey_.clear();
        publishGaugesLocked();
    }
    cv_.notify_all();
    // Sync HTTP responses waiting on these jobs resolve before workers are joined.
    notify(notifications);
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
}

void Queue::setClockForTests(Clock clock) {
    std::lock_guard<std::mutex> lock(mutex_);
    clock_ = std::move(clock);
}

std::chrono::steady_clock::time_point Queue::now() const {
    return clock_ ? clock_() : std::chrono::steady_clock::now();
}

SubmitResult Queue::submit(converter::ConversionRequest request, const std::string& videoId,
                           const std::string& jobId, TerminalCallback onTerminal) {
    std::lock_guard<std::mutex> lock(mutex_);
    SubmitResult result;
    if (!started_ || stopping_) {
        result.kind = SubmitKind::Stopping;
        return result;
    }
    pruneLocked();
    if (jobs_.count(jobId) != 0) {
        // Never silently replace an existing (live or retained) record.
        result.kind = SubmitKind::Duplicate;
        return result;
    }
    // The subscription bound is checked before anything is allocated, attached or not.
    if (liveJobs_ >= static_cast<std::size_t>(limits_.max_active_jobs)) {
        result.kind = SubmitKind::Busy;
        return result;
    }

    const std::string key = coalesceKey(request, videoId);
    const std::string format = request.format;
    const std::string requestId = request.request_id;
    std::shared_ptr<Operation> op;
    const auto active = activeByKey_.find(key);
    if (active != activeByKey_.end()) {
        const auto& candidate = active->second;
        // A refresh only shares an operation that has not started: its download will begin
        // after this request was admitted.
        if (!candidate->cancel->load() && (!candidate->refresh || !candidate->running)) {
            op = candidate;
        }
    }

    if (!op) {
        if (waiting_.size() + running_.size() >= static_cast<std::size_t>(limits_.max_operations)) {
            result.kind = SubmitKind::Full;
            return result;
        }
        op = std::make_shared<Operation>();
        op->key = key;
        op->refresh = request.refresh;
        if (request.admitted_at == 0) {
            request.admitted_at = cache::nowStamp();
        }
        op->request = std::move(request);
        waiting_.push_back(op);
        activeByKey_[key] = op;
        result.kind = SubmitKind::Queued;
        cv_.notify_one();
    } else {
        result.kind = SubmitKind::Attached;
    }

    Job job;
    job.snapshot.job_id = jobId;
    job.snapshot.video_id = videoId;
    job.snapshot.format = format;
    job.snapshot.request_id = requestId;
    job.snapshot.attached = result.kind == SubmitKind::Attached;
    job.snapshot.message = job.snapshot.attached ? "Attached to in-flight conversion" : "Queued";
    job.op = op;
    job.on_terminal = std::move(onTerminal);
    job.submitted_at = now();
    op->subscribers.push_back(jobId);
    ++liveJobs_;
    yt::metrics::recordJobAccepted(job.snapshot.attached);
    auto& stored = jobs_[jobId];
    stored = std::move(job);
    result.snapshot = snapshotLocked(stored);
    publishGaugesLocked();
    return result;
}

std::optional<JobSnapshot> Queue::find(const std::string& jobId) {
    std::lock_guard<std::mutex> lock(mutex_);
    pruneLocked();
    const auto it = jobs_.find(jobId);
    if (it == jobs_.end()) {
        return std::nullopt;
    }
    return snapshotLocked(it->second);
}

CancelResult Queue::cancel(const std::string& jobId) {
    std::vector<Notification> notifications;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pruneLocked();
        const auto it = jobs_.find(jobId);
        if (it == jobs_.end()) {
            return CancelResult::Missing;
        }
        Job& job = it->second;
        if (job.terminal) {
            return CancelResult::AlreadyFinished;
        }
        const std::shared_ptr<Operation> op = job.op;
        finalizeLocked(jobId, job, JobState::Canceled, nullptr, ErrorCode::Canceled, "Canceled",
                       notifications);
        detachLocked(jobId, op);
        publishGaugesLocked();
    }
    notify(notifications);
    return CancelResult::Canceled;
}

Stats Queue::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats stats;
    stats.live_jobs = liveJobs_;
    stats.job_records = jobs_.size();
    stats.terminal_jobs = jobs_.size() - liveJobs_;
    stats.queued_operations = waiting_.size();
    stats.running_operations = running_.size();
    stats.active_keys = activeByKey_.size();
    for (const auto& entry : jobs_) {
        if (!entry.second.terminal && entry.second.op) {
            if (entry.second.op->running) {
                ++stats.running_jobs;
            } else {
                ++stats.queued_jobs;
            }
        }
    }
    return stats;
}

// Removes one job's interest in its operation. The last subscriber leaving cancels the
// operation: a queued one is dropped immediately (freeing its slot), a running one has its token
// set and keeps its worker slot until its child has actually stopped.
void Queue::detachLocked(const std::string& jobId, const std::shared_ptr<Operation>& op) {
    if (!op) {
        return;
    }
    auto& subscribers = op->subscribers;
    subscribers.erase(std::remove(subscribers.begin(), subscribers.end(), jobId),
                      subscribers.end());
    if (!subscribers.empty()) {
        return;
    }
    op->cancel->store(true);
    releaseKeyLocked(op);
    if (!op->running) {
        waiting_.erase(std::remove(waiting_.begin(), waiting_.end(), op), waiting_.end());
    }
}

// New submissions never attach to an operation that is finishing or being canceled; they start
// a new operation instead. Only this operation's own mapping is removed.
void Queue::releaseKeyLocked(const std::shared_ptr<Operation>& op) {
    const auto active = activeByKey_.find(op->key);
    if (active != activeByKey_.end() && active->second == op) {
        activeByKey_.erase(active);
    }
}

void Queue::finalizeLocked(const std::string& jobId, Job& job, JobState state,
                           const converter::ConversionResult* result, ErrorCode code,
                           const std::string& message, std::vector<Notification>& out) {
    if (job.terminal) {
        return; // terminal states are final
    }
    job.terminal = true;
    job.terminal_at = now();
    job.op.reset();
    JobSnapshot& snapshot = job.snapshot;
    snapshot.state = state;
    snapshot.code = code;
    snapshot.error_code = errorCodeString(code);
    snapshot.message = truncateMessage(message);
    if (state == JobState::Succeeded && result != nullptr) {
        snapshot.stage = "done";
        snapshot.percent = 100;
        snapshot.output_path = result->output_path;
        snapshot.reused = result->reused;
        snapshot.download_ms = result->download_ms;
        snapshot.convert_ms = result->convert_ms;
        snapshot.bytes = result->source_bytes;
        snapshot.source_bytes = result->source_bytes;
        snapshot.output_bytes = result->output_bytes;
        snapshot.shared_download = result->shared_download;
        snapshot.source_cache_hit = result->source_cache_hit;
    }
    --liveJobs_;
    yt::metrics::recordJobOutcome(state == JobState::Succeeded  ? yt::metrics::JobOutcome::Succeeded
                                  : state == JobState::Canceled ? yt::metrics::JobOutcome::Canceled
                                                                : yt::metrics::JobOutcome::Failed,
                                  snapshot.reused);
    terminalOrder_.emplace_back(job.terminal_at, jobId);
    if (job.on_terminal) {
        out.emplace_back(std::move(job.on_terminal), snapshot);
        job.on_terminal = nullptr;
    }
}

// Drops terminal snapshots past the TTL (monotonic clock, measured from completion) and then the
// oldest ones beyond history_max. Live jobs are never pruned.
void Queue::pruneLocked() {
    const auto current = now();
    const auto ttl = std::chrono::seconds(limits_.history_ttl_sec);
    const std::size_t maxHistory = static_cast<std::size_t>(limits_.history_max);
    while (!terminalOrder_.empty()) {
        const auto& [at, id] = terminalOrder_.front();
        const std::size_t terminalCount = jobs_.size() - liveJobs_;
        if (current - at < ttl && terminalCount <= maxHistory) {
            break;
        }
        const auto it = jobs_.find(id);
        if (it != jobs_.end() && it->second.terminal) {
            jobs_.erase(it);
        }
        terminalOrder_.pop_front();
    }
}

JobSnapshot Queue::snapshotLocked(const Job& job) const {
    JobSnapshot snapshot = job.snapshot;
    if (!job.terminal && job.op) {
        if (job.op->running) {
            snapshot.state = JobState::Running;
            snapshot.message = "Running";
        }
        snapshot.stage = job.op->stage;
        snapshot.percent = job.op->percent;
    }
    return snapshot;
}

void Queue::publishGaugesLocked() const {
    std::uint64_t queuedJobs = 0;
    std::uint64_t runningJobs = 0;
    for (const auto& op : waiting_) {
        queuedJobs += op->subscribers.size();
    }
    for (const auto& op : running_) {
        runningJobs += op->subscribers.size();
    }
    yt::metrics::setQueueGauges(queuedJobs, runningJobs, waiting_.size(), running_.size());
}

void Queue::notify(std::vector<Notification>& notifications) {
    for (auto& [callback, snapshot] : notifications) {
        try {
            callback(snapshot);
        } catch (const std::exception& error) {
            yt::logger::Logger::getInstance().error(std::string("Job callback failed: ") +
                                                    error.what());
        } catch (...) {
            yt::logger::Logger::getInstance().error("Job callback failed");
        }
    }
    notifications.clear();
}

void Queue::workerLoop() {
    auto& logger = yt::logger::Logger::getInstance();
    while (true) {
        std::shared_ptr<Operation> op;
        converter::ConversionRequest request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !waiting_.empty(); });
            if (stopping_) {
                return;
            }
            op = waiting_.front();
            waiting_.pop_front();
            if (op->cancel->load() || op->subscribers.empty()) {
                releaseKeyLocked(op);
                continue;
            }
            op->running = true;
            op->stage = "download";
            running_.insert(op);
            const auto started = now();
            for (const auto& id : op->subscribers) {
                const auto it = jobs_.find(id);
                if (it != jobs_.end() && !it->second.terminal) {
                    const auto queueMs = millisBetween(it->second.submitted_at, started);
                    it->second.snapshot.queue_ms = queueMs;
                    yt::metrics::recordQueueMs(queueMs);
                }
            }
            // The executable request moves to this worker; the operation keeps only state.
            request = std::move(op->request);
            publishGaugesLocked();
        }

        std::weak_ptr<Operation> weak = op;
        request.cancel = op->cancel;
        request.on_progress = [this, weak](const std::string& stage, int percent) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (const auto live = weak.lock()) {
                live->stage = stage;
                if (percent >= 0) {
                    live->percent = percent;
                }
            }
        };

        converter::ConversionResult conversion;
        bool succeeded = false;
        ErrorCode code = ErrorCode::Ok;
        std::string message;
        try {
            conversion = converter::processVideo(request);
            succeeded = true;
        } catch (const Error& error) {
            code = error.code();
            message = error.message();
        } catch (const std::exception& error) {
            code = ErrorCode::Internal;
            message = error.what();
        } catch (...) {
            code = ErrorCode::Internal;
            message = "conversion failed";
        }
        if (code != ErrorCode::Ok && code != ErrorCode::Canceled) {
            logger.warning("Conversion operation failed: " + message);
        }

        std::vector<Notification> notifications;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_.erase(op);
            releaseKeyLocked(op);
            // Only jobs still subscribed receive the outcome; canceled ones stay canceled.
            for (const auto& id : op->subscribers) {
                const auto it = jobs_.find(id);
                if (it == jobs_.end()) {
                    continue;
                }
                if (succeeded) {
                    finalizeLocked(id, it->second, JobState::Succeeded, &conversion, ErrorCode::Ok,
                                   conversion.reused ? "Reused existing output"
                                                     : "Conversion completed",
                                   notifications);
                } else if (code == ErrorCode::Canceled) {
                    finalizeLocked(id, it->second, JobState::Canceled, nullptr, code, "Canceled",
                                   notifications);
                } else {
                    finalizeLocked(id, it->second, JobState::Failed, nullptr, code, message,
                                   notifications);
                }
            }
            op->subscribers.clear();
            pruneLocked();
            publishGaugesLocked();
        }
        notify(notifications);
    }
}

} // namespace yt::jobs
