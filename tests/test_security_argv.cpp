#include "converter.h"
#include "test_helpers.h"
#include "validation.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

TEST(Security, InjectionSuffixNeverReachesYtDlp) {
    EXPECT_FALSE(yt::validation::isValidURL(
        "https://www.youtube.com/watch?v=dQw4w9WgXcQ'; touch /tmp/pwned; echo '"));
    EXPECT_FALSE(yt::validation::isValidURL(
        "https://evil.example/?q=https://www.youtube.com/watch?v=dQw4w9WgXcQ"));
}

TEST(Security, ReconstructedUrlIsTheOnlyDownloadTarget) {
    const auto output = makeTestDir();
    setenv("YTCONV_FAKE_LOG_DIR", output.string().c_str(), 1);
    yt::converter::ConversionRequest request;
    request.url = "https://www.youtube.com/watch?v=dQw4w9WgXcQ&list=PLinjection";
    request.format = "mp3";
    request.config.output_dir = output.string();
    request.config.yt_dlp_path = fakePath("yt-dlp");
    request.config.ffmpeg_path = fakePath("ffmpeg");
    request.config.child_timeout_sec = 5;
    yt::converter::processVideo(request);
    unsetenv("YTCONV_FAKE_LOG_DIR");

    const std::string argv = readFile(output / "yt-dlp.argv");
    EXPECT_NE(argv.find("--no-playlist"), std::string::npos);
    EXPECT_NE(argv.find("https://www.youtube.com/watch?v=dQw4w9WgXcQ"), std::string::npos);
    EXPECT_EQ(argv.find("PLinjection"), std::string::npos);
    EXPECT_EQ(argv.find(';'), std::string::npos);
}

TEST(Security, OutputNameCannotEscapeRoot) {
    EXPECT_EQ(yt::converter::getOutputFilename("a_b-CDE1234", "wav").find('/'), std::string::npos);
    EXPECT_EQ(yt::converter::getOutputFilename("a_b-CDE1234", "wav").find(".."), std::string::npos);
}
