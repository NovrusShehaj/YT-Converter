#include "job_queue.h"
#include "logger.h"
#include "process.h"

#include <algorithm>
#include <chrono>
#include <csignal>

namespace yt::jobs {

void Queue::setWorkerCount(int count) {
    workerCount_ = std::max(1, count);
}

void Queue::setQueueDepth(int depth) {
    capacity_ = std::max<std::size_t>(1, static_cast<std::size_t>(depth));
}

void Queue::start() {
    if (running_.exchange(true)) {
        return;
    }
    for (int i = 0; i < workerCount_; ++i) {
        workers_.emplace_back(&Queue::workerLoop, this, i);
    }
}

void Queue::stop() {
    running_.store(false);
    cv_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable()) {
            w.join();
        }
    }
    workers_.clear();
}

void Queue::enqueue(QueuedJob job) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!tryEnqueue(std::move(job))) {
        throw Error(ErrorCode::Busy, "Too many concurrent conversions");
    }
    signalEnqueue();
}

bool Queue::tryEnqueue(QueuedJob job) {
    if (queue_.size() >= capacity_) {
        return false;
    }
    queue_.push(std::move(job));
    return true;
}

void Queue::signalEnqueue() {
    cv_.notify_one();
    enqueueCv_.notify_one();
}

QueuedJob Queue::dequeue() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !running_ || !queue_.empty(); });
    if (!running_ && queue_.empty()) {
        return QueuedJob{};
    }
    auto job = std::move(queue_.front());
    queue_.pop();
    return job;
}

void Queue::workerLoop(int workerIndex) {
    auto& logger = yt::logger::Logger::getInstance();
    while (running_.load(std::memory_order_acquire)) {
        auto job = dequeue();
        if (!running_) {
            break;
        }
        if (job.job_id.empty()) {
            continue;
        }
        logger.setContext({job.request.request_id, job.video_id});
        struct ContextGuard {
            ~ContextGuard() { yt::logger::Logger::getInstance().clearContext(); }
        } guard;

        auto startTime = std::chrono::steady_clock::now();
        try {
            const auto result = yt::converter::processVideo(job.request);
            logger.info("Queue worker " + std::to_string(workerIndex) + " completed " + job.job_id);
        } catch (const std::exception& ex) {
            logger.warning(std::string("Queue worker failed: ") + ex.what());
        } catch (...) {
            logger.warning("Queue worker failed with unknown error");
        }
        const auto elapsed = std::chrono::steady_clock::now() - startTime;
        logger.info("Worker " + std::to_string(workerIndex) + " finished job " + job.job_id);
    }
}

void Queue::registerJob(const std::string& job_id, QueuedJob job) {
    std::lock_guard<std::mutex> lock(jobMapMutex_);
    jobMap_[job_id] = std::move(job);
}

QueuedJob* Queue::findJob(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(jobMapMutex_);
    auto it = jobMap_.find(job_id);
    if (it == jobMap_.end()) {
        return nullptr;
    }
    return &it->second;
}

void Queue::removeJob(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(jobMapMutex_);
    jobMap_.erase(job_id);
}

void Queue::cancelJob(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(jobMapMutex_);
    auto it = jobMap_.find(job_id);
    if (it != jobMap_.end()) {
        // Signal the queue to cancel this job
        cv_.notify_all();
    }
}

bool Queue::empty() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
    return queue_.empty();
}

std::size_t Queue::size() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
    return queue_.size();
}

std::size_t Queue::capacity() const {
    return capacity_;
}

} // namespace yt::jobs
