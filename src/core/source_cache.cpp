#include "source_cache.h"
#include "error.h"
#include "logger.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <map>
#include <random>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace yt::cache {
namespace {

namespace fs = std::filesystem;

constexpr const char* kLeaseFile = ".lease";

std::string randomHex() {
    std::random_device device;
    std::mt19937_64 rng((static_cast<std::uint64_t>(device()) << 32) ^ device());
    std::ostringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << rng();
    return ss.str();
}

std::string stampText(std::int64_t stamp) {
    std::ostringstream ss;
    ss << std::setw(20) << std::setfill('0') << stamp;
    return ss.str();
}

void createPrivateDir(const fs::path& path) {
    fs::create_directories(path);
#ifndef _WIN32
    std::error_code ec;
    fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace, ec);
#endif
}

struct EntryName {
    std::string prefix; // "gen" or "inc"
    std::int64_t stamp = 0;
    std::string suffix;
};

// Parses "gen-<20 digit stamp>-<16 hex>" or "inc-...".
std::optional<EntryName> parseEntry(const std::string& name) {
    if (name.size() != 3 + 1 + 20 + 1 + 16) {
        return std::nullopt;
    }
    EntryName entry;
    entry.prefix = name.substr(0, 3);
    if ((entry.prefix != "gen" && entry.prefix != "inc") || name[3] != '-' || name[24] != '-') {
        return std::nullopt;
    }
    const std::string digits = name.substr(4, 20);
    if (!std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    entry.stamp = std::stoll(digits);
    entry.suffix = name.substr(25);
    return entry;
}

bool isFresh(std::int64_t stamp, int ttlSec) {
    if (ttlSec <= 0) {
        return false;
    }
    const std::int64_t age = std::max<std::int64_t>(0, nowStamp() - stamp);
    return age < static_cast<std::int64_t>(ttlSec) * 1000000000LL;
}

std::uintmax_t sizeOrZero(const fs::path& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    return ec ? 0 : size;
}

// Deletes a generation or incoming directory only if no one holds its lease.
bool removeIfUnleased(const fs::path& dir) {
    fs_lock::FileLock exclusive =
        fs_lock::FileLock::tryAcquire(dir / kLeaseFile, fs_lock::Mode::Exclusive);
    if (!exclusive.held()) {
        return false;
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    return !ec;
}

bool isUnleased(const fs::path& dir) {
    fs_lock::FileLock exclusive =
        fs_lock::FileLock::tryAcquire(dir / kLeaseFile, fs_lock::Mode::Exclusive);
    return exclusive.held();
}

void removeStalePartials(const fs::path& outputRoot, int maxAgeSec) {
    if (maxAgeSec <= 0) {
        return;
    }
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    for (const auto& entry : fs::directory_iterator(outputRoot, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.size() < 9 || name[0] != '.' || entry.path().extension() != ".partial") {
            continue;
        }
        std::error_code timeEc;
        const auto written = fs::last_write_time(entry.path(), timeEc);
        if (!timeEc && now - written > std::chrono::seconds(maxAgeSec)) {
            std::error_code removeEc;
            fs::remove(entry.path(), removeEc);
        }
    }
}

} // namespace

std::int64_t nowStamp() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

fs::path findSourceFile(const fs::path& directory) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        std::error_code typeEc;
        if (!entry.is_regular_file(typeEc)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        // Only "source.<ext>": yt-dlp's format-specific pieces ("source.f140.m4a") and
        // temporaries ("source.mp4.part", ".ytdl") are never treated as the finished download.
        if (name.rfind("source.", 0) != 0) {
            continue;
        }
        const std::string ext = name.substr(7);
        if (ext.empty() || ext.find('.') != std::string::npos || ext == "part" || ext == "ytdl" ||
            ext == "temp") {
            continue;
        }
        return entry.path();
    }
    return {};
}

Incoming::Incoming(Incoming&& other) noexcept
    : lock_(std::move(other.lock_)), dir_(std::move(other.dir_)), suffix_(std::move(other.suffix_)),
      stamp_(other.stamp_) {
    other.dir_.clear();
}

Incoming& Incoming::operator=(Incoming&& other) noexcept {
    if (this != &other) {
        discard();
        lock_ = std::move(other.lock_);
        dir_ = std::move(other.dir_);
        suffix_ = std::move(other.suffix_);
        stamp_ = other.stamp_;
        other.dir_.clear();
    }
    return *this;
}

Incoming::~Incoming() {
    discard();
}

