#include "converter.h"
#include "error.h"
#include "process.h"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>

namespace {

yt::Config testConfig(const std::filesystem::path& output, const std::string& ytdlp,
                      const std::string& ffmpeg) {
    yt::Config config;
    config.output_dir = output.string();
    config.yt_dlp_path = ytdlp;
    config.ffmpeg_path = ffmpeg;
    config.child_timeout_sec = 5;
    config.reuse_completed = true;
    config.force = false;
    return config;
}

} // namespace

class ConverterTest : public ::testing::Test {
protected:
    void SetUp() override {
        yt::process::resetShutdownForTests();
        output_ = makeTestDir();
        setenv("YTCONV_FAKE_LOG_DIR", output_.string().c_str(), 1);
    }

    void TearDown() override {
        unsetenv("YTCONV_FAKE_LOG_DIR");
        yt::process::resetShutdownForTests();
    }

    std::filesystem::path output_;
};

TEST_F(ConverterTest, SuccessWritesFinalFileAndRemovesTemp) {
    yt::converter::ConversionRequest request;
    request.url = "https://youtu.be/dQw4w9WgXcQ";
    request.format = "mp3";
    request.config = testConfig(output_, fakePath("yt-dlp"), fakePath("ffmpeg"));

    const auto result = yt::converter::processVideo(request);
    EXPECT_EQ(result.video_id, "dQw4w9WgXcQ");
    EXPECT_TRUE(std::filesystem::exists(result.output_path));
    EXPECT_EQ(readFile(result.output_path), "FAKEOUT");
    EXPECT_FALSE(std::filesystem::exists(output_ / "jobs" / result.job_id / "source.mp4"));

    const std::string ytdlpArgv = readFile(output_ / "yt-dlp.argv");
    EXPECT_NE(ytdlpArgv.find("--no-playlist"), std::string::npos);
    EXPECT_NE(ytdlpArgv.find("https://www.youtube.com/watch?v=dQw4w9WgXcQ"), std::string::npos);
    EXPECT_NE(ytdlpArgv.find("bestaudio/best"), std::string::npos);

    const std::string ffmpegArgv = readFile(output_ / "ffmpeg.argv");
    EXPECT_NE(ffmpegArgv.find("-y"), std::string::npos);
    EXPECT_NE(ffmpegArgv.find("-nostdin"), std::string::npos);
    EXPECT_NE(ffmpegArgv.find("libmp3lame"), std::string::npos);
}

TEST_F(ConverterTest, Mp4UsesVideoFormatSelector) {
    yt::converter::ConversionRequest request;
    request.url = "https://www.youtube.com/watch?v=dQw4w9WgXcQ";
    request.format = "mp4";
    request.config = testConfig(output_, fakePath("yt-dlp"), fakePath("ffmpeg"));
    yt::converter::processVideo(request);
    const std::string ytdlpArgv = readFile(output_ / "yt-dlp.argv");
    EXPECT_NE(ytdlpArgv.find("bestvideo"), std::string::npos);
}

TEST_F(ConverterTest, ReuseSkipsSecondDownload) {
    yt::converter::ConversionRequest request;
    request.url = "https://www.youtube.com/watch?v=dQw4w9WgXcQ";
    request.format = "wav";
    request.config = testConfig(output_, fakePath("yt-dlp"), fakePath("ffmpeg"));
    yt::converter::processVideo(request);
    const std::string firstCount = readFile(output_ / "yt-dlp.count");
    const auto second = yt::converter::processVideo(request);
    EXPECT_TRUE(second.reused);
    EXPECT_EQ(readFile(output_ / "yt-dlp.count"), firstCount);
}

TEST_F(ConverterTest, FailedFfmpegCleansTempAndOutput) {
    yt::converter::ConversionRequest request;
    request.url = "https://www.youtube.com/watch?v=dQw4w9WgXcQ";
    request.format = "mp3";
    request.config = testConfig(output_, fakePath("yt-dlp"), fakePath("fail"));
    EXPECT_THROW(
        {
            try {
                yt::converter::processVideo(request);
            } catch (const yt::Error& error) {
                EXPECT_EQ(error.code(), yt::ErrorCode::ConversionFailed);
                throw;
            }
        },
        yt::Error);
    EXPECT_TRUE(std::filesystem::is_empty(output_ / "jobs") ||
                !std::filesystem::exists(output_ / "dQw4w9WgXcQ.mp3"));
    EXPECT_FALSE(std::filesystem::exists(output_ / "dQw4w9WgXcQ.mp3"));
}

TEST_F(ConverterTest, TimeoutIsClassified) {
    yt::converter::ConversionRequest request;
    request.url = "https://www.youtube.com/watch?v=dQw4w9WgXcQ";
    request.format = "mp3";
    request.config = testConfig(output_, fakePath("hang"), fakePath("ffmpeg"));
    request.config.child_timeout_sec = 1;
    EXPECT_THROW(
        {
            try {
                yt::converter::processVideo(request);
            } catch (const yt::Error& error) {
                EXPECT_EQ(error.code(), yt::ErrorCode::Timeout);
                throw;
            }
        },
        yt::Error);
}

TEST_F(ConverterTest, MissingBinaryIsClassified) {
    yt::converter::ConversionRequest request;
    request.url = "https://www.youtube.com/watch?v=dQw4w9WgXcQ";
    request.format = "mp3";
    request.config = testConfig(output_, "/no/such/yt-dlp-ytconv", fakePath("ffmpeg"));
    EXPECT_THROW(
        {
            try {
                yt::converter::processVideo(request);
            } catch (const yt::Error& error) {
                EXPECT_EQ(error.code(), yt::ErrorCode::BinaryNotFound);
                throw;
            }
        },
        yt::Error);
}

TEST_F(ConverterTest, OutputFilenameNeverContainsTraversal) {
    EXPECT_EQ(yt::converter::getOutputFilename("dQw4w9WgXcQ", "mp3"), "dQw4w9WgXcQ.mp3");
    EXPECT_THROW(yt::converter::getOutputFilename("../etc/passwd", "mp3"), yt::Error);
    EXPECT_THROW(yt::converter::getOutputFilename("abc/defghij", "mp3"), yt::Error);
}
