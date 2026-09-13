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

void recordSuccess(std::uint64_t bytes) {
    auto& counters = global();
    counters.jobs_succeeded.fetch_add(1);
    if (counters.jobs_active.load() > 0) {
        counters.jobs_active.fetch_sub(1);
    }
    counters.bytes_written.fetch_add(bytes);
}

void recordFailure() {
    auto& counters = global();
    counters.jobs_failed.fetch_add(1);
    if (counters.jobs_active.load() > 0) {
        counters.jobs_active.fetch_sub(1);
    }
}

std::string toJson() {
    const auto& counters = global();
    std::ostringstream ss;
    ss << "{\"jobs_started\":" << counters.jobs_started.load()
       << ",\"jobs_succeeded\":" << counters.jobs_succeeded.load()
       << ",\"jobs_failed\":" << counters.jobs_failed.load()
       << ",\"jobs_active\":" << counters.jobs_active.load()
       << ",\"bytes_written\":" << counters.bytes_written.load() << '}';
    return ss.str();
}

void logSnapshot() {
    yt::logger::Logger::getInstance().info("metrics " + toJson());
}

} // namespace yt::metrics
