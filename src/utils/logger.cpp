#include "logger.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <ctime>

namespace yt::logging {

Logger& Logger::getInstance() {
    static Logger instance;
    return instance;
}

Logger::Logger() 
    : minLogLevel_(LogLevel::Info), 
      outputStream_(&std::cout), 
      errorStream_(&std::cerr),
      showTimestamp_(true) {
}

Logger::~Logger() {
}

void Logger::setOutputStream(std::ostream* out) {
    if (out) {
        outputStream_ = out;
    }
}

void Logger::setLogLevel(LogLevel level) {
    minLogLevel_ = level;
}

LogLevel Logger::getLogLevel() const {
    return minLogLevel_;
}

void Logger::debug(const std::string& message) {
    if (minLogLevel_ <= LogLevel::Debug) {
        writeLog(LogLevel::Debug, message);
    }
}

void Logger::info(const std::string& message) {
    if (minLogLevel_ <= LogLevel::Info) {
        writeLog(LogLevel::Info, message);
    }
}

void Logger::warning(const std::string& message) {
    if (minLogLevel_ <= LogLevel::Warning) {
        writeLog(LogLevel::Warning, message);
    }
}

void Logger::error(const std::string& message) {
    if (minLogLevel_ <= LogLevel::Error) {
        writeLog(LogLevel::Error, message);
    }
}

void Logger::critical(const std::string& message) {
    if (minLogLevel_ <= LogLevel::Critical) {
        writeLog(LogLevel::Critical, message);
    }
}

void Logger::logException(const std::string& title, const std::exception& e) {
    std::string message = title + ": " + std::string(e.what());
    error(message);
}

void Logger::setShowTimestamp(bool enable) {
    showTimestamp_ = enable;
}

void Logger::writeLog(LogLevel level, const std::string& message) {
    std::string formattedMessage = getLevelString(level);
    
    if (showTimestamp_) {
        formattedMessage += " [" + getTimestamp() + "]";
    }
    
    formattedMessage += " " + message;

    // Use appropriate stream based on log level
    if (level >= LogLevel::Error) {
        *errorStream_ << formattedMessage << std::endl;
    } else {
        *outputStream_ << formattedMessage << std::endl;
    }
}

std::string Logger::getLevelString(LogLevel level) const {
    switch (level) {
        case LogLevel::Debug:
            return "[DEBUG]";
        case LogLevel::Info:
            return "[INFO]";
        case LogLevel::Warning:
            return "[WARNING]";
        case LogLevel::Error:
            return "[ERROR]";
        case LogLevel::Critical:
            return "[CRITICAL]";
        default:
            return "[UNKNOWN]";
    }
}

std::string Logger::getTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    ss << "." << std::setfill('0') << std::setw(3) << ms.count();
    
    return ss.str();
}

} // namespace yt::logging