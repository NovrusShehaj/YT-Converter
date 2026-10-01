// Network-free integration tests that run the real ffmpeg/ffprobe on locally generated media.
// The fake yt-dlp copies a fixture into the download template, so no YouTube request is made.
#include "converter.h"
#include "error.h"
#include "process.h"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool toolAvailable(const std::string& tool, const std::string& flag) {
    yt::process::RunOptions options;
    options.timeout_ms = 20000;
    const auto result = yt::process::run({tool, flag}, options);
    return !result.not_found && result.exit_code == 0;
}

bool mediaTestsRequired() {
    const char* value = std::getenv("YTCONV_REQUIRE_MEDIA_TESTS");
    return value != nullptr && std::string(value) == "1";
}

void runFfmpeg(const std::vector<std::string>& args) {
    std::vector<std::string> argv{"ffmpeg", "-y", "-nostdin", "-hide_banner", "-loglevel", "error"};
    argv.insert(argv.end(), args.begin(), args.end());
    yt::process::RunOptions options;
    options.timeout_ms = 60000;
    const auto result = yt::process::run(argv, options);
    ASSERT_EQ(result.exit_code, 0) << result.stderr_text;
}

// Returns key=value pairs from ffprobe. Stream keys are prefixed with the codec type, for example
// "audio.codec_name"; format keys are unprefixed.
std::map<std::string, std::string> probe(const fs::path& file) {
    std::map<std::string, std::string> values;
    yt::process::RunOptions options;
    options.timeout_ms = 30000;
    options.max_output_bytes = 65536;
    const auto result = yt::process::run(
        {"ffprobe", "-v", "error", "-show_entries",
         "format=format_name,duration:stream=codec_type,codec_name,sample_rate,channels", "-of",
         "default", file.string()},
        options);
    EXPECT_EQ(result.exit_code, 0) << result.stderr_text;
    std::map<std::string, std::string> section;
    std::size_t start = 0;
    const std::string& text = result.stdout_text;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string line = text.substr(start, end - start);
        start = end + 1;
        if (line == "[STREAM]" || line == "[FORMAT]") {
            section.clear();
        } else if (line == "[/STREAM]") {
            const std::string prefix = section["codec_type"] + ".";
            for (const auto& entry : section) {
                values[prefix + entry.first] = entry.second;
            }
        } else if (line == "[/FORMAT]") {
            for (const auto& entry : section) {
                values[entry.first] = entry.second;
            }
        } else if (const auto eq = line.find('='); eq != std::string::npos) {
            section[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    return values;
}

double durationOf(const std::map<std::string, std::string>& values) {
    const auto it = values.find("duration");
    if (it == values.end()) {
        return 0.0;
    }
    try {
        return std::stod(it->second);
    } catch (const std::exception&) {
        return 0.0;
    }
}

int countLinesIn(const fs::path& path) {
    return static_cast<int>(readLines(path).size());
}

} // namespace

class MediaTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        available_ = toolAvailable("ffmpeg", "-version") && toolAvailable("ffprobe", "-version");
        if (!available_) {
            return;
        }
        fixtures_ = makeTestDir();
        // Stereo AAC tone in M4A, like a typical YouTube audio-only stream.
        runFfmpeg({"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=1", "-ac",
                   "2", "-c:a", "aac", "-b:a", "96k", (fixtures_ / "tone.m4a").string()});
        // MP4-compatible codecs in Matroska exercise the stream-copy remux path.
        runFfmpeg({"-f", "lavfi", "-i", "testsrc=size=160x120:rate=10:duration=1", "-f", "lavfi",
                   "-i", "sine=frequency=440:duration=1", "-c:v", "mpeg4", "-c:a", "aac",
                   "-shortest", (fixtures_ / "clip.mkv").string()});
        // A complete MP4 exercises the bypass path that publishes without ffmpeg.
        runFfmpeg({"-f", "lavfi", "-i", "testsrc=size=160x120:rate=10:duration=1", "-f", "lavfi",
                   "-i", "sine=frequency=440:duration=1", "-c:v", "mpeg4", "-c:a", "aac",
                   "-shortest", (fixtures_ / "clip.mp4").string()});
    }

    void SetUp() override {
        if (!available_) {
            if (mediaTestsRequired()) {
                FAIL() << "YTCONV_REQUIRE_MEDIA_TESTS=1 but ffmpeg/ffprobe are not on PATH";
            }
            GTEST_SKIP() << "ffmpeg/ffprobe not on PATH";
        }
        yt::process::resetShutdownForTests();
        output_ = makeTestDir();
        setenv("YTCONV_FAKE_LOG_DIR", output_.string().c_str(), 1);
    }

    void TearDown() override {
        unsetenv("YTCONV_FAKE_LOG_DIR");
        unsetenv("YTCONV_FAKE_SOURCE");
    }

    yt::converter::ConversionRequest request(const std::string& format,
                                             const std::string& fixture) const {
        setenv("YTCONV_FAKE_SOURCE", (fixtures_ / fixture).string().c_str(), 1);
        yt::converter::ConversionRequest req;
        req.url = "https://www.youtube.com/watch?v=dQw4w9WgXcQ";
        req.format = format;
        req.config.output_dir = output_.string();
        req.config.yt_dlp_path = fakePath("yt-dlp");
        req.config.ffmpeg_path = fakePath("ffmpeg-real");
        req.config.download_timeout_sec = 60;
        req.config.convert_timeout_sec = 60;
        return req;
    }

    static bool available_;
    static fs::path fixtures_;
    fs::path output_;
};

