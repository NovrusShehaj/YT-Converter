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
};

Counters& global();
void recordStart();
void recordSuccess(std::uint64_t bytes);
void recordFailure();
std::string toJson();
void logSnapshot();

} // namespace yt::metrics

#endif // YT_CONVERTER_METRICS_H
