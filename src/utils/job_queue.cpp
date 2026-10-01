#include "job_queue.h"
#include "error.h"
#include "logger.h"
#include "metrics.h"
#include "process.h"

#include <algorithm>
#include <utility>

namespace yt::jobs {
namespace {

std::string coalesceKey(const std::string& videoId, const std::string& format) {
    return videoId + "\n" + format;
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

void Queue::start(int workers, int depth) {
    int count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_) {
            return;
        }
        workerCount_ = std::max(1, workers);
        capacity_ = std::max<std::size_t>(1, static_cast<std::size_t>(depth));
        started_ = true;
        count = workerCount_;
    }
    workers_.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        workers_.emplace_back(&Queue::workerLoop, this, i);
    }
}

void Queue::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        started_ = false;
        for (const std::string& id : waiting_) {
            const auto it = jobs_.find(id);
            if (it != jobs_.end() && it->second) {
                it->second->cancel->store(true);
                it->second->snapshot.state = JobState::Canceled;
                it->second->snapshot.error_code = "canceled";
                it->second->snapshot.message = "Canceled";
                activeByKey_.erase(it->second->coalesce_key);
            }
        }
        waiting_.clear();
        for (auto& entry : jobs_) {
            if (entry.second) {
                entry.second->cancel->store(true);
            }
        }
    }
    cv_.notify_all();
    yt::process::requestShutdown();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    yt::process::resetShutdownForTests();
}

SubmitResult Queue::submit(converter::ConversionRequest request, const std::string& videoId,
                           const std::string& jobId) {
    std::lock_guard<std::mutex> lock(mutex_);
    SubmitResult result;
    const std::string key = coalesceKey(videoId, request.format);
    if (!request.config.force && !request.refresh) {
        const auto active = activeByKey_.find(key);
        if (active != activeByKey_.end()) {
            auto follower = std::make_shared<Record>();
            follower->snapshot.job_id = jobId;
            follower->snapshot.video_id = videoId;
            follower->snapshot.format = request.format;
            follower->snapshot.request_id = request.request_id;
            follower->snapshot.state = JobState::Queued;
            follower->snapshot.message = "Attached to in-flight conversion";
            follower->leader_id = active->second;
            follower->coalesce_key = key;
            follower->request = std::move(request);
            jobs_[jobId] = follower;
            result.kind = SubmitKind::Attached;
            result.snapshot = copySnapshot(*follower);
            return result;
        }
    }

    const std::size_t unfinished =
        waiting_.size() + static_cast<std::size_t>(std::max(0, running_));
    if (unfinished >= capacity_) {
        result.kind = SubmitKind::Full;
        return result;
    }

    auto record = std::make_shared<Record>();
    record->snapshot.job_id = jobId;
    record->snapshot.video_id = videoId;
    record->snapshot.format = request.format;
    record->snapshot.request_id = request.request_id;
    record->snapshot.state = JobState::Queued;
    record->snapshot.message = "Queued";
    record->coalesce_key = key;
    record->request = std::move(request);
    record->queued_at = std::chrono::steady_clock::now();
    jobs_[jobId] = record;
    activeByKey_[key] = jobId;
    waiting_.push_back(jobId);
    result.kind = SubmitKind::Queued;
    result.snapshot = copySnapshot(*record);
    cv_.notify_one();
    return result;
}

std::optional<JobSnapshot> Queue::find(const std::string& jobId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(jobId);
    if (it == jobs_.end() || !it->second) {
        return std::nullopt;
    }
    return copySnapshot(*it->second);
}

int Queue::cancel(const std::string& jobId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(jobId);
    if (it == jobs_.end() || !it->second) {
        return 0;
    }
    Record& record = *it->second;
    std::shared_ptr<Record> target = it->second;
    if (!record.leader_id.empty()) {
        const auto leader = jobs_.find(record.leader_id);
        if (leader != jobs_.end()) {
            target = leader->second;
        }
    }
    if (!target) {
        return 0;
    }
    if (target->snapshot.state == JobState::Succeeded ||
        target->snapshot.state == JobState::Failed ||
        target->snapshot.state == JobState::Canceled) {
        return 2;
    }
    target->cancel->store(true);
    target->snapshot.state = JobState::Canceled;
    target->snapshot.error_code = "canceled";
    target->snapshot.message = "Canceled";
    clearActiveLocked(*target);
    waiting_.erase(std::remove(waiting_.begin(), waiting_.end(), target->snapshot.job_id),
                   waiting_.end());
    return 1;
}