bool MediaTest::available_ = false;
fs::path MediaTest::fixtures_;

TEST_F(MediaTest, Mp3IsValidStereo44k) {
    const auto result = yt::converter::processVideo(request("mp3", "tone.m4a"));
    ASSERT_TRUE(fs::exists(result.output_path));
    EXPECT_GT(fs::file_size(result.output_path), 0u);
    const auto info = probe(result.output_path);
    EXPECT_EQ(info.at("format_name"), "mp3");
    EXPECT_EQ(info.at("audio.codec_name"), "mp3");
    EXPECT_EQ(info.at("audio.sample_rate"), "44100");
    EXPECT_EQ(info.at("audio.channels"), "2");
    EXPECT_GT(durationOf(info), 0.5);
    EXPECT_EQ(countLinesIn(output_ / "ffmpeg.count"), 1);
    EXPECT_EQ(countPartialFiles(output_), 0);
}

TEST_F(MediaTest, WavIsPcm16Stereo44k) {
    const auto result = yt::converter::processVideo(request("wav", "tone.m4a"));
    const auto info = probe(result.output_path);
    EXPECT_EQ(info.at("format_name"), "wav");
    EXPECT_EQ(info.at("audio.codec_name"), "pcm_s16le");
    EXPECT_EQ(info.at("audio.sample_rate"), "44100");
    EXPECT_EQ(info.at("audio.channels"), "2");
    EXPECT_GT(durationOf(info), 0.5);
    EXPECT_EQ(countPartialFiles(output_), 0);
}

TEST_F(MediaTest, NonMp4SourceIsRemuxedToMp4) {
    const auto result = yt::converter::processVideo(request("mp4", "clip.mkv"));
    const auto info = probe(result.output_path);
    EXPECT_NE(info.at("format_name").find("mp4"), std::string::npos);
    EXPECT_EQ(info.at("video.codec_name"), "mpeg4");
    EXPECT_EQ(info.at("audio.codec_name"), "aac");
    EXPECT_GT(durationOf(info), 0.5);
    EXPECT_EQ(countLinesIn(output_ / "ffmpeg.count"), 1);
    EXPECT_EQ(countPartialFiles(output_), 0);
}

TEST_F(MediaTest, ValidMp4BypassesFfmpeg) {
    const auto result = yt::converter::processVideo(request("mp4", "clip.mp4"));
    const auto info = probe(result.output_path);
    EXPECT_NE(info.at("format_name").find("mp4"), std::string::npos);
    EXPECT_GT(durationOf(info), 0.5);
    EXPECT_EQ(readFile(result.output_path), readFile(fixtures_ / "clip.mp4"));
    EXPECT_EQ(countLinesIn(output_ / "ffmpeg.count"), 0);
    EXPECT_EQ(countPartialFiles(output_), 0);
}

TEST_F(MediaTest, FailedRealEncodeKeepsPreviousOutputAndRemovesPartial) {
    const auto first = yt::converter::processVideo(request("mp3", "tone.m4a"));
    const std::string previous = readFile(first.output_path);
    ASSERT_FALSE(previous.empty());

    // A corrupt source with force+refresh forces a new download and a real ffmpeg failure.
    writeFile(output_ / "corrupt.m4a", "this is not media");
    auto replacement = request("mp3", "tone.m4a");
    setenv("YTCONV_FAKE_SOURCE", (output_ / "corrupt.m4a").string().c_str(), 1);
    replacement.config.force = true;
    replacement.refresh = true;
    EXPECT_THROW(
        {
            try {
                yt::converter::processVideo(replacement);
            } catch (const yt::Error& error) {
                EXPECT_EQ(error.code(), yt::ErrorCode::ConversionFailed);
                throw;
            }
        },
        yt::Error);
    EXPECT_EQ(readFile(first.output_path), previous);
    EXPECT_EQ(countPartialFiles(output_), 0);
}
