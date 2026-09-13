#include "validation.h"

#include <gtest/gtest.h>

using yt::ErrorCode;
using yt::validation::isValidVideoId;
using yt::validation::parseYouTubeUrl;

namespace {

struct Case {
    const char* url;
    bool ok;
    const char* id;
    yt::ErrorCode code;
};

} // namespace

TEST(Validation, AcceptsSupportedWatchAndShareUrls) {
    const Case cases[] = {
        {"https://www.youtube.com/watch?v=dQw4w9WgXcQ", true, "dQw4w9WgXcQ", ErrorCode::Ok},
        {"https://youtube.com/watch?v=dQw4w9WgXcQ", true, "dQw4w9WgXcQ", ErrorCode::Ok},
        {"http://www.youtube.com/watch?v=dQw4w9WgXcQ", true, "dQw4w9WgXcQ", ErrorCode::Ok},
        {"https://youtu.be/dQw4w9WgXcQ", true, "dQw4w9WgXcQ", ErrorCode::Ok},
        {"https://www.youtu.be/dQw4w9WgXcQ", true, "dQw4w9WgXcQ", ErrorCode::Ok},
        {"https://m.youtube.com/watch?v=dQw4w9WgXcQ", true, "dQw4w9WgXcQ", ErrorCode::Ok},
        {"https://music.youtube.com/watch?v=dQw4w9WgXcQ", true, "dQw4w9WgXcQ", ErrorCode::Ok},
        {"https://www.youtube.com/watch?v=dQw4w9WgXcQ&list=PLxxxx&index=1", true, "dQw4w9WgXcQ",
         ErrorCode::Ok},
        {"https://www.youtube.com/shorts/dQw4w9WgXcQ", true, "dQw4w9WgXcQ", ErrorCode::Ok},
        {"https://www.youtube.com/embed/dQw4w9WgXcQ", true, "dQw4w9WgXcQ", ErrorCode::Ok},
        {"https://www.youtube.com/live/dQw4w9WgXcQ", true, "dQw4w9WgXcQ", ErrorCode::Ok},
        {"  https://youtu.be/dQw4w9WgXcQ  ", true, "dQw4w9WgXcQ", ErrorCode::Ok},
    };

    for (const auto& item : cases) {
        const auto parsed = parseYouTubeUrl(item.url);
        EXPECT_TRUE(parsed.ok()) << item.url << " " << parsed.message;
        ASSERT_TRUE(parsed.video.has_value()) << item.url;
        EXPECT_EQ(parsed.video->id, item.id);
        EXPECT_EQ(parsed.video->canonical_url, "https://www.youtube.com/watch?v=dQw4w9WgXcQ");
    }
}

TEST(Validation, RejectsInjectionTraversalAndForeignHosts) {
    const Case cases[] = {
        {"", false, "", ErrorCode::InvalidUrl},
        {"https://example.com/watch?v=dQw4w9WgXcQ", false, "", ErrorCode::UnsupportedHost},
        {"https://evil.example/?q=https://www.youtube.com/watch?v=dQw4w9WgXcQ", false, "",
         ErrorCode::UnsupportedHost},
        {"https://www.youtube.com/watch?v=dQw4w9WgXcQ'; touch /tmp/pwned; echo '", false, "",
         ErrorCode::InvalidUrl},
        {"https://www.youtube.com/watch?v=abc/../x2345", false, "", ErrorCode::InvalidUrl},
        {"https://www.youtube.com/watch?v=abc/../../../tmp", false, "", ErrorCode::InvalidUrl},
        {"https://www.youtube.com/playlist?list=PLxxxxYYYYY", false, "", ErrorCode::PlaylistOnly},
        {"https://www.youtube.com/channel/UCxxxxxxxxxxxxxxxxxxxxxx", false, "", ErrorCode::ChannelUrl},
        {"https://www.youtube.com/@somechannel", false, "", ErrorCode::ChannelUrl},
        {"https://not-youtube.com/watch?v=dQw4w9WgXcQ", false, "", ErrorCode::UnsupportedHost},
        {"https://www.youtube.com.evil.test/watch?v=dQw4w9WgXcQ", false, "", ErrorCode::UnsupportedHost},
        {"https://user:pass@www.youtube.com/watch?v=dQw4w9WgXcQ", false, "", ErrorCode::InvalidUrl},
        {"ftp://www.youtube.com/watch?v=dQw4w9WgXcQ", false, "", ErrorCode::InvalidUrl},
    };

    for (const auto& item : cases) {
        const auto parsed = parseYouTubeUrl(item.url);
        EXPECT_FALSE(parsed.ok()) << item.url;
        EXPECT_EQ(parsed.code, item.code) << item.url;
    }
}

TEST(Validation, VideoIdAllowlist) {
    EXPECT_TRUE(isValidVideoId("dQw4w9WgXcQ"));
    EXPECT_TRUE(isValidVideoId("___________"));
    EXPECT_TRUE(isValidVideoId("a-b_CDE1234"));
    EXPECT_FALSE(isValidVideoId("short"));
    EXPECT_FALSE(isValidVideoId("dQw4w9WgXcQextra"));
    EXPECT_FALSE(isValidVideoId("abc/../x234"));
    EXPECT_FALSE(isValidVideoId("dQw4w9WgXcQ'"));
    EXPECT_FALSE(isValidVideoId("dQw4w9WgXc;"));
}

TEST(Validation, Formats) {
    EXPECT_TRUE(yt::validation::isValidFormat("mp3"));
    EXPECT_TRUE(yt::validation::isValidFormat("MP4"));
    EXPECT_TRUE(yt::validation::isValidFormat(" wav "));
    EXPECT_FALSE(yt::validation::isValidFormat("avi"));
    EXPECT_FALSE(yt::validation::isValidFormat(""));
    EXPECT_EQ(yt::validation::normalizeFormat("MP3"), "mp3");
}

TEST(Validation, InputHelper) {
    EXPECT_FALSE(yt::validation::validateConverterInput(
                     "https://www.youtube.com/watch?v=dQw4w9WgXcQ", "mp3")
                     .has_value());
    EXPECT_TRUE(yt::validation::validateConverterInput("https://example.com", "mp3").has_value());
    EXPECT_TRUE(yt::validation::validateConverterInput(
                    "https://www.youtube.com/watch?v=dQw4w9WgXcQ", "avi")
                    .has_value());
}
