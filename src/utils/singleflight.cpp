#include "singleflight.h"

namespace yt::singleflight {

Membership Group::join(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = inflight_.find(key);
    if (it != inflight_.end()) {
        return Membership{it->second, false};
    }
    auto flight = std::make_shared<Flight>();
    inflight_.emplace(key, flight);
    return Membership{flight, true};
}

void Group::succeed(const std::string& key, const std::shared_ptr<Flight>& flight) {
    {
        std::lock_guard<std::mutex> lock(flight->mu);
        flight->ok = true;
        flight->done = true;
    }
    flight->cv.notify_all();
    std::lock_guard<std::mutex> lock(mu_);
    inflight_.erase(key);
}

void Group::fail(const std::string& key, const std::shared_ptr<Flight>& flight, int errorCode,
                 std::string error) {
    {
        std::lock_guard<std::mutex> lock(flight->mu);
        flight->ok = false;
        flight->error_code = errorCode;
        flight->error = std::move(error);
        flight->done = true;
    }
    flight->cv.notify_all();
    std::lock_guard<std::mutex> lock(mu_);
    inflight_.erase(key);
}

void Group::wait(const std::shared_ptr<Flight>& flight) {
    std::unique_lock<std::mutex> lock(flight->mu);
    flight->cv.wait(lock, [&flight] { return flight->done; });
}

} // namespace yt::singleflight
