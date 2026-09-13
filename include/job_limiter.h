#ifndef YT_CONVERTER_JOB_LIMITER_H
#define YT_CONVERTER_JOB_LIMITER_H

#include <mutex>
#include <optional>

namespace yt {

class JobLimiter {
public:
    class Slot {
    public:
        Slot() = default;
        explicit Slot(JobLimiter* limiter);
        Slot(Slot&& other) noexcept;
        Slot& operator=(Slot&& other) noexcept;
        ~Slot();

        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;

    private:
        void release();
        JobLimiter* limiter_ = nullptr;
    };

    explicit JobLimiter(int max_concurrent);
    std::optional<Slot> tryAcquire();

private:
    friend class Slot;
    int max_;
    int current_ = 0;
    std::mutex mutex_;
};

} // namespace yt

#endif // YT_CONVERTER_JOB_LIMITER_H
