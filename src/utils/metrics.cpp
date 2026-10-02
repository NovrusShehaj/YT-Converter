#include "metrics.h"
#include "logger.h"

#include <sstream>

namespace yt::metrics {

Counters& global() {
    static Counters counters;
    return counters;
}

void recordStart() {
    auto& counters = global();
    counters.jobs_started.fetch_add(1);
    counters.jobs_active.fetch_add(1);
}

void recordSuccess(std::uint64_t bytes, std::uint64_t download_ms, std::uint64_t convert_ms,
                   std::uint64_t bytes_downloaded) {
    auto& counters = global();
    counters.jobs_succeeded.fetch_add(1);
    if (counters.jobs_active.load() > 0) {
        counters.jobs_active.fetch_sub(1);
    }
    counters.bytes_written.fetch_add(bytes);
    counters.download_ms_total.fetch_add(download_ms);
    counters.convert_ms_total.fetch_add(convert_ms);
    counters.bytes_downloaded.fetch_add(bytes_downloaded);
}

void recordFailure() {
    auto& counters = global();
    counters.jobs_failed.fetch_add(1);
    if (counters.jobs_active.load() > 0) {
        counters.jobs_active.fetch_sub(1);
    }
}

void recordDownloadMs(std::uint64_t ms) {
    global().download_ms_total.fetch_add(ms);
}

void recordConvertMs(std::uint64_t ms) {
    global().convert_ms_total.fetch_add(ms);
}

void recordBytesDownloaded(std::uint64_t bytes) {
    global().bytes_downloaded.fetch_add(bytes);
}

void recordQueueMs(std::uint64_t ms) {
    global().queue_ms_total.fetch_add(ms);
}

void recordReadyCheck(bool spawned) {
    global().ready_checks.fetch_add(1);
    if (spawned) {
        global().ready_spawns.fetch_add(1);
    }
}

void setQueueGauges(std::uint64_t jobsQueued, std::uint64_t jobsRunning,
                    std::uint64_t operationsQueued, std::uint64_t operationsRunning) {
    auto& counters = global();
    counters.jobs_queued.store(jobsQueued);
    counters.jobs_running.store(jobsRunning);
    counters.operations_queued.store(operationsQueued);
    counters.operations_running.store(operationsRunning);
}

std::string toJson() {
    const auto& counters = global();
    std::ostringstream ss;
    ss << "{\"jobs_started\":" << counters.jobs_started.load()
       << ",\"jobs_succeeded\":" << counters.jobs_succeeded.load()
       << ",\"jobs_failed\":" << counters.jobs_failed.load()
       << ",\"jobs_active\":" << counters.jobs_active.load()
       << ",\"bytes_written\":" << counters.bytes_written.load()
       << ",\"download_ms_total\":" << counters.download_ms_total.load()
       << ",\"convert_ms_total\":" << counters.convert_ms_total.load()
       << ",\"queue_ms_total\":" << counters.queue_ms_total.load()
       << ",\"bytes_downloaded\":" << counters.bytes_downloaded.load()
       << ",\"ready_checks\":" << counters.ready_checks.load()
       << ",\"ready_spawns\":" << counters.ready_spawns.load()
       << ",\"jobs_queued\":" << counters.jobs_queued.load()
       << ",\"jobs_running\":" << counters.jobs_running.load()
       << ",\"operations_queued\":" << counters.operations_queued.load()
       << ",\"operations_running\":" << counters.operations_running.load() << '}';
    return ss.str();
}

void logSnapshot() {
    yt::logger::Logger::getInstance().info("metrics " + toJson());
}

} // namespace yt::metrics