void Queue::clearActiveLocked(const Record& record) {
    const auto active = activeByKey_.find(record.coalesce_key);
    if (active != activeByKey_.end() && active->second == record.snapshot.job_id) {
        activeByKey_.erase(active);
    }
}

JobSnapshot Queue::copySnapshot(const Record& record) const {
    if (record.leader_id.empty()) {
        return record.snapshot;
    }
    const auto leader = jobs_.find(record.leader_id);
    if (leader == jobs_.end() || !leader->second) {
        return record.snapshot;
    }
    JobSnapshot snapshot = leader->second->snapshot;
    snapshot.job_id = record.snapshot.job_id;
    snapshot.request_id = record.snapshot.request_id;
    return snapshot;
}

void Queue::workerLoop(int workerIndex) {
    auto& logger = yt::logger::Logger::getInstance();
    while (true) {
        std::shared_ptr<Record> record;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !started_ || !waiting_.empty(); });
            if (!started_ && waiting_.empty()) {
                return;
            }
            if (waiting_.empty()) {
                continue;
            }
            const std::string id = waiting_.front();
            waiting_.pop_front();
            const auto it = jobs_.find(id);
            if (it == jobs_.end()) {
                continue;
            }
            record = it->second;
            ++running_;
            record->counted = true;
        }

        if (!record) {
            continue;
        }
        if (record->cancel->load()) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (record->snapshot.state != JobState::Canceled) {
                record->snapshot.state = JobState::Canceled;
                record->snapshot.error_code = "canceled";
                record->snapshot.message = "Canceled";
            }
            clearActiveLocked(*record);
            if (record->counted) {
                --running_;
                record->counted = false;
            }
            continue;
        }

        const auto queueMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - record->queued_at)
                                 .count();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            record->snapshot.state = JobState::Running;
            record->snapshot.stage = "download";
            record->snapshot.queue_ms =
                static_cast<std::uint64_t>(std::max<std::int64_t>(0, queueMs));
            record->snapshot.message = "Running";
        }
        yt::metrics::recordQueueMs(static_cast<std::uint64_t>(std::max<std::int64_t>(0, queueMs)));

        logger.setContext({record->request.request_id, record->snapshot.video_id});
        struct ContextGuard {
            ~ContextGuard() { yt::logger::Logger::getInstance().clearContext(); }
        } guard;

        auto* queue = this;
        const std::string jobId = record->snapshot.job_id;
        record->request.cancel = record->cancel;
        record->request.on_progress = [queue, jobId](const std::string& stage, int percent) {
            std::lock_guard<std::mutex> lock(queue->mutex_);
            const auto it = queue->jobs_.find(jobId);
            if (it == queue->jobs_.end() || !it->second) {
                return;
            }
            it->second->snapshot.stage = stage;
            if (percent >= 0) {
                it->second->snapshot.percent = percent;
            }
        };

        try {
            const auto conversion = yt::converter::processVideo(record->request);
            std::lock_guard<std::mutex> lock(mutex_);
            if (record->snapshot.state != JobState::Canceled && !record->cancel->load()) {
                record->snapshot.state = JobState::Succeeded;
                record->snapshot.stage = "done";
                record->snapshot.output_path = conversion.output_path;
                record->snapshot.error_code = "ok";
                record->snapshot.message =
                    conversion.reused ? "Reused existing output" : "Conversion completed";
                record->snapshot.reused = conversion.reused;
                record->snapshot.download_ms = conversion.download_ms;
                record->snapshot.convert_ms = conversion.convert_ms;
                record->snapshot.bytes = conversion.bytes_downloaded;
                record->snapshot.percent = 100;
            }
            clearActiveLocked(*record);
        } catch (const yt::Error& error) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (error.code() == yt::ErrorCode::Canceled || record->cancel->load()) {
                record->snapshot.state = JobState::Canceled;
                record->snapshot.error_code = "canceled";
                record->snapshot.message = "Canceled";
            } else if (record->snapshot.state != JobState::Canceled) {
                record->snapshot.state = JobState::Failed;
                record->snapshot.error_code = error.codeString();
                record->snapshot.message = error.message();
            }
            clearActiveLocked(*record);
            logger.warning(std::string("Queue worker ") + std::to_string(workerIndex) + " failed " +
                           record->snapshot.job_id + ": " + error.message());
        } catch (const std::exception& error) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (record->snapshot.state != JobState::Canceled) {
                record->snapshot.state = JobState::Failed;
                record->snapshot.error_code = "internal_error";
                record->snapshot.message = error.what();
            }
            clearActiveLocked(*record);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (record->counted) {
            --running_;
            record->counted = false;
        }
    }
}

} // namespace yt::jobs