void Incoming::discard() noexcept {
    if (!dir_.empty()) {
        // Only this operation's own unique directory is removed, while its lock is still held.
        std::error_code ec;
        fs::remove_all(dir_, ec);
        dir_.clear();
    }
    lock_.release();
}

SourceCache::SourceCache(fs::path outputRoot)
    : outputRoot_(std::move(outputRoot)), root_(outputRoot_ / "cache" / "src") {}

fs_lock::FileLock SourceCache::metadataLock() {
    createPrivateDir(root_);
    return fs_lock::FileLock::acquire(root_ / ".lock", fs_lock::Mode::Exclusive);
}

std::optional<Lease> SourceCache::acquire(const std::string& videoId, const std::string& kind,
                                          const Limits& limits, std::int64_t minStamp) {
    if (limits.ttl_sec <= 0) {
        return std::nullopt;
    }
    const fs::path kindDir = root_ / videoId / kind;
    auto guard = metadataLock();
    std::vector<std::pair<EntryName, fs::path>> generations;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(kindDir, ec)) {
        const auto parsed = parseEntry(entry.path().filename().string());
        if (parsed && parsed->prefix == "gen") {
            generations.emplace_back(*parsed, entry.path());
        }
    }
    std::sort(generations.begin(), generations.end(), [](const auto& left, const auto& right) {
        return left.first.stamp > right.first.stamp;
    });
    for (const auto& [name, dir] : generations) {
        if (name.stamp < minStamp || !isFresh(name.stamp, limits.ttl_sec)) {
            continue;
        }
        const fs::path source = findSourceFile(dir);
        const std::uintmax_t bytes = source.empty() ? 0 : sizeOrZero(source);
        if (bytes == 0) {
            continue;
        }
        fs_lock::FileLock shared =
            fs_lock::FileLock::tryAcquire(dir / kLeaseFile, fs_lock::Mode::Shared);
        if (!shared.held()) {
            continue;
        }
        Lease lease;
        lease.lock_ = std::move(shared);
        lease.source_ = source;
        lease.generation_ = dir.filename().string();
        lease.stamp_ = name.stamp;
        lease.bytes_ = bytes;
        return lease;
    }
    return std::nullopt;
}

Incoming SourceCache::beginIncoming(const std::string& videoId, const std::string& kind) {
    const fs::path kindDir = root_ / videoId / kind;
    auto guard = metadataLock();
    Incoming incoming;
    incoming.stamp_ = nowStamp();
    incoming.suffix_ = randomHex();
    const fs::path dir = kindDir / ("inc-" + stampText(incoming.stamp_) + "-" + incoming.suffix_);
    createPrivateDir(dir);
    // Locked before the metadata lock is released, so maintain() never sees it unlocked.
    incoming.lock_ = fs_lock::FileLock::acquire(dir / kLeaseFile, fs_lock::Mode::Exclusive);
    incoming.dir_ = dir;
    return incoming;
}

Lease SourceCache::publish(Incoming&& incoming, const fs::path& producedFile) {
    Incoming owned = std::move(incoming);
    const std::uintmax_t bytes = sizeOrZero(producedFile);
    if (bytes == 0 || producedFile.parent_path() != owned.dir_) {
        throw Error(ErrorCode::DownloadFailed, "yt-dlp completed without creating a source file");
    }
    const fs::path genDir =
        owned.dir_.parent_path() / ("gen-" + stampText(owned.stamp_) + "-" + owned.suffix_);
    auto guard = metadataLock();
    fs::rename(owned.dir_, genDir);
    Lease lease;
    // The exclusive download lock becomes this reader's shared lease. The metadata lock excludes
    // every other locker while the POSIX conversion briefly drops the lock.
    owned.lock_.downgrade();
    lease.lock_ = std::move(owned.lock_);
    owned.dir_.clear();
    lease.source_ = genDir / producedFile.filename();
    lease.generation_ = genDir.filename().string();
    lease.stamp_ = owned.stamp_;
    lease.bytes_ = bytes;
    return lease;
}

