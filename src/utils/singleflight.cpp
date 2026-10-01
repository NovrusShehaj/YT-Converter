#include "singleflight.h"

namespace yt::singleflight {

std::optional<std::string> Group::enter(const std::string& key) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto& entry = inflight_[key];
    if (entry.done) {
        return std::nullopt;
    }
    if (entry.result.has_value()) {
        return std::nullopt; // Following
    }
    std::string leaderJobId = ""; // Would be set by caller
    entry.result = leaderJobId;
    return leaderJobId;
}

void Group::done(const std::string& key, const std::string& leaderJobId) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = inflight_.find(key);
    if (it != inflight_.end()) {
        it->second.done = true;
        it->second.result = leaderJobId;
        it->second.cv.notify_all();
    }
}

std::optional<std::string> Group::waitFor(const std::string& key, const std::string& followerJobId) {
    std::unique_lock<std::mutex> lock(inflight_[key].mutex);
    inflight_[key].cv.wait(lock, [&] { return inflight_[key].done; });
    if (inflight_[key].error.empty()) {
        return inflight_[key].result;
    }
    return std::nullopt;
}

} // namespace yt::singleflight
