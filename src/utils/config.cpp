#include "config.h"
#include "error.h"
#include "logger.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace yt {
namespace {

std::string envOr(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value == nullptr ? fallback : std::string(value);
}

std::string trimCopy(std::string value) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

bool envFlag(const char* name) {
    std::string value = trimCopy(envOr(name, ""));
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

int envInt(const char* name, int fallback, int minValue, int maxValue) {
    const std::string raw = trimCopy(envOr(name, ""));
    if (raw.empty()) {
        return fallback;
    }
    try {
        const int parsed = std::stoi(raw);
        if (parsed < minValue || parsed > maxValue) {
            throw Error(ErrorCode::ConfigError, std::string(name) + " is out of range");
        }
        return parsed;
    } catch (const Error&) {
        throw;
    } catch (const std::exception&) {
        throw Error(ErrorCode::ConfigError, std::string(name) + " must be an integer");
    }
}

} // namespace

bool isLoopbackBind(const std::string& bind) {
    return bind == "127.0.0.1" || bind == "localhost" || bind == "::1";
}

bool isWildcardBind(const std::string& bind) {
    return bind == "0.0.0.0" || bind == "::" || bind == "*";
}

bool constantTimeEquals(const std::string& left, const std::string& right) {
    const std::size_t n = left.size() > right.size() ? left.size() : right.size();
    unsigned char diff = static_cast<unsigned char>(left.size() != right.size());
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char a = i < left.size() ? static_cast<unsigned char>(left[i]) : 0;
        const unsigned char b = i < right.size() ? static_cast<unsigned char>(right[i]) : 0;
        diff = static_cast<unsigned char>(diff | (a ^ b));
    }
    return diff == 0;
}

Config loadConfigFromEnv() {
    Config config;
    config.bind = trimCopy(envOr("YTCONV_BIND", config.bind));
    config.port = envInt("YTCONV_PORT", config.port, 1, 65535);
    config.log_level = trimCopy(envOr("YTCONV_LOG_LEVEL", config.log_level));
    config.log_format = trimCopy(envOr("YTCONV_LOG_FORMAT", config.log_format));
    config.output_dir = trimCopy(envOr("YTCONV_OUTPUT_DIR", config.output_dir));
    config.max_concurrent = envInt("YTCONV_MAX_CONCURRENT", config.max_concurrent, 1, 32);
    config.child_timeout_sec =
        envInt("YTCONV_CHILD_TIMEOUT_SEC", config.child_timeout_sec, 1, 86400);
    config.socket_timeout_sec =
        envInt("YTCONV_SOCKET_TIMEOUT_SEC", config.socket_timeout_sec, 1, 300);
    config.max_filesize = trimCopy(envOr("YTCONV_MAX_FILESIZE", config.max_filesize));
    config.retries = envInt("YTCONV_RETRIES", config.retries, 0, 10);
    config.max_height = envInt("YTCONV_MAX_HEIGHT", config.max_height, 144, 4320);
    config.yt_dlp_path = trimCopy(envOr("YTCONV_YT_DLP", config.yt_dlp_path));
    config.ffmpeg_path = trimCopy(envOr("YTCONV_FFMPEG", config.ffmpeg_path));
    config.api_key = envOr("YTCONV_API_KEY", "");
    config.allow_remote = envFlag("YTCONV_ALLOW_REMOTE");
    config.allow_unauthenticated_localhost =
        envFlag("YTCONV_ALLOW_UNAUTHENTICATED_LOCALHOST");
    config.log_urls = envFlag("YTCONV_LOG_URLS");
    return config;
}

void applyLogConfig(const Config& config) {
    auto& logger = yt::logger::Logger::getInstance();
    logger.setLogLevel(yt::logger::parseLogLevel(config.log_level));
    logger.setLogFormat(yt::logger::parseLogFormat(config.log_format));
}

void validateApiConfig(const Config& config) {
    if (config.port < 1 || config.port > 65535) {
        throw Error(ErrorCode::ConfigError, "YTCONV_PORT must be between 1 and 65535");
    }
    if (isWildcardBind(config.bind)) {
        if (!config.allow_remote) {
            throw Error(ErrorCode::ConfigError,
                        "Refusing to bind " + config.bind +
                            ". Set YTCONV_ALLOW_REMOTE=1 and YTCONV_API_KEY to enable remote bind");
        }
        if (config.api_key.empty()) {
            throw Error(ErrorCode::ConfigError,
                        "Remote bind requires YTCONV_API_KEY to be set");
        }
    } else if (!isLoopbackBind(config.bind)) {
        throw Error(ErrorCode::ConfigError,
                    "Bind address must be 127.0.0.1, localhost, ::1, or a wildcard with remote "
                    "guards enabled");
    }
    if (config.api_key.empty() && !config.allow_unauthenticated_localhost) {
        throw Error(ErrorCode::ConfigError,
                    "Set YTCONV_API_KEY or pass --allow-unauthenticated-localhost");
    }
    if (!config.api_key.empty() && config.api_key.size() < 8) {
        throw Error(ErrorCode::ConfigError, "YTCONV_API_KEY must be at least 8 characters");
    }
}

std::string resolveOutputRoot(const std::string& output_dir) {
    namespace fs = std::filesystem;
    fs::path path = output_dir.empty() ? fs::path("./output") : fs::path(output_dir);
    if (path.is_relative()) {
        path = fs::current_path() / path;
    }
    fs::create_directories(path);
#ifndef _WIN32
    fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace);
#endif
    return fs::weakly_canonical(path).string();
}

} // namespace yt
