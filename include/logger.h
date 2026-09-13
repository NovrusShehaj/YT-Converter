#ifndef YT_CONVERTER_LOGGER_H
#define YT_CONVERTER_LOGGER_H

#include <fstream>
#include <memory>
#include <mutex>
#include <string>

namespace yt::logger {

enum class LogLevel { DEBUG, INFO, WARNING, ERROR, CRITICAL };

enum class LogFormat { Text, Json };

struct LogContext {
    std::string request_id;
    std::string video_id;
};

class Logger {
public:
    static Logger& getInstance();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    void debug(const std::string& message);
    void info(const std::string& message);
    void warning(const std::string& message);
    void error(const std::string& message);
    void critical(const std::string& message);

    void setLogLevel(LogLevel level);
    void setLogFormat(LogFormat format);
    void setConsoleOutput(bool enabled);
    void setFileOutput(bool enabled, const std::string& filename = "converter.log");
    LogLevel getLogLevel() const;
    void setContext(const LogContext& context);
    void clearContext();

private:
    Logger();
    ~Logger();

    void log(LogLevel level, const std::string& message);
    std::string levelToString(LogLevel level) const;
    std::string getCurrentTimestamp() const;
    std::string formatMessage(LogLevel level, const std::string& message,
                              const LogContext& context) const;

    mutable std::mutex mutex_;
    LogLevel currentLogLevel;
    LogFormat currentLogFormat;
    bool consoleOutputEnabled;
    bool fileOutputEnabled;
    std::string logFileName;
    std::unique_ptr<std::ofstream> logFile;
};

LogLevel parseLogLevel(const std::string& value);
LogFormat parseLogFormat(const std::string& value);

} // namespace yt::logger

#endif // YT_CONVERTER_LOGGER_H
