#ifndef YT_CONVERTER_SOURCE_CACHE_H
#define YT_CONVERTER_SOURCE_CACHE_H

#include "file_lock.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace yt::cache {

// Downloaded sources live in immutable generations:
//
//   <output>/cache/src/.lock                          cache metadata lock (held only briefly)
//   <output>/cache/src/<video>/<kind>/gen-<stamp>-<rand>/source.<ext>
//   <output>/cache/src/<video>/<kind>/gen-<stamp>-<rand>/.lease
//   <output>/cache/src/<video>/<kind>/inc-<stamp>-<rand>/   download in progress
//
// A reader holds a shared lock on the generation's .lease file for as long as it uses the source
// (through encode or MP4 publication). Lookup-and-lease, publication, and eviction all run under
// the cache metadata lock, and eviction only deletes a generation after taking an exclusive
// .lease lock without waiting, so a leased generation is never deleted, by this process or by
// another one sharing the output root. Locks are advisory flock()/LockFileEx locks released by
// the kernel when a process dies; a dead downloader's inc-* directory becomes removable as soon
// as its lock is gone. A refresh publishes a new generation and never modifies the old one.

struct Limits {
    // Retained-byte cap. <= 0 means unlimited.
    std::int64_t max_bytes = 0;
    // Generations older than this are not reused and become eligible for removal. <= 0 disables
    // reuse, so every generation is removed once no reader holds it.
    int ttl_sec = 0;
    // Hidden "*.partial" outputs in the output root older than this are removed (0 skips).
    int partial_max_age_sec = 0;
};

struct Stats {
    std::uint64_t retained_bytes = 0; // unleased generations kept after maintenance
    std::uint64_t leased_bytes = 0;   // generations in active use (bounded working bytes)
    std::uint64_t retained_entries = 0;
    std::uint64_t leased_entries = 0;
    std::uint64_t deferred_entries = 0; // eligible for removal, but leased: retried later
    std::uint64_t removed_entries = 0;
    // Retained plus leased bytes still exceed max_bytes because only leased entries remain.
    bool pressure = false;
};

// Shared read lease on one immutable generation. Releasing it (destruction) never deletes data;
// the next maintain() call reclaims retired or over-cap generations.
class Lease {
  public:
    Lease() = default;
    Lease(Lease&&) noexcept = default;
    Lease& operator=(Lease&&) noexcept = default;

    const std::filesystem::path& source() const { return source_; }
    const std::string& generation() const { return generation_; }
    // Download start time, nanoseconds since the Unix epoch.
    std::int64_t stamp() const { return stamp_; }
    std::uintmax_t bytes() const { return bytes_; }
    bool valid() const { return lock_.held(); }

  private:
    friend class SourceCache;
    fs_lock::FileLock lock_;
    std::filesystem::path source_;
    std::string generation_;
    std::int64_t stamp_ = 0;
    std::uintmax_t bytes_ = 0;
};

// A unique, exclusively locked download directory. Destruction removes it unless published.
class Incoming {
  public:
    Incoming() = default;
    Incoming(Incoming&& other) noexcept;
    Incoming& operator=(Incoming&& other) noexcept;
    ~Incoming();

    Incoming(const Incoming&) = delete;
    Incoming& operator=(const Incoming&) = delete;

    const std::filesystem::path& dir() const { return dir_; }
    std::int64_t stamp() const { return stamp_; }

  private:
    friend class SourceCache;
    void discard() noexcept;
    fs_lock::FileLock lock_;
    std::filesystem::path dir_;
    std::string suffix_;
    std::int64_t stamp_ = 0;
};

class SourceCache {
  public:
    explicit SourceCache(std::filesystem::path outputRoot);

    // Leases the newest valid generation of <video>/<kind> that is fresh under limits.ttl_sec and
    // whose download started at or after minStamp. Lookup and lease are one atomic step.
    std::optional<Lease> acquire(const std::string& videoId, const std::string& kind,
                                 const Limits& limits, std::int64_t minStamp = 0);

    // Creates a unique locked download directory for <video>/<kind>.
    Incoming beginIncoming(const std::string& videoId, const std::string& kind);

    // Verifies the downloaded file, publishes the directory as a new generation, and returns a
    // lease on it. Older generations become retired and are removed once unleased.
    Lease publish(Incoming&& incoming, const std::filesystem::path& producedFile);

    // Removes retired, expired, and over-cap generations that no one leases, plus abandoned
    // incoming directories and stale partial outputs. Never removes a leased generation.
    Stats maintain(const Limits& limits);

    const std::filesystem::path& root() const { return root_; }

  private:
    fs_lock::FileLock metadataLock();

    std::filesystem::path outputRoot_;
    std::filesystem::path root_;
};

// Nanoseconds since the Unix epoch (system clock, comparable across processes).
std::int64_t nowStamp();

// Finds "source.<ext>" in a download directory, ignoring yt-dlp temporaries.
std::filesystem::path findSourceFile(const std::filesystem::path& directory);

} // namespace yt::cache

#endif // YT_CONVERTER_SOURCE_CACHE_H
