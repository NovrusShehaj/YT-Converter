#include "converter.h"
#include "error.h"
#include "logger.h"
#include "metrics.h"
#include "process.h"
#include "singleflight.h"
#include "validation.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
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

singleflight::Group& downloadFlights() {
    static singleflight::Group group;
    return group;
}

singleflight::Group& encodeFlights() {
    static singleflight::Group group;
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
           lower.find("enospc") != std::string::npos || lower.find("disk full") != std::string::npos;
}

void throwSpawnError(ErrorCode fallback, const std::string& tool, const yt::process::RunResult& result) {
    if (result.canceled) {
        throw Error(ErrorCode::Canceled, tool + " canceled");
    }
    if (result.not_found) {
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

void throwFlightError(const std::shared_ptr<singleflight::Flight>& flight) {
    const auto code = static_cast<ErrorCode>(flight->error_code);
    const std::string message = flight->error.empty() ? "conversion failed" : flight->error;
    throw Error(flight->error_code == 0 ? ErrorCode::Internal : code, message);
}

fs::path findSourceFile(const fs::path& directory) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name.rfind("source.", 0) == 0 && name.find(".part") == std::string::npos) {
            return entry.path();
        }
    }
    return {};
}

std::string videoFormatSelector(int maxHeight) {
    std::ostringstream ss;
    ss << "bestvideo[height<=" << maxHeight << "][ext=mp4]+bestaudio[ext=m4a]/best[height<="
       << maxHeight << "][ext=mp4]/mp4";
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
    while (begin > 0 && (std::isdigit(static_cast<unsigned char>(line[begin - 1])) || line[begin - 1] == '.')) {
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
    const auto need = static_cast<std::uintmax_t>(std::max<std::int64_t>(0, maxFileBytes)) + kMinFreeBytes;
    if (availableBytes(root) < need) {
        throw Error(ErrorCode::DiskFull, "Not enough free disk space in the output directory");
    }
}

bool cacheFresh(const fs::path& file, int ttlSec) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec) || fs::file_size(file, ec) == 0) {
        return false;
    }
    if (ttlSec <= 0) {
        return false;
    }
    const auto age = fs::file_time_type::clock::now() - fs::last_write_time(file, ec);
    if (ec) {
        return false;
    }
    return age < std::chrono::seconds(ttlSec);
}

fs::path metadataCacheDir(const fs::path& outputRoot, const Config& config) {
    if (config.cache_dir.empty()) {
        return outputRoot / "cache" / "ytdlp";
    }
    const fs::path configured(config.cache_dir);
    if (configured.is_absolute()) {
        return configured;
    }
    return outputRoot / configured;
}

void evictSourceCache(const fs::path& root, std::int64_t maxBytes, const fs::path& keep) {
    if (maxBytes <= 0 || !fs::exists(root)) {
        return;
    }
    struct Item {
        fs::path file;
        std::uintmax_t bytes = 0;
        fs::file_time_type when{};
    };
    std::vector<Item> items;
    std::uintmax_t total = 0;
    std::error_code ec;
    for (const auto& videoDir : fs::directory_iterator(root, ec)) {
        if (!videoDir.is_directory()) {
            continue;
        }
        for (const auto& kindDir : fs::directory_iterator(videoDir.path(), ec)) {
            if (!kindDir.is_directory()) {
                continue;
            }
            const fs::path source = findSourceFile(kindDir.path());
            if (source.empty()) {
                continue;
            }
            Item item;
            item.file = source;
            item.bytes = fs::file_size(source, ec);
            item.when = fs::last_write_time(source, ec);
            total += item.bytes;
            items.push_back(std::move(item));
        }
    }
    std::sort(items.begin(), items.end(),
              [](const Item& left, const Item& right) { return left.when < right.when; });
    for (const Item& item : items) {
        if (total <= static_cast<std::uintmax_t>(maxBytes)) {
            break;
        }
        if (item.file == keep) {
            continue;
        }
        removeIfExists(item.file);
        if (total >= item.bytes) {
            total -= item.bytes;
        }
    }
}

void publishFile(const fs::path& source, const fs::path& finalPath) {
    const fs::path partial(finalPath.string() + ".partial");
    std::error_code ec;
    fs::remove(partial, ec);
    fs::create_hard_link(source, partial, ec);
    if (ec) {
        fs::copy_file(source, partial, fs::copy_options::overwrite_existing);
    }
    fs::rename(partial, finalPath);
}

struct SpawnedDownload {
    fs::path source;
    std::uint64_t download_ms = 0;
    std::uint64_t bytes = 0;
};

