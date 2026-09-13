#include "error.h"
#include "job_limiter.h"

#include <gtest/gtest.h>

TEST(Error, MapsCodesToHttpAndExit) {
    EXPECT_EQ(yt::errorHttpStatus(yt::ErrorCode::InvalidUrl), 400);
    EXPECT_EQ(yt::errorHttpStatus(yt::ErrorCode::Unauthorized), 401);
    EXPECT_EQ(yt::errorHttpStatus(yt::ErrorCode::Busy), 503);
    EXPECT_EQ(yt::errorHttpStatus(yt::ErrorCode::Timeout), 504);
    EXPECT_EQ(yt::errorHttpStatus(yt::ErrorCode::DiskFull), 507);
    EXPECT_EQ(yt::errorExitCode(yt::ErrorCode::InvalidUrl), 2);
    EXPECT_EQ(yt::errorExitCode(yt::ErrorCode::BinaryNotFound), 3);
    EXPECT_EQ(yt::errorExitCode(yt::ErrorCode::DownloadFailed), 4);
    EXPECT_EQ(yt::errorExitCode(yt::ErrorCode::ConversionFailed), 5);
    EXPECT_EQ(yt::errorExitCode(yt::ErrorCode::Canceled), 130);
    EXPECT_STREQ(yt::errorCodeString(yt::ErrorCode::Ok), "ok");

    const yt::Error error(yt::ErrorCode::InvalidUrl, "bad url");
    EXPECT_EQ(error.httpStatus(), 400);
    EXPECT_STREQ(error.what(), "bad url");
}

TEST(JobLimiter, CapsAndReleasesSlots) {
    yt::JobLimiter limiter(1);
    auto first = limiter.tryAcquire();
    ASSERT_TRUE(first.has_value());
    EXPECT_FALSE(limiter.tryAcquire().has_value());
    first.reset();
    EXPECT_TRUE(limiter.tryAcquire().has_value());
}
