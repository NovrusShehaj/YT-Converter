#ifndef YT_CONVERTER_FILE_LOCK_H
#define YT_CONVERTER_FILE_LOCK_H

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>

namespace yt::fs_lock {

enum class Mode { Shared, Exclusive };

// RAII advisory lock on a lock file, valid across threads and processes. POSIX uses flock() on a
// close-on-exec descriptor (so spawned children never inherit a lock); Windows uses LockFileEx.
// The kernel releases the lock when the owning process dies, so no stale-lock recovery is needed.
class FileLock {
  public:
    FileLock() = default;
    FileLock(FileLock&& other) noexcept;
    FileLock& operator=(FileLock&& other) noexcept;
    ~FileLock();

    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

    // Blocks until the lock is held. Throws yt::Error(Internal) if the file cannot be opened.
    static FileLock acquire(const std::filesystem::path& path, Mode mode);
    // Returns an unlocked FileLock (held() == false) if another holder conflicts.
    static FileLock tryAcquire(const std::filesystem::path& path, Mode mode);
    // Polls until held, `cancel` is set, or `timeout` passes. Returns an unlocked FileLock on
    // cancellation or timeout, so waiting never outlives the caller's interest.
    static FileLock acquireCancelable(const std::filesystem::path& path, Mode mode,
                                      const std::shared_ptr<std::atomic<bool>>& cancel,
                                      std::chrono::milliseconds timeout);

    bool held() const noexcept { return handle_ != kInvalid; }
    // Converts an exclusive lock to shared. Callers must exclude competing lockers (for example
    // by holding the cache metadata lock), because POSIX conversion is not atomic.
    void downgrade();
    void release() noexcept;

  private:
#ifdef _WIN32
    using Handle = void*;
    static constexpr Handle kInvalid = nullptr;
#else
    using Handle = int;
    static constexpr Handle kInvalid = -1;
#endif
    explicit FileLock(Handle handle) : handle_(handle) {}
    static FileLock lock(const std::filesystem::path& path, Mode mode, bool wait);

    Handle handle_ = kInvalid;
};

} // namespace yt::fs_lock

#endif // YT_CONVERTER_FILE_LOCK_H
