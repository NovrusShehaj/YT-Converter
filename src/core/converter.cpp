#include "converter.h"
#include "dependencies.h"
#include "error.h"
#include "file_lock.h"
#include "logger.h"
#include "metrics.h"
#include "process.h"
#include "singleflight.h"
#include "source_cache.h"
#include "validation.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace yt::converter {
namespace {

namespace fs = std::filesystem;

constexpr std::uintmax_t kMinFreeBytes = 50ull * 1024ull * 1024ull;

std::mutex g_freeSpaceMutex;
std::optional<std::uintmax_t> g_freeSpaceOverride;

// Result of one physical download, shared by every subscriber of its flight. The lease keeps the
// new generation alive until the last subscriber that needs it is done.
struct DownloadOutcome {
    std::shared_ptr<cache::Lease> lease;
    std::uint64_t download_ms = 0;
};

using DownloadGroup = singleflight::Group<DownloadOutcome>;

DownloadGroup& downloadFlights() {
    static DownloadGroup group;
    return group;
}

std::string randomSuffix() {
    std::random_device device;
    std::mt19937 rng(device());
    std::uniform_int_distribution<unsigned int> dist(0, 0xffffffffu);
    std::ostringstream ss;
    ss << std::hex << std::setw(8) << std::setfill('0') << dist(rng);
    return ss.str();
}

bool isSafeJobId(const std::string& id) {
    if (id.empty() || id.size() > 80) {
        return false;
    }
    for (char ch : id) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && ch != '_' && ch != '-') {
            return false;
        }
    }
    return true;
}

std::string makeJobId(const std::string& videoId, const std::string& requested) {
    if (!requested.empty()) {
        if (!isSafeJobId(requested)) {
            throw Error(ErrorCode::InvalidInput, "Job ID contains unsafe characters");
        }
        return requested;
    }
    return videoId + '-' + randomSuffix();
}

bool pathIsInside(const fs::path& root, const fs::path& candidate) {
    const fs::path normalRoot = fs::weakly_canonical(root);
    const fs::path normalCandidate = fs::weakly_canonical(candidate);
    const std::string rootStr = normalRoot.string();
    const std::string candStr = normalCandidate.string();
    if (candStr.size() < rootStr.size()) {
        return false;
    }
    if (candStr.compare(0, rootStr.size(), rootStr) != 0) {
        return false;
    }
    return candStr.size() == rootStr.size() || candStr[rootStr.size()] == '/' ||
           candStr[rootStr.size()] == '\\';
}

void createPrivateDir(const fs::path& path) {
    fs::create_directories(path);
#ifndef _WIN32
    fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace);
#endif
}

void removeIfExists(const fs::path& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
}

bool looksLikeDiskFull(const std::string& text) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("no space left") != std::string::npos ||
           lower.find("enospc") != std::string::npos ||
           lower.find("disk full") != std::string::npos;
}

void throwSpawnError(ErrorCode fallback, const std::string& tool,
                     const yt::process::RunResult& result) {
    if (result.canceled) {
        throw Error(ErrorCode::Canceled, tool + " canceled");
    }
    if (result.not_found) {
        // A cached "ready" must not survive a tool that can no longer be launched.
        yt::deps::invalidateReadiness();
        throw Error(ErrorCode::BinaryNotFound, "Required tool not found: " + tool);
    }
    if (result.timed_out) {
        throw Error(ErrorCode::Timeout, tool + " timed out");
    }
    if (looksLikeDiskFull(result.stderr_text)) {
        throw Error(ErrorCode::DiskFull, tool + " failed: disk full");
    }
    std::string detail = result.stderr_text.empty() ? result.stdout_text : result.stderr_text;
    if (detail.size() > 512) {
        detail.resize(512);
    }
    std::ostringstream ss;
    ss << tool << " failed with exit code " << result.exit_code;
    if (!detail.empty()) {
        ss << ": " << detail;
    }
    throw Error(fallback, ss.str());
}