SpawnedDownload downloadMedia(const ConversionRequest& request, const validation::VideoRef& video,
                              const fs::path& outputRoot, const fs::path& kindDir) {
    const bool audioOnly = request.format == "mp3" || request.format == "wav";
    const fs::path incoming = kindDir / "incoming";
    removeIfExists(incoming);
    createPrivateDir(incoming);
    const fs::path outputTemplate = incoming / "source.%(ext)s";
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
    argv.insert(argv.end(), {"-f", audioOnly ? audioFormatSelector()
                                             : videoFormatSelector(request.config.max_height),
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
    options.cancel = request.cancel;

    const auto started = std::chrono::steady_clock::now();
    const auto result = yt::process::run(argv, options);
    if (result.exit_code != 0 || result.not_found || result.timed_out || result.canceled) {
        removeIfExists(incoming);
        throwSpawnError(ErrorCode::DownloadFailed, request.config.yt_dlp_path, result);
    }
    const fs::path produced = findSourceFile(incoming);
    if (produced.empty() || fs::file_size(produced) == 0) {
        removeIfExists(incoming);
        throw Error(ErrorCode::DownloadFailed, "yt-dlp completed without creating a source file");
    }
    const fs::path stable = kindDir / produced.filename();
    std::error_code ec;
    fs::remove(stable, ec);
    fs::rename(produced, stable);
    removeIfExists(incoming);
    evictSourceCache(outputRoot / "cache" / "src", request.config.source_cache_max_bytes, stable);

    SpawnedDownload downloaded;
    downloaded.source = stable;
    downloaded.bytes = fs::file_size(stable);
    downloaded.download_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
            .count());
    if (downloaded.download_ms == 0) {
        downloaded.download_ms = 1;
    }
    return downloaded;
}

void convertMedia(const ConversionRequest& request, const fs::path& source, const fs::path& partial) {
    std::vector<std::string> argv{
        request.config.ffmpeg_path, "-y", "-nostdin", "-hide_banner", "-loglevel", "error", "-i",
        source.string(),
    };
    if (request.format == "mp3") {
        argv.insert(argv.end(), {"-vn", "-ar", "44100", "-ac", "2", "-b:a", "192k", "-codec:a", "libmp3lame"});
    } else if (request.format == "wav") {
        argv.insert(argv.end(), {"-vn", "-acodec", "pcm_s16le", "-ar", "44100", "-ac", "2"});
    } else {
        argv.insert(argv.end(), {"-c", "copy"});
    }
    argv.push_back(partial.string());

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
       << " convert_ms=" << result.convert_ms << " bytes=" << result.bytes_downloaded;
    yt::logger::Logger::getInstance().info(ss.str());
}

ConversionResult resultFromFlight(const ConversionRequest& request, const validation::VideoRef& video,
                                  const singleflight::Flight& flight, bool reused) {
    ConversionResult result;
    result.output_path = flight.value;
    result.job_id = request.job_id;
    result.video_id = video.id;
    result.reused = reused;
    result.download_ms = flight.download_ms;
    result.convert_ms = flight.convert_ms;
    result.bytes_downloaded = flight.bytes;
    return result;
}

} // namespace

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
    const fs::path partial(finalPath.string() + ".partial");

    if (request.config.reuse_completed && !request.config.force && !request.refresh &&
        fs::exists(finalPath) && fs::file_size(finalPath) > 0) {
        yt::metrics::recordStart();
        ConversionResult reused;
        reused.output_path = fs::weakly_canonical(finalPath).string();
        reused.job_id = request.job_id;
        reused.video_id = video.id;
        reused.reused = true;
        reused.bytes_downloaded = fs::file_size(finalPath);
        yt::metrics::recordSuccess(fs::file_size(finalPath), 0, 0, reused.bytes_downloaded);
        logCompletion(reused, request.format);
        return reused;
    }

    const bool audioOnly = request.format == "mp3" || request.format == "wav";
    const std::string kind = audioOnly ? "audio" : ("v" + std::to_string(request.config.max_height));
    const std::string downloadKey = outputRoot.string() + "\n" + video.id + "\n" + kind;
    const std::string encodeKey = outputRoot.string() + "\n" + video.id + "\n" + request.format;
    const fs::path kindDir = outputRoot / "cache" / "src" / video.id / kind;
    if (!pathIsInside(outputRoot, kindDir)) {
        throw Error(ErrorCode::InvalidInput, "Cache directory escapes the output root");
    }

    const auto encodeMembership = encodeFlights().join(encodeKey);
    if (!encodeMembership.is_leader) {
        encodeFlights().wait(encodeMembership.flight);
        if (!encodeMembership.flight->ok) {
            throwFlightError(encodeMembership.flight);
        }
        const ConversionResult followed =
            resultFromFlight(request, video, *encodeMembership.flight, true);
        logCompletion(followed, request.format);
        return followed;
    }

    bool encodePublished = false;
    bool metricsStarted = false;
    try {
        logger.info("Starting conversion to " + request.format);
        yt::metrics::recordStart();
        metricsStarted = true;

        fs::path source;
        std::uint64_t downloadMs = 0;
        std::uint64_t downloadedBytes = 0;
        const fs::path cached = findSourceFile(kindDir);
        if (!request.refresh && cacheFresh(cached, request.config.source_cache_ttl_sec)) {
            source = cached;
            downloadedBytes = fs::file_size(cached);
            logger.info("Reusing cached source for " + video.id);
        } else {
            const auto downloadMembership = downloadFlights().join(downloadKey);
            if (!downloadMembership.is_leader) {
                downloadFlights().wait(downloadMembership.flight);
                if (!downloadMembership.flight->ok) {
                    throwFlightError(downloadMembership.flight);
                }
                source = downloadMembership.flight->value;
                downloadMs = downloadMembership.flight->download_ms;
                downloadedBytes = downloadMembership.flight->bytes;
            } else {
                try {
                    ensureSpace(outputRoot, request.config.max_filesize_bytes);
                    if (request.refresh) {
                        removeIfExists(cached);
                    }
                    const SpawnedDownload downloaded =
                        downloadMedia(request, video, outputRoot, kindDir);
                    source = downloaded.source;
                    downloadMs = downloaded.download_ms;
                    downloadedBytes = downloaded.bytes;
                    downloadMembership.flight->value = source.string();
                    downloadMembership.flight->download_ms = downloadMs;
                    downloadMembership.flight->bytes = downloadedBytes;
                    downloadFlights().succeed(downloadKey, downloadMembership.flight);
                } catch (const Error& error) {
                    downloadFlights().fail(downloadKey, downloadMembership.flight,
                                           static_cast<int>(error.code()), error.message());
                    throw;
                } catch (const std::exception& error) {
                    downloadFlights().fail(downloadKey, downloadMembership.flight,
                                           static_cast<int>(ErrorCode::Internal), error.what());
                    throw;
                }
            }
        }

        std::uint64_t convertMs = 0;
        if (!request.config.force && fs::exists(finalPath) && fs::file_size(finalPath) > 0) {
            ConversionResult ready;
            ready.output_path = fs::weakly_canonical(finalPath).string();
            ready.job_id = request.job_id;
            ready.video_id = video.id;
            ready.reused = true;
            ready.download_ms = downloadMs;
            ready.convert_ms = 0;
            ready.bytes_downloaded = downloadedBytes;
            encodeMembership.flight->value = ready.output_path;
            encodeMembership.flight->download_ms = downloadMs;
            encodeMembership.flight->bytes = downloadedBytes;
            encodeFlights().succeed(encodeKey, encodeMembership.flight);
            encodePublished = true;
            yt::metrics::recordSuccess(fs::file_size(finalPath), downloadMs, 0, downloadedBytes);
            logCompletion(ready, request.format);
            return ready;
        }

        const bool alreadyMp4 = !audioOnly && source.extension() == ".mp4" && fs::file_size(source) > 0;
        if (alreadyMp4) {
            logger.info("Skipping ffmpeg remux for MP4");
            publishFile(source, finalPath);
        } else {
            const auto convertStart = std::chrono::steady_clock::now();
            convertMedia(request, source, partial);
            convertMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                     convertStart)
                    .count());
            if (!fs::exists(partial) || fs::file_size(partial) == 0) {
                throw Error(ErrorCode::ConversionFailed, "ffmpeg produced an empty output file");
            }
            fs::rename(partial, finalPath);
        }

        if (!fs::exists(finalPath) || fs::file_size(finalPath) == 0) {
            throw Error(ErrorCode::ConversionFailed, "Conversion produced an empty output file");
        }

        ConversionResult result;
        result.output_path = fs::weakly_canonical(finalPath).string();
        result.job_id = request.job_id;
        result.video_id = video.id;
        result.download_ms = downloadMs;
        result.convert_ms = convertMs;
        result.bytes_downloaded = downloadedBytes;
        encodeMembership.flight->value = result.output_path;
        encodeMembership.flight->download_ms = downloadMs;
        encodeMembership.flight->convert_ms = convertMs;
        encodeMembership.flight->bytes = downloadedBytes;
        encodeFlights().succeed(encodeKey, encodeMembership.flight);
        encodePublished = true;
        yt::metrics::recordSuccess(fs::file_size(finalPath), downloadMs, convertMs, downloadedBytes);
        logCompletion(result, request.format);
        return result;
    } catch (const Error& error) {
        removeIfExists(partial);
        if (!encodePublished) {
            encodeFlights().fail(encodeKey, encodeMembership.flight, static_cast<int>(error.code()),
                                 error.message());
        }
        if (metricsStarted) {
            yt::metrics::recordFailure();
        }
        throw;
    } catch (const std::exception& error) {
        removeIfExists(partial);
        if (!encodePublished) {
            encodeFlights().fail(encodeKey, encodeMembership.flight, static_cast<int>(ErrorCode::Internal),
                                 error.what());
        }
        if (metricsStarted) {
            yt::metrics::recordFailure();
        }
        throw;
    } catch (...) {
        removeIfExists(partial);
        if (!encodePublished) {
            encodeFlights().fail(encodeKey, encodeMembership.flight, static_cast<int>(ErrorCode::Internal),
                                 "conversion failed");
        }
        if (metricsStarted) {
            yt::metrics::recordFailure();
        }
        throw;
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
