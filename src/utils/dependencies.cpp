#include "dependencies.h"
#include "error.h"
#include "metrics.h"
#include "process.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>

namespace yt::deps {
namespace {

std::string firstLine(const std::string& text) {
    const std::size_t newline = text.find('\n');
    std::string line = newline == std::string::npos ? text : text.substr(0, newline);
    while (!line.empty() &&
           (line.back() == '\r' || std::isspace(static_cast<unsigned char>(line.back())))) {
        line.pop_back();
    }
    return line;
}

std::string probeVersion(const std::string& binary, const std::string& flag) {
    yt::process::RunOptions options;
    options.timeout_ms = 10000;
    options.inherit_stderr = false;
    const auto result = yt::process::run({binary, flag}, options);
    if (result.not_found) {
        throw Error(ErrorCode::BinaryNotFound,
                    "Required tool not found: " + binary + ". " + installHint());
    }
    if (result.timed_out) {
        throw Error(ErrorCode::Timeout, "Timed out while checking " + binary);
    }
    if (result.exit_code != 0) {
        throw Error(ErrorCode::BinaryNotFound,
                    binary + " failed during preflight. " + installHint());
    }
    const std::string version =
        firstLine(result.stdout_text.empty() ? result.stderr_text : result.stdout_text);
    return version.empty() ? binary : version;
}

} // namespace

std::string installHint() {
#ifdef __APPLE__
    return "Install with: brew install yt-dlp ffmpeg";
#else
    return "Install with: sudo apt-get install ffmpeg && pip install yt-dlp";
#endif
}

PreflightResult checkTools(const Config& config) {
    PreflightResult result;
    try {
        result.yt_dlp_version = probeVersion(config.yt_dlp_path, "--version");
        result.ffmpeg_version = probeVersion(config.ffmpeg_path, "-version");
        result.ok = true;
    } catch (const Error& error) {
        result.ok = false;
        result.message = error.message();
    }
    return result;
}

void requireTools(const Config& config) {
    const PreflightResult result = checkTools(config);
    if (!result.ok) {
        throw Error(ErrorCode::BinaryNotFound, result.message);
    }
}

// Readiness cache. Entries are keyed by the effective tool configuration and stamped with the
// invalidation generation that was current when their probe started. Only successful results are
// cached, and only if no invalidation happened while the probe ran, so a stale success can never
// overwrite a newer missing-tool failure. Concurrent checks of one configuration share a probe.
// The mutex is never held while a probe runs.
namespace {

struct CacheEntry {
    PreflightResult result;
    std::chrono::steady_clock::time_point checked_at;
    std::uint64_t generation = 0;
};

struct Probe {
    std::uint64_t generation = 0;
    bool done = false;
    PreflightResult result;
};

std::mutex g_readyMutex;
std::condition_variable g_readyCv;
std::uint64_t g_generation = 0;
std::map<std::string, CacheEntry> g_readyCache;
std::map<std::string, std::shared_ptr<Probe>> g_probes;

std::string configKey(const Config& config) {
    return config.yt_dlp_path + '\n' + config.ffmpeg_path;
}

} // namespace

PreflightResult checkToolsCached(const Config& config) {
    const auto ttl = std::chrono::seconds(std::max(0, config.ready_ttl_sec));
    const std::string key = configKey(config);
    std::shared_ptr<Probe> probe;
    {
        std::unique_lock<std::mutex> lock(g_readyMutex);
        const auto cached = g_readyCache.find(key);
        if (cached != g_readyCache.end() && cached->second.generation == g_generation &&
            ttl.count() > 0 && std::chrono::steady_clock::now() - cached->second.checked_at < ttl) {
            yt::metrics::recordReadyCheck(false);
            return cached->second.result;
        }
        const auto running = g_probes.find(key);
        if (running != g_probes.end() && running->second->generation == g_generation) {
            // Join the probe already running for this configuration and generation.
            const auto shared = running->second;
            g_readyCv.wait(lock, [&shared] { return shared->done; });
            yt::metrics::recordReadyCheck(false);
            return shared->result;
        }
        probe = std::make_shared<Probe>();
        probe->generation = g_generation;
        g_probes[key] = probe;
    }

    yt::metrics::recordReadyCheck(true);
    // Bounded: each version probe has its own timeout (see probeVersion).
    PreflightResult result = checkTools(config);

    {
        std::lock_guard<std::mutex> lock(g_readyMutex);
        if (probe->generation == g_generation) {
            if (result.ok) {
                g_readyCache[key] =
                    CacheEntry{result, std::chrono::steady_clock::now(), probe->generation};
            } else {
                g_readyCache.erase(key);
            }
        }
        probe->result = result;
        probe->done = true;
        const auto mine = g_probes.find(key);
        if (mine != g_probes.end() && mine->second == probe) {
            g_probes.erase(mine);
        }
    }
    g_readyCv.notify_all();
    return result;
}

void invalidateReadiness() {
    {
        std::lock_guard<std::mutex> lock(g_readyMutex);
        ++g_generation;
        g_readyCache.clear();
    }
    g_readyCv.notify_all();
}

void clearReadyCacheForTests() {
    invalidateReadiness();
}

} // namespace yt::deps
