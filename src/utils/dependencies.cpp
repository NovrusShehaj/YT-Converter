#include "dependencies.h"
#include "error.h"
#include "process.h"

#include <algorithm>
#include <cctype>

namespace yt::deps {
namespace {

std::string firstLine(const std::string& text) {
    const std::size_t newline = text.find('\n');
    std::string line = newline == std::string::npos ? text : text.substr(0, newline);
    while (!line.empty() && (line.back() == '\r' || std::isspace(static_cast<unsigned char>(line.back())))) {
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
        throw Error(ErrorCode::BinaryNotFound, "Required tool not found: " + binary + ". " + installHint());
    }
    if (result.timed_out) {
        throw Error(ErrorCode::Timeout, "Timed out while checking " + binary);
    }
    if (result.exit_code != 0) {
        throw Error(ErrorCode::BinaryNotFound,
                    binary + " failed during preflight. " + installHint());
    }
    const std::string version = firstLine(result.stdout_text.empty() ? result.stderr_text : result.stdout_text);
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

} // namespace yt::deps
