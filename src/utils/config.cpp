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

bool envBool(const char* name, bool fallback) {
    std::string value = trimCopy(envOr(name, ""));
    if (value.empty()) {
        return fallback;
    }
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        return false;
    }
    throw Error(ErrorCode::ConfigError, std::string(name) + " must be 1 or 0");
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

int64_t parseHumanSize(const std::string& value) {
    if (value.empty()) {
        throw Error(ErrorCode::ConfigError, "YTCONV_MAX_FILESIZE is empty");
    }
    std::string v = value;
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    int64_t multiplier = 1;
    // Check for two-character suffixes first (KB, MB, GB)
    if (v.size() >= 2) {
        std::string suffix = v.substr(v.size() - 2);
        if (suffix == "kb") {
            multiplier = 1000;
            v = v.substr(0, v.size() - 2);
        } else if (suffix == "mb") {
            multiplier = 1000 * 1000;
            v = v.substr(0, v.size() - 2);
        } else if (suffix == "gb") {
            multiplier = 1000 * 1000 * 1000;
            v = v.substr(0, v.size() - 2);
        }
    }
    // Then check for single-character suffixes (K, M, G)
    if (multiplier == 1 && v.size() >= 1) {
        char last = v.back();
        if (last == 'k') {
            multiplier = 1024;
            v.pop_back();
        } else if (last == 'm') {
            multiplier = 1024 * 1024;
            v.pop_back();
        } else if (last == 'g') {
            multiplier = 1024 * 1024 * 1024;
            v.pop_back();
        }
    }
    try {
        const int64_t size = std::stoll(v);
        if (size <= 0) {
            throw Error(ErrorCode::ConfigError, "YTCONV_MAX_FILESIZE must be positive");
        }
        return size * multiplier;
    } catch (const Error&) {
        throw;
    } catch (const std::exception&) {
        throw Error(ErrorCode::ConfigError,
                    "YTCONV_MAX_FILESIZE must be a number such as 500M or 1G");
    }
}

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
    config.queue_depth = envInt("YTCONV_QUEUE_DEPTH", config.queue_depth, 1, 256);
    config.max_active_jobs = envInt("YTCONV_MAX_ACTIVE_JOBS", config.max_active_jobs, 1, 65536);
    config.job_history_max = envInt("YTCONV_JOB_HISTORY_MAX", config.job_history_max, 0, 100000);
    config.job_history_ttl_sec =
        envInt("YTCONV_JOB_HISTORY_TTL_SEC", config.job_history_ttl_sec, 1, 7 * 86400);
    config.request_read_timeout_sec =
        envInt("YTCONV_REQUEST_READ_TIMEOUT_SEC", config.request_read_timeout_sec, 1, 300);
    config.max_pending_reads =
        envInt("YTCONV_MAX_PENDING_READS", config.max_pending_reads, 1, 1024);
    config.max_connections = envInt("YTCONV_MAX_CONNECTIONS", config.max_connections, 1, 4096);
    if (config.max_connections < config.max_pending_reads) {
        throw Error(ErrorCode::ConfigError,
                    "YTCONV_MAX_CONNECTIONS must be at least YTCONV_MAX_PENDING_READS");
    }
    if (config.max_active_jobs < config.queue_depth) {
        throw Error(ErrorCode::ConfigError,
                    "YTCONV_MAX_ACTIVE_JOBS must be at least YTCONV_QUEUE_DEPTH, because every "
                    "operation has at least one job");
    }
    config.ready_ttl_sec = envInt("YTCONV_READY_TTL_SEC", config.ready_ttl_sec, 0, 86400);
    config.source_cache_ttl_sec =
        envInt("YTCONV_SOURCE_CACHE_TTL_SEC", config.source_cache_ttl_sec, 0, 86400 * 30);
    config.sync_conversions = envFlag("YTCONV_SYNC_CONVERSIONS");
    config.child_timeout_sec =
        envInt("YTCONV_CHILD_TIMEOUT_SEC", config.child_timeout_sec, 1, 86400);
    config.download_timeout_sec =
        envInt("YTCONV_DOWNLOAD_TIMEOUT_SEC", config.download_timeout_sec, 1, 86400);
    config.convert_timeout_sec =
        envInt("YTCONV_CONVERT_TIMEOUT_SEC", config.convert_timeout_sec, 1, 86400);
    config.socket_timeout_sec =
        envInt("YTCONV_SOCKET_TIMEOUT_SEC", config.socket_timeout_sec, 1, 300);
    std::string maxFilesizeStr = trimCopy(envOr("YTCONV_MAX_FILESIZE", config.max_filesize));
    config.max_filesize = maxFilesizeStr;
    config.max_filesize_bytes = parseHumanSize(maxFilesizeStr);
    if (config.max_filesize_bytes <= 0) {
        config.max_filesize_bytes = 500 * 1024 * 1024;
    }
    config.retries = envInt("YTCONV_RETRIES", config.retries, 0, 10);
    config.concurrent_fragments =
        envInt("YTCONV_CONCURRENT_FRAGMENTS", config.concurrent_fragments, 1, 16);
    config.fragment_retries = envInt("YTCONV_FRAGMENT_RETRIES", config.fragment_retries, 0, 100);
    config.max_height = envInt("YTCONV_MAX_HEIGHT", config.max_height, 144, 4320);
    config.cache_dir = trimCopy(envOr("YTCONV_CACHE_DIR", config.cache_dir));
    const std::string cacheMax = trimCopy(envOr("YTCONV_SOURCE_CACHE_MAX_BYTES", ""));
    if (!cacheMax.empty()) {
        config.source_cache_max_bytes = parseHumanSize(cacheMax);
    }
    config.yt_dlp_path = trimCopy(envOr("YTCONV_YT_DLP", config.yt_dlp_path));
    config.ffmpeg_path = trimCopy(envOr("YTCONV_FFMPEG", config.ffmpeg_path));
    config.api_key = envOr("YTCONV_API_KEY", "");
    config.allow_remote = envFlag("YTCONV_ALLOW_REMOTE");
    config.allow_unauthenticated_localhost = envFlag("YTCONV_ALLOW_UNAUTHENTICATED_LOCALHOST");
    config.log_urls = envFlag("YTCONV_LOG_URLS");
    config.reuse_completed = envBool("YTCONV_REUSE_COMPLETED", config.reuse_completed);
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
            throw Error(ErrorCode::ConfigError, "Remote bind requires YTCONV_API_KEY to be set");
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
