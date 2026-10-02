#ifndef YT_CONVERTER_SINGLEFLIGHT_H
#define YT_CONVERTER_SINGLEFLIGHT_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace yt::singleflight {

// One shared execution of keyed work. Subscribers register an interest token (their own
// cancellation flag); the work is abandoned only when every subscriber has canceled.
template <typename Result> struct Flight {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    bool abandoned = false;
    int error_code = 0;
    std::string error;
    std::optional<Result> value;
    std::int64_t started_stamp = 0;
    // A null token means a subscriber that cannot cancel (it stays interested).
    std::vector<std::shared_ptr<std::atomic<bool>>> interested;
};

template <typename Result> struct Membership {
    std::shared_ptr<Flight<Result>> flight;
    bool is_leader = false;
};

// Lock order: Group::mu_ may be held while taking a Flight::mu, never the reverse. Neither lock
// is held while running work or while a caller blocks.
template <typename Result> class Group {
  public:
    using FlightPtr = std::shared_ptr<Flight<Result>>;

    Group() = default;
    Group(const Group&) = delete;
    Group& operator=(const Group&) = delete;

    // Joins the in-flight work for `key`, or becomes its leader. Work that started before
    // `notBefore` (or that every previous subscriber abandoned) is never joined; a new flight
    // replaces it in the table and the old one finishes on its own.
    Membership<Result> join(const std::string& key, std::shared_ptr<std::atomic<bool>> token,
                            std::int64_t notBefore, std::int64_t now) {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = inflight_.find(key);
        if (it != inflight_.end()) {
            const FlightPtr& existing = it->second;
            std::lock_guard<std::mutex> flightLock(existing->mu);
            if (!existing->done && !existing->abandoned && !allCanceledLocked(*existing) &&
                existing->started_stamp >= notBefore) {
                existing->interested.push_back(std::move(token));
                return Membership<Result>{existing, false};
            }
        }
        auto flight = std::make_shared<Flight<Result>>();
        flight->started_stamp = now;
        flight->interested.push_back(std::move(token));
        inflight_[key] = flight;
        return Membership<Result>{flight, true};
    }

    // True once every subscriber has canceled. Latches, so a later joiner starts new work
    // instead of attaching to work that is being torn down.
    bool abandoned(const FlightPtr& flight) {
        std::lock_guard<std::mutex> lock(flight->mu);
        if (!flight->abandoned && allCanceledLocked(*flight)) {
            flight->abandoned = true;
        }
        return flight->abandoned;
    }

    void succeed(const std::string& key, const FlightPtr& flight, Result value) {
        {
            std::lock_guard<std::mutex> lock(flight->mu);
            flight->value = std::move(value);
            flight->done = true;
        }
        finish(key, flight);
    }

    void fail(const std::string& key, const FlightPtr& flight, int errorCode, std::string error) {
        {
            std::lock_guard<std::mutex> lock(flight->mu);
            flight->error_code = errorCode;
            flight->error = std::move(error);
            flight->done = true;
        }
        finish(key, flight);
    }

    // Waits for the flight or for `token` to be set. Returns false when canceled; the waiter's
    // canceled token then counts as lost interest. Waits use a bounded poll interval so a
    // canceled waiter returns promptly even if no notification arrives.
    bool wait(const FlightPtr& flight, const std::shared_ptr<std::atomic<bool>>& token) {
        std::unique_lock<std::mutex> lock(flight->mu);
        while (!flight->done) {
            if (token && token->load()) {
                return false;
            }
            flight->cv.wait_for(lock, std::chrono::milliseconds(50));
        }
        return true;
    }

    std::size_t sizeForTests() {
        std::lock_guard<std::mutex> lock(mu_);
        return inflight_.size();
    }

  private:
    static bool allCanceledLocked(const Flight<Result>& flight) {
        if (flight.interested.empty()) {
            return false;
        }
        for (const auto& token : flight.interested) {
            if (!token || !token->load()) {
                return false;
            }
        }
        return true;
    }

    void finish(const std::string& key, const FlightPtr& flight) {
        flight->cv.notify_all();
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = inflight_.find(key);
        // Never erase a newer flight that replaced this one.
        if (it != inflight_.end() && it->second == flight) {
            inflight_.erase(it);
        }
    }

    std::mutex mu_;
    std::map<std::string, FlightPtr> inflight_;
};

} // namespace yt::singleflight

#endif // YT_CONVERTER_SINGLEFLIGHT_H
