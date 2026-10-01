#ifndef YT_CONVERTER_JOB_QUEUE_H
#define YT_CONVERTER_JOB_QUEUE_H

#include "converter.h"
#include "error.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace yt::jobs {

struct QueuedJob {
    std::string job_id;
    std::string video_id;
    converter::ConversionRequest request;
    std::chrono::steady_clock::time_point queued_at;
};

class Queue {
public:
    Queue() = default;
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    void setWorkerCount(int count);
    void setQueueDepth(int depth);

    void start();
    void stop();

    void enqueue(QueuedJob job);
    QueuedJob dequeue();

    void registerJob(const std::string& job_id, QueuedJob job);
    QueuedJob* findJob(const std::string& job_id);
    void removeJob(const std::string& job_id);

    void cancelJob(const std::string& job_id);

    bool empty() const;
    std::size_t size() const;
    std::size_t capacity() const;

private:
    void workerLoop(int workerIndex);
    bool tryEnqueue(QueuedJob job);
    void signalEnqueue();

    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable enqueueCv_;
    std::queue<QueuedJob> queue_;
    std::size_t capacity_ = 8;
    int workerCount_ = 1;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};
    std::map<std::string, QueuedJob> jobMap_;
    std::mutex jobMapMutex_;
};

} // namespace yt::jobs

#endif // YT_CONVERTER_JOB_QUEUE_H
