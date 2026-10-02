#include "metrics.h"
#include "logger.h"

#include <sstream>

namespace yt::metrics {

Counters& global() {
    static Counters counters;
    return counters;
}

void recordJobAccepted(bool coalesced) {
    auto& counters = global();
    counters.jobs_started.fetch_add(1);
    if (coalesced) {
        counters.jobs_coalesced.fetch_add(1);
    }
}

void recordJobOutcome(JobOutcome outcome, bool reused) {
    auto& counters = global();
    switch (outcome) {
    case JobOutcome::Succeeded:
        counters.jobs_succeeded.fetch_add(1);
        if (reused) {
            counters.jobs_reused.fetch_add(1);
        }
        break;
    case JobOutcome::Failed:
        counters.jobs_failed.fetch_add(1);
        break;
    case JobOutcome::Canceled:
        counters.jobs_canceled.fetch_add(1);
        break;
    }
}

void recordDownload(bool succeeded, std::uint64_t ms, std::uint64_t sourceBytes) {
    auto& counters = global();
    counters.download_ms_total.fetch_add(ms);
    if (succeeded) {
        counters.downloads_total.fetch_add(1);
        counters.source_bytes_downloaded.fetch_add(sourceBytes);
    } else {
        counters.download_failures_total.fetch_add(1);
    }
}

void recordEncode(bool succeeded, std::uint64_t ms) {
    auto& counters = global();
    counters.convert_ms_total.fetch_add(ms);
    if (succeeded) {
        counters.encodes_total.fetch_add(1);
    } else {
        counters.encode_failures_total.fetch_add(1);
    }
}

void recordPublication(std::uint64_t outputBytes) {
    auto& counters = global();
    counters.outputs_published_total.fetch_add(1);
    counters.bytes_written.fetch_add(outputBytes);
}

void recordSourceCacheHit() {
    global().source_cache_hits_total.fetch_add(1);
}

void recordSharedDownload() {
    global().shared_downloads_total.fetch_add(1);
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
    const auto& c = global();
    std::ostringstream ss;
    ss << "{\"jobs_started\":" << c.jobs_started.load()
       << ",\"jobs_succeeded\":" << c.jobs_succeeded.load()
       << ",\"jobs_failed\":" << c.jobs_failed.load()
       << ",\"jobs_canceled\":" << c.jobs_canceled.load()
       << ",\"jobs_reused\":" << c.jobs_reused.load()
       << ",\"jobs_coalesced\":" << c.jobs_coalesced.load()
       << ",\"jobs_active\":" << (c.jobs_queued.load() + c.jobs_running.load())
       << ",\"jobs_queued\":" << c.jobs_queued.load()
       << ",\"jobs_running\":" << c.jobs_running.load()
       << ",\"operations_queued\":" << c.operations_queued.load()
       << ",\"operations_running\":" << c.operations_running.load()
       << ",\"downloads_total\":" << c.downloads_total.load()
       << ",\"download_failures_total\":" << c.download_failures_total.load()
       << ",\"download_ms_total\":" << c.download_ms_total.load() << ",\"source_bytes_downloaded\":"
       << c.source_bytes_downloaded.load()
       // Compatibility alias: same value as source_bytes_downloaded (an approximation).
       << ",\"bytes_downloaded\":" << c.source_bytes_downloaded.load()
       << ",\"source_cache_hits_total\":" << c.source_cache_hits_total.load()
       << ",\"shared_downloads_total\":" << c.shared_downloads_total.load()
       << ",\"encodes_total\":" << c.encodes_total.load()
       << ",\"encode_failures_total\":" << c.encode_failures_total.load()
       << ",\"convert_ms_total\":" << c.convert_ms_total.load()
       << ",\"outputs_published_total\":" << c.outputs_published_total.load()
       << ",\"bytes_written\":" << c.bytes_written.load()
       << ",\"queue_ms_total\":" << c.queue_ms_total.load()
       << ",\"ready_checks\":" << c.ready_checks.load()
       << ",\"ready_spawns\":" << c.ready_spawns.load() << '}';
    return ss.str();
}

void logSnapshot() {
    yt::logger::Logger::getInstance().info("metrics " + toJson());
}

} // namespace yt::metrics