void throwIfCanceled(const ConversionRequest& request) {
    if ((request.cancel && request.cancel->load()) || yt::process::shutdownRequested()) {
        throw Error(ErrorCode::Canceled, "Conversion canceled");
    }
}

std::string videoFormatSelector(int maxHeight) {
    std::ostringstream ss;
    ss << "bestvideo[height<=" << maxHeight
       << "][ext=mp4]+bestaudio[ext=m4a]/best[height<=" << maxHeight << "][ext=mp4]/mp4";
    return ss.str();
}

std::string audioFormatSelector() {
    return "ba[ext=m4a]/ba[ext=webm]/ba[ext=opus]/ba[acodec!=none]";
}

int parsePercent(const std::string& line) {
    const auto mark = line.find('%');
    if (mark == std::string::npos || mark == 0) {
        return -1;
    }
    std::size_t begin = mark;
    while (begin > 0 &&
           (std::isdigit(static_cast<unsigned char>(line[begin - 1])) || line[begin - 1] == '.')) {
        --begin;
    }
    try {
        return static_cast<int>(std::stod(line.substr(begin, mark - begin)));
    } catch (const std::exception&) {
        return -1;
    }
}

struct LineState {
    const ConversionRequest* request = nullptr;
    const char* stage = "download";
};

void onToolLine(const std::string& line, void* user) {
    auto* state = static_cast<LineState*>(user);
    if (state == nullptr || state->request == nullptr) {
        return;
    }
    if (state->request->show_progress && line.find("[download]") != std::string::npos) {
        std::cerr << line << '\n';
    }
    if (state->request->on_progress) {
        state->request->on_progress(state->stage, parsePercent(line));
    }
}

std::uintmax_t availableBytes(const fs::path& root) {
    std::lock_guard<std::mutex> lock(g_freeSpaceMutex);
    if (g_freeSpaceOverride.has_value()) {
        return *g_freeSpaceOverride;
    }
    return fs::space(root).available;
}

void ensureSpace(const fs::path& root, std::int64_t maxFileBytes) {
    const auto need =
        static_cast<std::uintmax_t>(std::max<std::int64_t>(0, maxFileBytes)) + kMinFreeBytes;
    if (availableBytes(root) < need) {
        throw Error(ErrorCode::DiskFull, "Not enough free disk space in the output directory");
    }
}

fs::path metadataCacheDir(const fs::path& outputRoot, const Config& config) {
    if (config.cache_dir.empty()) {
        return outputRoot / "cache" / "ytdlp";
    }
    fs::path configured(config.cache_dir);
    if (configured.is_absolute()) {
        return configured;
    }
    return outputRoot / configured;
}

cache::Limits cacheLimits(const Config& config) {
    cache::Limits limits;
    limits.max_bytes = config.source_cache_max_bytes;
    limits.ttl_sec = config.source_cache_ttl_sec;
    // An active encode is bounded by convert_timeout_sec, so older partials belong to a crashed
    // process.
    limits.partial_max_age_sec = config.convert_timeout_sec + 3600;
    return limits;
}

// A unique temporary name in the output directory, so concurrent writers never share a partial
// file and the final rename stays on one filesystem.
fs::path uniquePartialPath(const fs::path& finalPath) {
    return finalPath.parent_path() / ("." + finalPath.filename().string() + "." + randomSuffix() +
                                      randomSuffix() + ".partial");
}

// ffmpeg cannot infer a muxer from a ".partial" name, so the container is always explicit.
const char* outputMuxer(const std::string& format) {
    if (format == "mp3") {
        return "mp3";
    }
    if (format == "wav") {
        return "wav";
    }
    return "mp4";
}

void linkOrCopy(const fs::path& source, const fs::path& partial) {
    std::error_code ec;
    fs::create_hard_link(source, partial, ec);
    if (ec) {
        fs::copy_file(source, partial, fs::copy_options::overwrite_existing);
    }
}

