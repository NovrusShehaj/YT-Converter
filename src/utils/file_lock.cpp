#include "file_lock.h"
#include "error.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace yt::fs_lock {

FileLock::FileLock(FileLock&& other) noexcept : handle_(other.handle_) {
    other.handle_ = kInvalid;
}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
    if (this != &other) {
        release();
        handle_ = other.handle_;
        other.handle_ = kInvalid;
    }
    return *this;
}

FileLock::~FileLock() {
    release();
}

FileLock FileLock::acquire(const std::filesystem::path& path, Mode mode) {
    return lock(path, mode, true);
}

FileLock FileLock::tryAcquire(const std::filesystem::path& path, Mode mode) {
    return lock(path, mode, false);
}

FileLock FileLock::acquireCancelable(const std::filesystem::path& path, Mode mode,
                                     const std::shared_ptr<std::atomic<bool>>& cancel,
                                     std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        FileLock held = lock(path, mode, false);
        if (held.held()) {
            return held;
        }
        if ((cancel && cancel->load()) || std::chrono::steady_clock::now() >= deadline) {
            return FileLock();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

#ifdef _WIN32

FileLock FileLock::lock(const std::filesystem::path& path, Mode mode, bool wait) {
    HANDLE handle = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw Error(ErrorCode::Internal, "Unable to open lock file " + path.string());
    }
    DWORD flags = mode == Mode::Exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    if (!wait) {
        flags |= LOCKFILE_FAIL_IMMEDIATELY;
    }
    OVERLAPPED overlapped{};
    if (!LockFileEx(handle, flags, 0, 1, 0, &overlapped)) {
        CloseHandle(handle);
        if (!wait) {
            return FileLock();
        }
        throw Error(ErrorCode::Internal, "Unable to lock " + path.string());
    }
    return FileLock(handle);
}

void FileLock::downgrade() {
    if (!held()) {
        return;
    }
    // LockFileEx cannot convert a lock in place, so unlock and relock shared. Callers hold the
    // cache metadata lock while doing this.
    OVERLAPPED overlapped{};
    UnlockFileEx(handle_, 0, 1, 0, &overlapped);
    OVERLAPPED shared{};
    if (!LockFileEx(handle_, 0, 0, 1, 0, &shared)) {
        throw Error(ErrorCode::Internal, "Unable to downgrade lock");
    }
}

void FileLock::release() noexcept {
    if (handle_ != kInvalid) {
        OVERLAPPED overlapped{};
        UnlockFileEx(handle_, 0, 1, 0, &overlapped);
        CloseHandle(handle_);
        handle_ = kInvalid;
    }
}

#else

FileLock FileLock::lock(const std::filesystem::path& path, Mode mode, bool wait) {
    int fd = -1;
    do {
        fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        throw Error(ErrorCode::Internal,
                    "Unable to open lock file " + path.string() + ": " + std::strerror(errno));
    }
    const int operation = (mode == Mode::Exclusive ? LOCK_EX : LOCK_SH) | (wait ? 0 : LOCK_NB);
    int rc = 0;
    do {
        rc = ::flock(fd, operation);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        const int error = errno;
        ::close(fd);
        if (!wait && error == EWOULDBLOCK) {
            return FileLock();
        }
        throw Error(ErrorCode::Internal,
                    "Unable to lock " + path.string() + ": " + std::strerror(error));
    }
    return FileLock(fd);
}

void FileLock::downgrade() {
    if (!held()) {
        return;
    }
    int rc = 0;
    do {
        rc = ::flock(handle_, LOCK_SH);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        throw Error(ErrorCode::Internal,
                    std::string("Unable to downgrade lock: ") + std::strerror(errno));
    }
}

void FileLock::release() noexcept {
    if (handle_ != kInvalid) {
        ::close(handle_); // closing the only descriptor releases the flock
        handle_ = kInvalid;
    }
}

#endif

} // namespace yt::fs_lock
