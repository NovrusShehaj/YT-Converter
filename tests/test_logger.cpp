#include "logger.h"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <thread>
#include <vector>

TEST(Logger, ConcurrentWritesAreWholeLines) {
    const auto dir = makeTestDir();
    const auto logPath = dir / "logger.log";
    auto& logger = yt::logger::Logger::getInstance();
    logger.setConsoleOutput(false);
    logger.setFileOutput(true, logPath.string());
    logger.setLogLevel(yt::logger::LogLevel::INFO);
    logger.setLogFormat(yt::logger::LogFormat::Text);

    constexpr int threads = 8;
    constexpr int perThread = 40;
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&logger, t]() {
            for (int i = 0; i < perThread; ++i) {
                logger.info("thread-" + std::to_string(t) + "-message-" + std::to_string(i));
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    const std::string text = readFile(logPath);
    std::size_t lines = 0;
    std::size_t pos = 0;
    while (true) {
        const auto next = text.find('\n', pos);
        if (next == std::string::npos) {
            break;
        }
        ++lines;
        pos = next + 1;
    }
    EXPECT_EQ(lines, static_cast<std::size_t>(threads * perThread));

    logger.setLogFormat(yt::logger::LogFormat::Json);
    logger.info("json-line");
    const std::string jsonLog = readFile(logPath);
    EXPECT_NE(jsonLog.find("\"msg\":\"json-line\""), std::string::npos);
    logger.setFileOutput(false);
    logger.setConsoleOutput(true);
    logger.setLogFormat(yt::logger::LogFormat::Text);
}

TEST(Logger, ParsesLevels) {
    EXPECT_EQ(yt::logger::parseLogLevel("debug"), yt::logger::LogLevel::DEBUG);
    EXPECT_EQ(yt::logger::parseLogLevel("INFO"), yt::logger::LogLevel::INFO);
    EXPECT_EQ(yt::logger::parseLogFormat("json"), yt::logger::LogFormat::Json);
}
