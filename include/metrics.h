#ifndef YT_CONVERTER_METRICS_H
#define YT_CONVERTER_METRICS_H

#include <atomic>
#include <cstdint>
#include <string>

namespace yt::metrics {

struct Counters {
    std::atomic<std::uint64_t> jobs_started{0};
    std::atomic<std::uint64_t> jobs_succeeded{0};
    std::atomic<std::uint64_t> jobs_failed{0};
    std::atomic<std::uint64_t> jobs_active{0};
    std::atomic<std::uint64_t> bytes_written{0};
    std::atomic<std::uint64_t> download_ms_total{0};
    std::atomic<std::uint64_t> convert_ms_total{0};
    std::atomic<std::uint64_t> queue_ms_total{0};
    std::atomic<std::uint64_t> bytes_downloaded{0};
    std::atomic<std::uint64_t> ready_checks{0};
    std::atomic<std::uint64_t> ready_spawns{0};
    // Queue gauges, published as absolute values from queue state (they cannot underflow).
    std::atomic<std::uint64_t> jobs_queued{0};
    std::atomic<std::uint64_t> jobs_running{0};
    std::atomic<std::uint64_t> operations_queued{0};
    std::atomic<std::uint64_t> operations_running{0};
};

Counters& global();
void recordStart();
void recordSuccess(std::uint64_t bytes, std::uint64_t download_ms, std::uint64_t convert_ms,
                   std::uint64_t bytes_downloaded);
void recordFailure();
std::string toJson();
void logSnapshot();

void recordDownloadMs(std::uint64_t ms);
void recordConvertMs(std::uint64_t ms);
void recordBytesDownloaded(std::uint64_t bytes);
void recordQueueMs(std::uint64_t ms);
void recordReadyCheck(bool spawned);
void setQueueGauges(std::uint64_t jobsQueued, std::uint64_t jobsRunning,
                    std::uint64_t operationsQueued, std::uint64_t operationsRunning);

} // namespace yt::metrics

#endif // YT_CONVERTER_METRICS_H
