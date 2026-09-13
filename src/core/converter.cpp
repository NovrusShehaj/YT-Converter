#include "converter.h"
#include "error.h"
#include "logger.h"
#include "metrics.h"
#include "process.h"
#include "validation.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace yt::converter {
namespace {

namespace fs = std::filesystem;

constexpr std::uintmax_t kMinFreeBytes = 50ull * 1024ull * 1024ull;

std::mutex g_idMapMutex;
std::map<std::string, std::shared_ptr<std::mutex>> g_idLocks;

std::shared_ptr<std::mutex> lockForVideo(const std::string& videoId) {
    std::lock_guard<std::mutex> lock(g_idMapMutex);
    auto& entry = g_idLocks[videoId];
    if (!entry) {
        entry = std::make_shared<std::mutex>();
    }
    return entry;
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
    const std::string lower = [&]() {
        std::string value = text;
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }();
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

fs::path findSourceFile(const fs::path& jobDir) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(jobDir, ec)) {
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

void downloadMedia(const ConversionRequest& request, const validation::VideoRef& video,
                   const fs::path& jobDir) {
    const bool audioOnly = request.format == "mp3" || request.format == "wav";
    const fs::path outputTemplate = jobDir / "source.%(ext)s";
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
        "-f",
        audioOnly ? "bestaudio/best" : videoFormatSelector(request.config.max_height),
        "-o",
        outputTemplate.string(),
        video.canonical_url,
    };

    auto& logger = yt::logger::Logger::getInstance();
    logger.info("Downloading video " + video.id);
    if (request.config.log_urls) {
        logger.debug("Download URL: " + video.canonical_url);
    }

    yt::process::RunOptions options;
    options.timeout_ms = request.config.child_timeout_sec * 1000;
    options.inherit_stderr = request.show_progress;
    const auto result = yt::process::run(argv, options);
    if (result.exit_code != 0 || result.not_found || result.timed_out || result.canceled) {
        throwSpawnError(ErrorCode::DownloadFailed, request.config.yt_dlp_path, result);
    }
    if (findSourceFile(jobDir).empty()) {
        throw Error(ErrorCode::DownloadFailed, "yt-dlp completed without creating a source file");
    }
}

void convertMedia(const ConversionRequest& request, const fs::path& source,
                  const fs::path& outputFile) {
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
        argv.insert(argv.end(), {"-vn", "-ar", "44100", "-ac", "2", "-b:a", "192k", "-codec:a",
                                 "libmp3lame"});
    } else if (request.format == "wav") {
        argv.insert(argv.end(), {"-vn", "-acodec", "pcm_s16le", "-ar", "44100", "-ac", "2"});
    } else {
        argv.insert(argv.end(), {"-c", "copy"});
    }
    argv.push_back(outputFile.string());

    auto& logger = yt::logger::Logger::getInstance();
    logger.info("Converting to " + request.format);

    yt::process::RunOptions options;
    options.timeout_ms = request.config.child_timeout_sec * 1000;
    options.inherit_stderr = request.show_progress;
    const auto result = yt::process::run(argv, options);
    if (result.exit_code != 0 || result.not_found || result.timed_out || result.canceled) {
        throwSpawnError(ErrorCode::ConversionFailed, request.config.ffmpeg_path, result);
    }
}

class JobWorkspace {
public:
    explicit JobWorkspace(fs::path jobDir) : jobDir_(std::move(jobDir)) {}

    ~JobWorkspace() {
        if (!keepFinal_) {
            removeIfExists(jobDir_);
        } else {
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(jobDir_, ec)) {
                const std::string name = entry.path().filename().string();
                if (name.rfind("source.", 0) == 0 || name.rfind("temp", 0) == 0) {
                    removeIfExists(entry.path());
                }
            }
        }
    }

    void markSuccess() { keepFinal_ = true; }

private:
    fs::path jobDir_;
    bool keepFinal_ = false;
};

} // namespace

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
    yt::metrics::recordStart();
    try {
        logger.info("Starting conversion to " + request.format);
        if (request.config.log_urls) {
            logger.debug("User URL accepted and reconstructed to " + video.canonical_url);
        }

        const fs::path outputRoot = fs::path(resolveOutputRoot(request.config.output_dir));
        const fs::path jobsRoot = outputRoot / "jobs";
        createPrivateDir(jobsRoot);
        const fs::path jobDir = jobsRoot / request.job_id;
        if (!pathIsInside(outputRoot, jobDir)) {
            throw Error(ErrorCode::InvalidInput, "Job directory escapes the output root");
        }
        createPrivateDir(jobDir);

        const fs::path finalPath = outputRoot / getOutputFilename(video.id, request.format);
        if (!pathIsInside(outputRoot, finalPath)) {
            throw Error(ErrorCode::InvalidInput, "Output path escapes the output root");
        }

        const auto idLock = lockForVideo(video.id);
        std::lock_guard<std::mutex> exclusive(*idLock);

        if (request.config.reuse_completed && !request.config.force && fs::exists(finalPath) &&
            fs::file_size(finalPath) > 0) {
            logger.info("Reusing existing output for " + video.id);
            ConversionResult reused;
            reused.output_path = fs::weakly_canonical(finalPath).string();
            reused.job_id = request.job_id;
            reused.video_id = video.id;
            reused.reused = true;
            removeIfExists(jobDir);
            yt::metrics::recordSuccess(fs::file_size(finalPath));
            return reused;
        }

        const auto space = fs::space(outputRoot);
        if (space.available < kMinFreeBytes) {
            removeIfExists(jobDir);
            throw Error(ErrorCode::DiskFull, "Not enough free disk space in the output directory");
        }

        JobWorkspace workspace(jobDir);
        const fs::path tempOutput = jobDir / getOutputFilename(video.id, request.format);

        try {
            downloadMedia(request, video, jobDir);
            const fs::path source = findSourceFile(jobDir);
            convertMedia(request, source, tempOutput);
            if (!fs::exists(tempOutput) || fs::file_size(tempOutput) == 0) {
                throw Error(ErrorCode::ConversionFailed, "ffmpeg produced an empty output file");
            }
            fs::rename(tempOutput, finalPath);
            workspace.markSuccess();
            removeIfExists(jobDir);
        } catch (...) {
            removeIfExists(tempOutput);
            removeIfExists(finalPath);
            throw;
        }

        ConversionResult result;
        result.output_path = fs::weakly_canonical(finalPath).string();
        result.job_id = request.job_id;
        result.video_id = video.id;
        logger.info("Conversion completed");
        yt::metrics::recordSuccess(fs::file_size(finalPath));
        return result;
    } catch (...) {
        yt::metrics::recordFailure();
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
