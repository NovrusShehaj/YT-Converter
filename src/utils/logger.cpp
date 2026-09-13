#include "logger.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

namespace yt::logger {
namespace {

thread_local LogContext t_context;

std::string escapeJson(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char ch : input) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out.push_back(ch);
            break;
        }
    }
    return out;
}

std::string upperCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

} // namespace

Logger& Logger::getInstance() {
    static Logger instance;
    return instance;
}

Logger::Logger()
    : currentLogLevel(LogLevel::INFO),
      currentLogFormat(LogFormat::Text),
      consoleOutputEnabled(true),
      fileOutputEnabled(false),
      logFileName("converter.log") {}

Logger::~Logger() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (logFile && logFile->is_open()) {
        logFile->close();
    }
}

void Logger::setLogLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    currentLogLevel = level;
}

LogLevel Logger::getLogLevel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentLogLevel;
}

void Logger::setLogFormat(LogFormat format) {
    std::lock_guard<std::mutex> lock(mutex_);
    currentLogFormat = format;
}

void Logger::setConsoleOutput(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    consoleOutputEnabled = enabled;
}

void Logger::setFileOutput(bool enabled, const std::string& filename) {
    std::lock_guard<std::mutex> lock(mutex_);
    fileOutputEnabled = enabled;
    logFileName = filename;
    if (enabled) {
        logFile = std::make_unique<std::ofstream>(filename, std::ios::app);
    } else if (logFile) {
        logFile->close();
        logFile.reset();
    }
}

void Logger::setContext(const LogContext& context) { t_context = context; }

void Logger::clearContext() { t_context = LogContext{}; }

void Logger::debug(const std::string& message) { log(LogLevel::DEBUG, message); }

void Logger::info(const std::string& message) { log(LogLevel::INFO, message); }

void Logger::warning(const std::string& message) { log(LogLevel::WARNING, message); }

void Logger::error(const std::string& message) { log(LogLevel::ERROR, message); }

void Logger::critical(const std::string& message) { log(LogLevel::CRITICAL, message); }

void Logger::log(LogLevel level, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (level < currentLogLevel) {
        return;
    }
    const std::string formatted = formatMessage(level, message, t_context);
    if (consoleOutputEnabled) {
        if (level >= LogLevel::ERROR) {
            std::cerr << formatted << std::endl;
        } else {
            std::cout << formatted << std::endl;
        }
    }
    if (fileOutputEnabled && logFile && logFile->is_open()) {
        *logFile << formatted << std::endl;
        logFile->flush();
    }
}

std::string Logger::levelToString(LogLevel level) const {
    switch (level) {
    case LogLevel::DEBUG:
        return "DEBUG";
    case LogLevel::INFO:
        return "INFO";
    case LogLevel::WARNING:
        return "WARNING";
    case LogLevel::ERROR:
        return "ERROR";
    case LogLevel::CRITICAL:
        return "CRITICAL";
    }
    return "UNKNOWN";
}

std::string Logger::getCurrentTimestamp() const {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) %
                    1000;
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::ostringstream ss;
    ss << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

std::string Logger::formatMessage(LogLevel level, const std::string& message,
                                  const LogContext& context) const {
    const std::string timestamp = getCurrentTimestamp();
    if (currentLogFormat == LogFormat::Json) {
        std::ostringstream ss;
        ss << "{\"ts\":\"" << timestamp << "\",\"level\":\"" << levelToString(level)
           << "\",\"msg\":\"" << escapeJson(message) << "\"";
        if (!context.request_id.empty()) {
            ss << ",\"request_id\":\"" << escapeJson(context.request_id) << "\"";
        }
        if (!context.video_id.empty()) {
            ss << ",\"video_id\":\"" << escapeJson(context.video_id) << "\"";
        }
        ss << "}";
        return ss.str();
    }

    std::ostringstream ss;
    ss << '[' << timestamp << "] [" << levelToString(level) << ']';
    if (!context.request_id.empty()) {
        ss << " [" << context.request_id << ']';
    }
    if (!context.video_id.empty()) {
        ss << " [id=" << context.video_id << ']';
    }
    ss << ' ' << message;
    return ss.str();
}

LogLevel parseLogLevel(const std::string& value) {
    const std::string upper = upperCopy(value);
    if (upper == "DEBUG") {
        return LogLevel::DEBUG;
    }
    if (upper == "WARNING" || upper == "WARN") {
        return LogLevel::WARNING;
    }
    if (upper == "ERROR") {
        return LogLevel::ERROR;
    }
    if (upper == "CRITICAL") {
        return LogLevel::CRITICAL;
    }
    return LogLevel::INFO;
}

LogFormat parseLogFormat(const std::string& value) {
    const std::string upper = upperCopy(value);
    if (upper == "JSON") {
        return LogFormat::Json;
    }
    return LogFormat::Text;
}

} // namespace yt::logger
