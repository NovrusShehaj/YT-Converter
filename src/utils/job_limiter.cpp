#include "job_limiter.h"

#include <algorithm>

namespace yt {

JobLimiter::Slot::Slot(JobLimiter* limiter) : limiter_(limiter) {}

JobLimiter::Slot::Slot(Slot&& other) noexcept : limiter_(other.limiter_) {
    other.limiter_ = nullptr;
}

JobLimiter::Slot& JobLimiter::Slot::operator=(Slot&& other) noexcept {
    if (this != &other) {
        release();
        limiter_ = other.limiter_;
        other.limiter_ = nullptr;
    }
    return *this;
}

JobLimiter::Slot::~Slot() { release(); }

void JobLimiter::Slot::release() {
    if (limiter_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(limiter_->mutex_);
    if (limiter_->current_ > 0) {
        --limiter_->current_;
    }
    limiter_ = nullptr;
}

JobLimiter::JobLimiter(int max_concurrent) : max_(std::max(1, max_concurrent)) {}

std::optional<JobLimiter::Slot> JobLimiter::tryAcquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_ >= max_) {
        return std::nullopt;
    }
    ++current_;
    return Slot(this);
}

} // namespace yt
