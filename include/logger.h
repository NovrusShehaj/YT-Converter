#ifndef YT_CONVERTER_LOGGER_H
#define YT_CONVERTER_LOGGER_H

#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <memory>

namespace yt::logger {

/**
 * @enum LogLevel
 * @brief Enumeration of logging levels
 */
enum class LogLevel {
    DEBUG,      ///< Debug messages for development
    INFO,       ///< General informational messages
    WARNING,    ///< Warning messages for potentially problematic situations
    ERROR,      ///< Error messages for failures
    CRITICAL    ///< Critical error messages for severe failures
};

/**
 * @class Logger
 * @brief Singleton logger for the YouTube converter
 * 
 * Provides thread-safe logging to console and optionally to file.
 * Uses the singleton pattern to ensure only one logger instance exists.
 * 
 * Usage:
 *   Logger::getInstance().debug("Debug message");
 *   Logger::getInstance().info("Info message");
 *   Logger::getInstance().warning("Warning message");
 *   Logger::getInstance().error("Error message");
 *   Logger::getInstance().critical("Critical message");
 */
class Logger {
public:
    /**
     * @brief Get the singleton instance of the logger
     * @return Reference to the logger instance
     */
    static Logger& getInstance();

    /**
     * @brief Delete copy constructor
     */
    Logger(const Logger&) = delete;

    /**
     * @brief Delete copy assignment operator
     */
    Logger& operator=(const Logger&) = delete;

    /**
     * @brief Delete move constructor
     */
    Logger(Logger&&) = delete;

    /**
     * @brief Delete move assignment operator
     */
    Logger& operator=(Logger&&) = delete;

    /**
     * @brief Log a debug message
     * @param message The message to log
     */
    void debug(const std::string& message);

    /**
     * @brief Log an info message
     * @param message The message to log
     */
    void info(const std::string& message);

    /**
     * @brief Log a warning message
     * @param message The message to log
     */
    void warning(const std::string& message);

    /**
     * @brief Log an error message
     * @param message The message to log
     */
    void error(const std::string& message);

    /**
     * @brief Log a critical error message
     * @param message The message to log
     */
    void critical(const std::string& message);

    /**
     * @brief Set the logging level
     * @param level The minimum level to log (messages below this level are ignored)
     */
    void setLogLevel(LogLevel level);

    /**
     * @brief Enable or disable console output
     * @param enabled true to enable console output, false to disable
     */
    void setConsoleOutput(bool enabled);

    /**
     * @brief Enable or disable file output
     * @param enabled true to enable file output, false to disable
     * @param filename The log file path (default: converter.log)
     */
    void setFileOutput(bool enabled, const std::string& filename = "converter.log");

    /**
     * @brief Get the current log level
     * @return The current LogLevel
     */
    LogLevel getLogLevel() const;

private:
    /**
     * @brief Private constructor for singleton pattern
     */
    Logger();

    /**
     * @brief Private destructor
     */
    ~Logger();

    /**
     * @brief Internal logging method
     * @param level The log level
     * @param message The message to log
     */
    void log(LogLevel level, const std::string& message);

    /**
     * @brief Convert LogLevel to string representation
     * @param level The log level
     * @return String representation of the log level
     */
    std::string levelToString(LogLevel level) const;

    /**
     * @brief Get the current timestamp
     * @return Formatted timestamp string
     */
    std::string getCurrentTimestamp() const;

    /**
     * @brief Format a log message with timestamp, level, and content
     * @param level The log level
     * @param message The message content
     * @return Formatted log message
     */
    std::string formatMessage(LogLevel level, const std::string& message) const;

    // Member variables
    LogLevel currentLogLevel;
    bool consoleOutputEnabled;
    bool fileOutputEnabled;
    std::string logFileName;
    std::unique_ptr<std::ofstream> logFile;
};

} // namespace yt::logger

#endif // YT_CONVERTER_LOGGER_H