#include "process.h"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <utility>

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
    std::filesystem::permissions(script, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    const auto result = yt::process::run({script.string()}, {
        .timeout_ms = 5000,
        .capture_stdout = true,
        .capture_stderr = false,
        .max_output_bytes = 1024 * 1024 + 16,
    });
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_FALSE(result.timed_out);
    EXPECT_GE(result.stdout_text.size(), 1024 * 1024u);
}

struct CallbackData {
    std::string* line = nullptr;
    bool* received = nullptr;
};

TEST(Process, ProgressLinesAreDelivered) {
    // Use a small Python script that prints a progress line then exits
    const auto dir = makeTestDir();
    const auto script = (dir / "progress.py");
    {
        std::ofstream in(script);
        in << "#!/usr/bin/env python3\n";
        in << "import sys\n";
        in << "print('[download] 50%% of ~100MB')\n";
        in << "sys.exit(0)\n";
    }
    std::filesystem::permissions(script, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    
    std::string capturedLine;
    bool lineReceived = false;

    yt::process::RunOptions opts;
    opts.timeout_ms = 5000;
    opts.capture_stdout = true;
    opts.capture_stderr = false;
    opts.max_output_bytes = 8192;
    opts.on_line = [](const std::string& line, void* user) {
        auto* d = static_cast<CallbackData*>(user);
        if (d->line) d->line->assign(line);
        if (d->received) *d->received = true;
    };
    auto* userData = new CallbackData;
    userData->line = &capturedLine;
    userData->received = &lineReceived;
    opts.on_line_user = reinterpret_cast<void*>(userData);

    const auto result = yt::process::run({script.string()}, opts);

    auto* d = static_cast<CallbackData*>(userData);
    delete d;
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_FALSE(result.timed_out);
    // The callback should have been called
    EXPECT_TRUE(lineReceived && !capturedLine.empty());
}

TEST(Process, TrueUtilityExitsZero) {
    const auto result = yt::process::run({"/bin/true"});
    EXPECT_EQ(result.exit_code, 0);
}
