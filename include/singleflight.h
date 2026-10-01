#ifndef YT_CONVERTER_SINGLEFLIGHT_H
#define YT_CONVERTER_SINGLEFLIGHT_H

#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <shared_mutex>

namespace yt::singleflight {

struct InFlight {
    std::condition_variable cv;
    std::mutex mutex;
    std::optional<std::string> result; // job id that's doing the work
    std::string error;
    bool done = false;
};

class Group {
public:
    Group() = default;
    Group(const Group&) = delete;
    Group& operator=(const Group&) = delete;

    // Returns the leader job_id if this caller is the leader, empty if following
    std::optional<std::string> enter(const std::string& key);
    
    // Called by the leader when done
    void done(const std::string& key, const std::string& leaderJobId);
    
    // Wait for the leader to finish
    std::optional<std::string> waitFor(const std::string& key, const std::string& followerJobId);

private:
    mutable std::shared_mutex mutex_;
    std::map<std::string, InFlight> inflight_;
};

} // namespace yt::singleflight

#endif // YT_CONVERTER_SINGLEFLIGHT_H
