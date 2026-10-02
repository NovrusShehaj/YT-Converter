#ifndef YT_CONVERTER_METRICS_H
#define YT_CONVERTER_METRICS_H

#include <atomic>
#include <cstdint>
#include <string>

namespace yt::metrics {

// Two kinds of counters, never mixed:
//
// Physical work is recorded once where it happens (one download, one encode, one publication),
// no matter how many jobs share it. Logical job outcomes are recorded once per client job by the
// job's owner (the queue or the HTTP fast path), including attached and reused jobs. The CLI's
// single job is reported by its exit status instead.
// Gauges are published as absolute values from queue state, so they cannot underflow.
struct Counters {
    // Logical client jobs.
    std::atomic<std::uint64_t> jobs_started{0}; // accepted jobs (queued, attached, or reused)
    std::atomic<std::uint64_t> jobs_succeeded{0};
    std::atomic<std::uint64_t> jobs_failed{0};
    std::atomic<std::uint64_t> jobs_canceled{0};
    std::atomic<std::uint64_t> jobs_reused{0};    // satisfied by a completed output file
    std::atomic<std::uint64_t> jobs_coalesced{0}; // attached to an existing operation

    // Physical downloads. source_bytes_downloaded is the size of each newly downloaded source
    // file: an approximation of media bytes, not wire traffic (retries, metadata, headers, and
    // merge overhead are not counted). Failed downloads add 0 bytes.
    std::atomic<std::uint64_t> downloads_total{0};
    std::atomic<std::uint64_t> download_failures_total{0}; // failed or canceled after spawning
    std::atomic<std::uint64_t> download_ms_total{0};       // every attempt, success or not
    std::atomic<std::uint64_t> source_bytes_downloaded{0};
    std::atomic<std::uint64_t> source_cache_hits_total{0}; // operations served from the cache
    std::atomic<std::uint64_t> shared_downloads_total{0};  // operations that joined a download

    // Physical encodes and publications.
    std::atomic<std::uint64_t> encodes_total{0};
    std::atomic<std::uint64_t> encode_failures_total{0};
    std::atomic<std::uint64_t> convert_ms_total{0}; // every ffmpeg attempt
    std::atomic<std::uint64_t> outputs_published_total{0};
    std::atomic<std::uint64_t> bytes_written{0}; // newly published output bytes (not reuse)

    std::atomic<std::uint64_t> queue_ms_total{0};
    std::atomic<std::uint64_t> ready_checks{0};
    std::atomic<std::uint64_t> ready_spawns{0};

    // Gauges.
    std::atomic<std::uint64_t> jobs_queued{0};
    std::atomic<std::uint64_t> jobs_running{0};
    std::atomic<std::uint64_t> operations_queued{0};
    std::atomic<std::uint64_t> operations_running{0};
};

enum class JobOutcome { Succeeded, Failed, Canceled };

Counters& global();

// Logical job accounting (one call each per client job).
void recordJobAccepted(bool coalesced);
void recordJobOutcome(JobOutcome outcome, bool reused);

// Physical work accounting (one call per actual child run or publication).
void recordDownload(bool succeeded, std::uint64_t ms, std::uint64_t sourceBytes);
void recordEncode(bool succeeded, std::uint64_t ms);
void recordPublication(std::uint64_t outputBytes);
void recordSourceCacheHit();
void recordSharedDownload();

void recordQueueMs(std::uint64_t ms);
void recordReadyCheck(bool spawned);
void setQueueGauges(std::uint64_t jobsQueued, std::uint64_t jobsRunning,
                    std::uint64_t operationsQueued, std::uint64_t operationsRunning);

std::string toJson();
void logSnapshot();

} // namespace yt::metrics

#endif // YT_CONVERTER_METRICS_H
