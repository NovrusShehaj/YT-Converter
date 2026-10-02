#include "process.h"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <signal.h>
#endif

#ifndef YTCONV_OUTPUT_WRITER
#define YTCONV_OUTPUT_WRITER ""
#endif

namespace {

struct Lines {
    std::vector<std::string> lines;
    std::size_t maxPayload = 0;
};

void collect(const std::string& line, void* user) {
    auto* lines = static_cast<Lines*>(user);
    lines->maxPayload = std::max(lines->maxPayload, line.size());
    lines->lines.push_back(line);
}

yt::process::RunOptions callbackOptions(Lines& lines) {
    yt::process::RunOptions options;
    options.timeout_ms = 30000;
    options.max_output_bytes = 8192;
    options.max_line_bytes = 8192;
    options.on_line = collect;
    options.on_line_user = &lines;
    return options;
}

std::vector<std::string> writer(std::vector<std::string> actions) {
    actions.insert(actions.begin(), YTCONV_OUTPUT_WRITER);
    return actions;
}

} // namespace

TEST(Process, EchoPreservesMetacharactersAsSeparateArguments) {
    const auto dir = makeTestDir();
    const auto script = fakePath("yt-dlp");
    ASSERT_TRUE(std::filesystem::exists(script));

    setenv("YTCONV_FAKE_LOG_DIR", dir.string().c_str(), 1);
    const auto result = yt::process::run(
        {script, "-o", (dir / "output.%(ext)s").string(), "https://example.test/'$; touch"});
    unsetenv("YTCONV_FAKE_LOG_DIR");

    EXPECT_EQ(result.exit_code, 0);
    EXPECT_FALSE(result.not_found);
    EXPECT_FALSE(result.timed_out);
    const std::string argv = readFile(dir / "yt-dlp.argv");
    EXPECT_NE(argv.find("https://example.test/'$; touch"), std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(dir / "output.mp4"));
}

TEST(Process, MissingBinaryIsNotFound) {
    const auto result = yt::process::run({"/this/binary/does/not/exist-ytconv"});
    EXPECT_TRUE(result.not_found);
    EXPECT_EQ(result.exit_code, 127);
}

TEST(Process, NonZeroExitIsPreserved) {
    const auto result = yt::process::run({fakePath("fail")});
    EXPECT_EQ(result.exit_code, 1);
    EXPECT_NE(result.stderr_text.find("simulated failure"), std::string::npos);
}

TEST(Process, TimeoutKillsHungChild) {
    yt::process::RunOptions options;
    options.timeout_ms = 400;
    const auto started = std::chrono::steady_clock::now();
    const auto result = yt::process::run({fakePath("hang")}, options);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_TRUE(result.timed_out);
    EXPECT_LT(elapsed, std::chrono::seconds(5));
}