Stats SourceCache::maintain(const Limits& limits) {
    Stats stats;
    removeStalePartials(outputRoot_, limits.partial_max_age_sec);

    struct Generation {
        fs::path dir;
        std::int64_t stamp = 0;
        std::uintmax_t bytes = 0;
        bool current = false;
    };

    auto guard = metadataLock();
    std::vector<Generation> keep;
    std::error_code ec;
    for (const auto& videoDir : fs::directory_iterator(root_, ec)) {
        std::error_code typeEc;
        if (!videoDir.is_directory(typeEc)) {
            continue;
        }
        std::error_code kindEc;
        for (const auto& kindDir : fs::directory_iterator(videoDir.path(), kindEc)) {
            if (!kindDir.is_directory(typeEc)) {
                continue;
            }
            std::vector<Generation> generations;
            std::error_code entryEc;
            for (const auto& entry : fs::directory_iterator(kindDir.path(), entryEc)) {
                const std::string name = entry.path().filename().string();
                const auto parsed = parseEntry(name);
                if (!parsed) {
                    // Pre-generation layout ("source.<ext>" and "incoming/") from older builds.
                    if (name == "incoming" || name.rfind("source.", 0) == 0) {
                        std::error_code removeEc;
                        fs::remove_all(entry.path(), removeEc);
                        ++stats.removed_entries;
                    }
                    continue;
                }
                if (parsed->prefix == "inc") {
                    // A live download holds its lock; a dead process's directory is unlocked.
                    if (removeIfUnleased(entry.path())) {
                        ++stats.removed_entries;
                    }
                    continue;
                }
                Generation generation;
                generation.dir = entry.path();
                generation.stamp = parsed->stamp;
                const fs::path source = findSourceFile(entry.path());
                generation.bytes = source.empty() ? 0 : sizeOrZero(source);
                generations.push_back(std::move(generation));
            }
            // Only the newest valid generation of each video/kind stays current.
            Generation* current = nullptr;
            for (auto& generation : generations) {
                if (generation.bytes > 0 &&
                    (current == nullptr || generation.stamp > current->stamp)) {
                    current = &generation;
                }
            }
            if (current != nullptr && isFresh(current->stamp, limits.ttl_sec)) {
                current->current = true;
            }
            for (auto& generation : generations) {
                if (generation.current) {
                    keep.push_back(generation);
                } else if (removeIfUnleased(generation.dir)) {
                    ++stats.removed_entries;
                } else {
                    ++stats.deferred_entries;
                    ++stats.leased_entries;
                    stats.leased_bytes += generation.bytes;
                }
            }
        }
    }

    // Split current generations into leased (working) and retained, then enforce the cap by
    // removing unleased generations, oldest first.
    std::vector<Generation> retained;
    for (auto& generation : keep) {
        if (isUnleased(generation.dir)) {
            retained.push_back(generation);
            stats.retained_bytes += generation.bytes;
        } else {
            ++stats.leased_entries;
            stats.leased_bytes += generation.bytes;
        }
    }
    std::sort(
        retained.begin(), retained.end(),
        [](const Generation& left, const Generation& right) { return left.stamp < right.stamp; });
    const auto cap = static_cast<std::uint64_t>(std::max<std::int64_t>(0, limits.max_bytes));
    for (const auto& generation : retained) {
        if (limits.max_bytes <= 0 || stats.retained_bytes + stats.leased_bytes <= cap) {
            ++stats.retained_entries;
            continue;
        }
        if (removeIfUnleased(generation.dir)) {
            ++stats.removed_entries;
            stats.retained_bytes -= std::min<std::uint64_t>(stats.retained_bytes, generation.bytes);
        } else {
            // Leased between the scan and now (cannot happen under the metadata lock, but stay
            // safe).
            ++stats.deferred_entries;
            ++stats.retained_entries;
        }
    }
    stats.pressure = limits.max_bytes > 0 && stats.retained_bytes + stats.leased_bytes > cap;

    // Drop empty video/kind directories.
    std::error_code pruneEc;
    for (const auto& videoDir : fs::directory_iterator(root_, pruneEc)) {
        std::error_code typeEc;
        if (!videoDir.is_directory(typeEc)) {
            continue;
        }
        std::error_code kindEc;
        for (const auto& kindDir : fs::directory_iterator(videoDir.path(), kindEc)) {
            std::error_code removeEc;
            if (kindDir.is_directory(typeEc) && fs::is_empty(kindDir.path(), removeEc)) {
                fs::remove(kindDir.path(), removeEc);
            }
        }
        std::error_code removeEc;
        if (fs::is_empty(videoDir.path(), removeEc)) {
            fs::remove(videoDir.path(), removeEc);
        }
    }
    if (stats.pressure) {
        std::ostringstream ss;
        ss << "Source cache over its cap while sources are in use: retained_bytes="
           << stats.retained_bytes << " leased_bytes=" << stats.leased_bytes
           << " max_bytes=" << limits.max_bytes;
        yt::logger::Logger::getInstance().warning(ss.str());
    }
    return stats;
}

} // namespace yt::cache