std::uintmax_t usableSize(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return 0;
    }
    const auto size = fs::file_size(path, ec);
    return ec ? 0 : size;
}

std::uint64_t elapsedMs(std::chrono::steady_clock::time_point started) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();
    return static_cast<std::uint64_t>(std::max<std::int64_t>(1, ms));
}

// Downloads into a unique incoming directory and publishes it as a new leased generation. The
// child is canceled only when every subscriber of the flight has canceled.
DownloadOutcome downloadSource(const ConversionRequest& request, const validation::VideoRef& video,
                               const fs::path& outputRoot, cache::SourceCache& sourceCache,
                               const std::string& kind, const std::function<bool()>& abandoned) {
    const bool audioOnly = request.format == "mp3" || request.format == "wav";
    ensureSpace(outputRoot, request.config.max_filesize_bytes);
    cache::Incoming incoming = sourceCache.beginIncoming(video.id, kind);
    const fs::path outputTemplate = incoming.dir() / "source.%(ext)s";
    const fs::path ytCache = metadataCacheDir(outputRoot, request.config);
    createPrivateDir(ytCache);

    std::vector<std::string> argv{
        request.config.yt_dlp_path,
        "--no-playlist",
        "--newline",
        "--socket-timeout",
        std::to_string(request.config.socket_timeout_sec),
        "--max-filesize",
        request.config.max_filesize,
        "--retries",
        std::to_string(request.config.retries),
        "--concurrent-fragments",
        std::to_string(request.config.concurrent_fragments),
        "--fragment-retries",
        std::to_string(request.config.fragment_retries),
        "--retry-sleep",
        "linear=1::2",
        "--no-mtime",
        "--cache-dir",
        ytCache.string(),
    };
    if (!audioOnly) {
        argv.insert(argv.end(), {"--merge-output-format", "mp4"});
    }
    argv.insert(argv.end(),
                {"-f",
                 audioOnly ? audioFormatSelector() : videoFormatSelector(request.config.max_height),
                 "-o", outputTemplate.string(), video.canonical_url});

    auto& logger = yt::logger::Logger::getInstance();
    logger.info("Downloading video " + video.id);
    if (request.config.log_urls) {
        logger.debug("Download URL: " + video.canonical_url);
    }
    if (request.on_progress) {
        request.on_progress("download", -1);
    }

    LineState lines{&request, "download"};
    yt::process::RunOptions options;
    options.timeout_ms = request.config.download_timeout_sec * 1000;
    options.inherit_stderr = false;
    options.on_line = onToolLine;
    options.on_line_user = &lines;
    options.should_cancel = abandoned;

    const auto started = std::chrono::steady_clock::now();
    const auto result = yt::process::run(argv, options);
    const std::uint64_t downloadMs = elapsedMs(started);
    // One measurement per physical download attempt. Partial bytes of a failed download are
    // unknown, so failures add zero bytes.
    if (result.exit_code != 0 || result.not_found || result.timed_out || result.canceled) {
        yt::metrics::recordDownload(false, downloadMs, 0);
        throwSpawnError(ErrorCode::DownloadFailed, request.config.yt_dlp_path, result);
    }
    const fs::path produced = cache::findSourceFile(incoming.dir());
    if (produced.empty() || usableSize(produced) == 0) {
        yt::metrics::recordDownload(false, downloadMs, 0);
        throw Error(ErrorCode::DownloadFailed, "yt-dlp completed without creating a source file");
    }
    DownloadOutcome outcome;
    outcome.lease =
        std::make_shared<cache::Lease>(sourceCache.publish(std::move(incoming), produced));
    outcome.download_ms = downloadMs;
    yt::metrics::recordDownload(true, downloadMs, outcome.lease->bytes());
    return outcome;
}

