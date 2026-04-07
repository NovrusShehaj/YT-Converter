#include "logger.h"
#include <iomanip>

namespace yt::logger {

Logger& Logger::getInstance() {
    static Logger instance;
    return instance;
}

Logger::Logger() 
    : currentLogLevel(LogLevel::INFO), 
      consoleOutputEnabled(true),
      fileOutputEnabled(false),
      logFileName("converter.log") {
}

Logger::~Logger() {
    if (logFile && logFile->is_open()) {
        logFile->close();
    }
}

void Logger::setLogLevel(LogLevel level) {
    currentLogLevel = level;
}

LogLevel Logger::getLogLevel() const {
    return currentLogLevel;
}

void Logger::setConsoleOutput(bool enabled) {
    consoleOutputEnabled = enabled;
}

void Logger::setFileOutput(bool enabled, const std::string& filename) {
    fileOutputEnabled = enabled;
    logFileName = filename;
    
    if (enabled) {
        logFile = std::make_unique<std::ofstream>(filename, std::ios::app);
    } else if (logFile) {
        logFile->close();
        logFile.reset();
    }
}

void Logger::debug(const std::string& message) {
    log(LogLevel::DEBUG, message);
}

void Logger::info(const std::string& message) {
    log(LogLevel::INFO, message);
}

void Logger::warning(const std::string& message) {
    log(LogLevel::WARNING, message);
}

void Logger::error(const std::string& message) {
    log(LogLevel::ERROR, message);
}

void Logger::critical(const std::string& message) {
    log(LogLevel::CRITICAL, message);
}

void Logger::log(LogLevel level, const std::string& message) {
    if (level < currentLogLevel) {
        return;
    }
    
    std::string formattedMessage = formatMessage(level, message);
    
    if (consoleOutputEnabled) {
        if (level >= LogLevel::ERROR) {
            std::cerr << formattedMessage << std::endl;
        } else {
            std::cout << formattedMessage << std::endl;
        }
    }
    
    if (fileOutputEnabled && logFile && logFile->is_open()) {
        *logFile << formattedMessage << std::endl;
        logFile->flush();
    }
}

std::string Logger::levelToString(LogLevel level) const {
    switch (level) {
        case LogLevel::DEBUG:    return "DEBUG";
        case LogLevel::INFO:     return "INFO";
        case LogLevel::WARNING:  return "WARNING";
        case LogLevel::ERROR:    return "ERROR";
        case LogLevel::CRITICAL: return "CRITICAL";
        default:                 return "UNKNOWN";
    }
}

std::string Logger::getCurrentTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    ss << "." << std::setfill('0') << std::setw(3) << ms.count();
    
    return ss.str();
}

std::string Logger::formatMessage(LogLevel level, const std::string& message) const {
    std::stringstream ss;
    ss << "[" << getCurrentTimestamp() << "] "
       << "[" << levelToString(level) << "] "
       << message;
    return ss.str();
}

} // namespace yt::logger