TEST(Process, LargeStdoutCompletesWithoutTimeout) {
    auto* fake = new char[1024 * 1024 + 1];
    std::memset(fake, 'A', 1024 * 1024);
    std::string megabyte(reinterpret_cast<char*>(fake), 1024 * 1024);
    delete[] fake;
    const auto dir = makeTestDir();
    const auto script = (dir / "writer.sh");
    {
        std::ofstream in(script);
        in << "#!/bin/bash\n" << "printf '%s' '" << megabyte << "'\n";
    }
    std::filesystem::permissions(script, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    yt::process::RunOptions options;
    options.timeout_ms = 5000;
    options.capture_stdout = true;
    options.capture_stderr = false;
    options.max_output_bytes = 1024 * 1024 + 16;
    const auto result = yt::process::run({script.string()}, options);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_FALSE(result.timed_out);
    EXPECT_GE(result.stdout_text.size(), 1024 * 1024u);
}

TEST(Process, TrueUtilityExitsZero) {
    // PATH lookup: macOS has /usr/bin/true, not /bin/true.
    const auto result = yt::process::run({"true"});
    EXPECT_EQ(result.exit_code, 0);
}

TEST(ProcessLines, MultiMegabyteNewlineFreeOutputIsBounded) {
    Lines lines;
    auto options = callbackOptions(lines);
    const auto result = yt::process::run(writer({"out:4194304:A", "err:4194304:B"}), options);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_FALSE(result.timed_out);
    // One truncated callback per stream, each exactly the line bound.
    ASSERT_EQ(lines.lines.size(), 2u);
    EXPECT_EQ(lines.maxPayload, 8192u);
    EXPECT_LE(result.peak_line_bytes, 8192u);
    EXPECT_LE(result.stdout_text.size(), 8192u);
    EXPECT_LE(result.stderr_text.size(), 8192u);
    for (const auto& line : lines.lines) {
        EXPECT_TRUE(line == std::string(8192, 'A') || line == std::string(8192, 'B'));
    }
}

TEST(ProcessLines, GiantLineIsTruncatedOnceThenProgressParses) {
    Lines lines;
    auto options = callbackOptions(lines);
    const auto result = yt::process::run(
        writer({"out:1000000:x", "outline:", "outline:[download]  42.0% of 10.00MiB"}), options);
    EXPECT_EQ(result.exit_code, 0);
    ASSERT_EQ(lines.lines.size(), 2u);
    EXPECT_EQ(lines.lines[0], std::string(8192, 'x'));
    EXPECT_EQ(lines.lines[1], "[download]  42.0% of 10.00MiB");
    EXPECT_LE(result.peak_line_bytes, 8192u);
}

TEST(ProcessLines, FinalUnterminatedLineFollowsTheSameBound) {
    Lines lines;
    auto options = callbackOptions(lines);
    const auto result = yt::process::run(writer({"outline:first", "out:20000:y"}), options);
    EXPECT_EQ(result.exit_code, 0);
    ASSERT_EQ(lines.lines.size(), 2u);
    EXPECT_EQ(lines.lines[0], "first");
    EXPECT_EQ(lines.lines[1], std::string(8192, 'y'));
}

TEST(ProcessLines, BothStreamsDeliverLinesAndCrIsDropped) {
    Lines lines;
    auto options = callbackOptions(lines);
    const auto result = yt::process::run(writer({"outline:alpha\r", "errline:beta"}), options);
    EXPECT_EQ(result.exit_code, 0);
    ASSERT_EQ(lines.lines.size(), 2u);
    EXPECT_NE(std::find(lines.lines.begin(), lines.lines.end(), "alpha"), lines.lines.end());
    EXPECT_NE(std::find(lines.lines.begin(), lines.lines.end(), "beta"), lines.lines.end());
}

#ifndef _WIN32
TEST(ProcessLines, ThrowingCallbackReapsChild) {
    const auto dir = makeTestDir();
    const auto pidFile = dir / "child.pid";
    yt::process::RunOptions options;
    options.timeout_ms = 30000;
    options.on_line = [](const std::string&, void*) {
        throw std::runtime_error("callback failed");
    };
    const auto started = std::chrono::steady_clock::now();
    EXPECT_THROW(
        yt::process::run(writer({"pidfile:" + pidFile.string(), "outline:boom", "sleep:30000"}),
                         options),
        std::runtime_error);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(10));
    const pid_t pid = static_cast<pid_t>(std::stol(readFile(pidFile)));
    // ESRCH proves the child was killed and reaped (a zombie would still accept signal 0).
    EXPECT_EQ(::kill(pid, 0), -1);
    EXPECT_EQ(errno, ESRCH);
}
#endif

TEST(ProcessLines, TimeoutWithEndlessOutputCompletesPromptly) {
    Lines lines;
    auto options = callbackOptions(lines);
    options.timeout_ms = 500;
    const auto started = std::chrono::steady_clock::now();
    const auto result = yt::process::run(writer({"spam:z"}), options);
    EXPECT_TRUE(result.timed_out);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(10));
    EXPECT_LE(lines.maxPayload, 8192u);
    EXPECT_LE(result.peak_line_bytes, 8192u);
    EXPECT_LE(result.stdout_text.size(), 8192u);
}

TEST(ProcessLines, CancellationWithEndlessOutputCompletesPromptly) {
    Lines lines;
    auto options = callbackOptions(lines);
    options.cancel = std::make_shared<std::atomic<bool>>(false);
    std::thread canceller([cancel = options.cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        cancel->store(true);
    });
    const auto started = std::chrono::steady_clock::now();
    const auto result = yt::process::run(writer({"spam:q"}), options);
    canceller.join();
    EXPECT_TRUE(result.canceled);
    EXPECT_FALSE(result.timed_out);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(10));
    EXPECT_LE(lines.maxPayload, 8192u);
}

TEST(ProcessLines, ProgressLinesAreDelivered) {
    Lines lines;
    auto options = callbackOptions(lines);
    const auto result = yt::process::run(writer({"outline:[download]  50.0% of ~100MB"}), options);
    EXPECT_EQ(result.exit_code, 0);
    ASSERT_EQ(lines.lines.size(), 1u);
    EXPECT_EQ(lines.lines[0], "[download]  50.0% of ~100MB");
}