struct SourceAcquisition {
    std::shared_ptr<cache::Lease> lease;
    std::uint64_t download_ms = 0;
    bool cache_hit = false;
    bool shared = false;
    bool downloaded = false;
};

// Returns a lease on a usable source generation: a fresh cached one when policy allows, else the
// result of a (possibly shared) download. The lease is held until the caller is done with it.
SourceAcquisition acquireSource(const ConversionRequest& request, const validation::VideoRef& video,
                                const fs::path& outputRoot, cache::SourceCache& sourceCache,
                                const std::string& kind) {
    const cache::Limits limits = cacheLimits(request.config);
    // A refresh only accepts a generation whose download started after the request was admitted.
    const std::int64_t minStamp = request.refresh ? request.admitted_at : 0;
    SourceAcquisition acquired;
    if (auto lease = sourceCache.acquire(video.id, kind, limits, minStamp)) {
        acquired.lease = std::make_shared<cache::Lease>(std::move(*lease));
        acquired.cache_hit = true;
        yt::metrics::recordSourceCacheHit();
        yt::logger::Logger::getInstance().info("Reusing cached source for " + video.id);
        return acquired;
    }

    const std::string key = outputRoot.string() + "\n" + video.id + "\n" + kind;
    auto& flights = downloadFlights();
    const auto membership = flights.join(key, request.cancel, minStamp, cache::nowStamp());
    if (!membership.is_leader) {
        if (!flights.wait(membership.flight, request.cancel)) {
            throw Error(ErrorCode::Canceled, "Conversion canceled while waiting for a download");
        }
        const auto& flight = *membership.flight;
        if (!flight.value) {
            const auto code = flight.error_code == 0 ? ErrorCode::Internal
                                                     : static_cast<ErrorCode>(flight.error_code);
            throw Error(code, flight.error.empty() ? "download failed" : flight.error);
        }
        acquired.lease = flight.value->lease;
        acquired.download_ms = flight.value->download_ms;
        acquired.shared = true;
        yt::metrics::recordSharedDownload();
        return acquired;
    }

    const auto flight = membership.flight;
    try {
        DownloadOutcome outcome =
            downloadSource(request, video, outputRoot, sourceCache, kind,
                           [flight] { return downloadFlights().abandoned(flight); });
        acquired.lease = outcome.lease;
        acquired.download_ms = outcome.download_ms;
        acquired.downloaded = true;
        flights.succeed(key, flight, std::move(outcome));
    } catch (const Error& error) {
        flights.fail(key, flight, static_cast<int>(error.code()), error.message());
        throw;
    } catch (const std::exception& error) {
        flights.fail(key, flight, static_cast<int>(ErrorCode::Internal), error.what());
        throw;
    } catch (...) {
        flights.fail(key, flight, static_cast<int>(ErrorCode::Internal), "download failed");
        throw;
    }
    // Reclaim retired or over-cap generations now that the new one is leased.
    try {
        sourceCache.maintain(limits);
    } catch (const std::exception& error) {
        yt::logger::Logger::getInstance().warning(std::string("Source cache maintenance failed: ") +
                                                  error.what());
    }
    return acquired;
}

void convertMedia(const ConversionRequest& request, const fs::path& source,
                  const fs::path& partial) {
    std::vector<std::string> argv{
        request.config.ffmpeg_path,
        "-y",
        "-nostdin",
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        source.string(),
    };
    if (request.format == "mp3") {
        argv.insert(argv.end(),
                    {"-vn", "-ar", "44100", "-ac", "2", "-b:a", "192k", "-codec:a", "libmp3lame"});
    } else if (request.format == "wav") {
        argv.insert(argv.end(), {"-vn", "-acodec", "pcm_s16le", "-ar", "44100", "-ac", "2"});
    } else {
        argv.insert(argv.end(), {"-c", "copy"});
    }
    argv.insert(argv.end(), {"-f", outputMuxer(request.format), partial.string()});

    auto& logger = yt::logger::Logger::getInstance();
    logger.info("Converting to " + request.format);
    if (request.on_progress) {
        request.on_progress("convert", -1);
    }

    LineState lines{&request, "convert"};
    yt::process::RunOptions options;
    options.timeout_ms = request.config.convert_timeout_sec * 1000;
    options.inherit_stderr = false;
    options.on_line = onToolLine;
    options.on_line_user = &lines;
    options.cancel = request.cancel;
    const auto result = yt::process::run(argv, options);
    if (result.exit_code != 0 || result.not_found || result.timed_out || result.canceled) {
        throwSpawnError(ErrorCode::ConversionFailed, request.config.ffmpeg_path, result);
    }
}

void logCompletion(const ConversionResult& result, const std::string& format) {
    std::ostringstream ss;
    ss << "Conversion completed video_id=" << result.video_id << " format=" << format
       << " reused=" << (result.reused ? "true" : "false") << " download_ms=" << result.download_ms
       << " convert_ms=" << result.convert_ms << " source_bytes=" << result.source_bytes
       << " output_bytes=" << result.output_bytes;
    yt::logger::Logger::getInstance().info(ss.str());
}

// Per-output publication record, written under the writer lock after each publication. It lets
// a writer detect that a newer publication already satisfied its request.
struct OutputRecord {
    std::int64_t generation_stamp = 0; // download start of the source generation used
    std::int64_t published_at = 0;     // when the output was renamed into place
};

fs::path outputRecordPath(const fs::path& outputRoot, const fs::path& finalPath) {
    return outputRoot / "cache" / "outputs" / (finalPath.filename().string() + ".meta");
}

std::optional<OutputRecord> readOutputRecord(const fs::path& path) {
    std::ifstream in(path);
    if (!in) {
        return std::nullopt;
    }
    OutputRecord record;
    std::string line;
    bool haveGeneration = false;
    bool havePublished = false;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        try {
            const std::int64_t value = std::stoll(line.substr(eq + 1));
            if (line.compare(0, eq, "generation_stamp") == 0) {
                record.generation_stamp = value;
                haveGeneration = true;
            } else if (line.compare(0, eq, "published_at") == 0) {
                record.published_at = value;
                havePublished = true;
            }
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    if (!haveGeneration || !havePublished) {
        return std::nullopt;
    }
    return record;
}

void writeOutputRecord(const fs::path& path, const OutputRecord& record) {
    createPrivateDir(path.parent_path());
    const fs::path temp = path.string() + "." + randomSuffix() + ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        out << "generation_stamp=" << record.generation_stamp << '\n'
            << "published_at=" << record.published_at << '\n';
        if (!out) {
            throw Error(ErrorCode::Internal, "Unable to write output record");
        }
    }
    fs::rename(temp, path);
}

// Removes a partial output on every exit path unless it was renamed into place.
struct PartialGuard {
    fs::path path;
    ~PartialGuard() { removeIfExists(path); }
};

// Releases the source lease before reclaiming cache space, on every exit path.
struct CacheMaintenance {
    cache::SourceCache& cache;
    cache::Limits limits;
    std::shared_ptr<cache::Lease>* lease;
    ~CacheMaintenance() {
        lease->reset();
        try {
            cache.maintain(limits);
        } catch (const std::exception& error) {
            yt::logger::Logger::getInstance().warning(
                std::string("Source cache maintenance failed: ") + error.what());
        }
    }
};

} // namespace

bool allowsCompletedOutputReuse(const ConversionRequest& request) {
    return request.config.reuse_completed && !request.config.force && !request.refresh;
}

std::optional<ConversionResult> findReusableOutput(const ConversionRequest& request) {
    if (!allowsCompletedOutputReuse(request)) {
        return std::nullopt;
    }
    const std::string format = validation::requireFormat(request.format);
    const validation::VideoRef video = validation::requireVideo(request.url);
    const fs::path outputRoot = fs::path(resolveOutputRoot(request.config.output_dir));
    const fs::path finalPath = outputRoot / getOutputFilename(video.id, format);
    const std::uintmax_t size = usableSize(finalPath);
    if (size == 0) {
        return std::nullopt;
    }
    ConversionResult result;
    result.output_path = fs::weakly_canonical(finalPath).string();
    result.job_id = request.job_id;
    result.video_id = video.id;
    result.reused = true;
    result.output_bytes = size;
    return result;
}

void setFreeSpaceBytesForTests(std::optional<std::uintmax_t> bytes) {
    std::lock_guard<std::mutex> lock(g_freeSpaceMutex);
    g_freeSpaceOverride = bytes;
}

std::string getOutputFilename(const std::string& videoID, const std::string& format) {
    if (!validation::isValidVideoId(videoID)) {
        throw Error(ErrorCode::InvalidInput, "Video ID is not safe for use in a filename");
    }
    const std::string normalized = validation::requireFormat(format);
    const std::string name = videoID + '.' + normalized;
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
        name.find("..") != std::string::npos) {
        throw Error(ErrorCode::InvalidInput, "Output filename would escape the output directory");
    }
    return name;
}

ConversionResult processVideo(const ConversionRequest& rawRequest) {
    ConversionRequest request = rawRequest;
    request.format = validation::requireFormat(request.format);
    const validation::VideoRef video = validation::requireVideo(request.url);
    if (request.job_id.empty()) {
        request.job_id = makeJobId(video.id, {});
    }
    if (request.admitted_at == 0) {
        request.admitted_at = cache::nowStamp();
    }

    auto& logger = yt::logger::Logger::getInstance();
    logger.setContext({request.request_id, video.id});
    struct ContextGuard {
        ~ContextGuard() { yt::logger::Logger::getInstance().clearContext(); }
    } contextGuard;

    const fs::path outputRoot = fs::path(resolveOutputRoot(request.config.output_dir));
    const fs::path finalPath = outputRoot / getOutputFilename(video.id, request.format);
    if (!pathIsInside(outputRoot, finalPath)) {
        throw Error(ErrorCode::InvalidInput, "Output path escapes the output root");
    }

    auto completedOutput = [&](bool reused, std::uint64_t downloadMs) {
        ConversionResult result;
        result.output_path = fs::weakly_canonical(finalPath).string();
        result.job_id = request.job_id;
        result.video_id = video.id;
        result.reused = reused;
        result.download_ms = downloadMs;
        result.output_bytes = usableSize(finalPath);
        return result;
    };

    // Physical metrics are recorded where work happens; logical job outcomes are recorded by
    // the job's owner (queue, HTTP fast path, or CLI). Reuse adds no physical work.
    if (auto reused = findReusableOutput(request)) {
        reused->job_id = request.job_id;
        logCompletion(*reused, request.format);
        return *reused;
    }

    const bool audioOnly = request.format == "mp3" || request.format == "wav";
    const std::string kind =
        audioOnly ? "audio" : ("v" + std::to_string(request.config.max_height));
    const fs::path kindDir = outputRoot / "cache" / "src" / video.id / kind;
    if (!pathIsInside(outputRoot, kindDir)) {
        throw Error(ErrorCode::InvalidInput, "Cache directory escapes the output root");
    }

    cache::SourceCache sourceCache(outputRoot);
    std::shared_ptr<cache::Lease> lease;
    // Declared before the work so it runs last: drop the lease, then reclaim space.
    CacheMaintenance maintenance{sourceCache, cacheLimits(request.config), &lease};
    PartialGuard partial{uniquePartialPath(finalPath)};

    {
        logger.info("Starting conversion to " + request.format);
        SourceAcquisition source = acquireSource(request, video, outputRoot, sourceCache, kind);
        lease = source.lease;
        // A canceled request stops here even when it carried a download that others still use.
        throwIfCanceled(request);

        // One writer per final output, across threads and processes. Held through the encode so
        // competing writers neither duplicate work nor interleave publication.
        const fs::path lockDir = outputRoot / "cache" / "locks";
        createPrivateDir(lockDir);
        const auto waitLimit = std::chrono::seconds(
            static_cast<std::int64_t>(request.config.convert_timeout_sec) + 30);
        fs_lock::FileLock writer = fs_lock::FileLock::acquireCancelable(
            lockDir / (finalPath.filename().string() + ".lock"), fs_lock::Mode::Exclusive,
            request.cancel, waitLimit);
        if (!writer.held()) {
            throwIfCanceled(request);
            throw Error(ErrorCode::Timeout, "Timed out waiting for another writer of this output");
        }

        // Decide under the writer lock whether this request still needs to publish:
        //  - an output another writer completed meanwhile satisfies an ordinary request;
        //  - output built from a newer source generation is never replaced by an older one;
        //  - a non-refresh replacement is satisfied by any publication after its admission.
        // A refresh carries a generation that started after its admission, so it publishes
        // unless an even newer generation is already published.
        const fs::path recordPath = outputRecordPath(outputRoot, finalPath);
        const std::uintmax_t existingBytes = usableSize(finalPath);
        const auto record = existingBytes > 0 ? readOutputRecord(recordPath) : std::nullopt;
        const bool satisfiedByExisting =
            existingBytes > 0 &&
            (allowsCompletedOutputReuse(request) ||
             (record && record->generation_stamp > lease->stamp()) ||
             (record && !request.refresh && record->published_at >= request.admitted_at));
        if (satisfiedByExisting) {
            ConversionResult ready = completedOutput(true, source.download_ms);
            ready.superseded = !allowsCompletedOutputReuse(request);
            ready.source_bytes = lease->bytes();
            ready.source_generation = lease->generation();
            ready.source_cache_hit = source.cache_hit;
            ready.shared_download = source.shared;
            ready.downloaded = source.downloaded;
            logCompletion(ready, request.format);
            return ready;
        }

        std::uint64_t convertMs = 0;
        const bool alreadyMp4 = !audioOnly && lease->source().extension() == ".mp4";
        if (alreadyMp4) {
            logger.info("Skipping ffmpeg remux for MP4");
            linkOrCopy(lease->source(), partial.path);
        } else {
            const auto convertStart = std::chrono::steady_clock::now();
            try {
                convertMedia(request, lease->source(), partial.path);
                if (usableSize(partial.path) == 0) {
                    throw Error(ErrorCode::ConversionFailed,
                                "ffmpeg produced an empty output file");
                }
            } catch (...) {
                yt::metrics::recordEncode(false, elapsedMs(convertStart));
                throw;
            }
            convertMs = elapsedMs(convertStart);
            yt::metrics::recordEncode(true, convertMs);
        }
        // A job canceled before publication never creates new output.
        throwIfCanceled(request);
        const std::uintmax_t publishedBytes = usableSize(partial.path);
        fs::rename(partial.path, finalPath);
        writeOutputRecord(recordPath, OutputRecord{lease->stamp(), cache::nowStamp()});
        yt::metrics::recordPublication(publishedBytes);

        ConversionResult result = completedOutput(false, source.download_ms);
        result.convert_ms = convertMs;
        result.source_bytes = lease->bytes();
        result.source_generation = lease->generation();
        result.source_cache_hit = source.cache_hit;
        result.shared_download = source.shared;
        result.downloaded = source.downloaded;
        result.published = true;
        if (result.output_bytes == 0) {
            throw Error(ErrorCode::ConversionFailed, "Conversion produced an empty output file");
        }
        logCompletion(result, request.format);
        return result;
    }
}

ConversionResult processVideo(const std::string& url, const std::string& format) {
    ConversionRequest request;
    request.url = url;
    request.format = format;
    request.config = loadConfigFromEnv();
    return processVideo(request);
}

} // namespace yt::converter
