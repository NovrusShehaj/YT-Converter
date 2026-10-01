#include "api_app.h"
#include "dependencies.h"
#include "metrics.h"
#include "process.h"
#include "test_helpers.h"

#include <cpprest/http_client.h>
#include <cpprest/json.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <thread>

using namespace web;
using namespace web::http;
using namespace web::http::client;

namespace {

json::value postBody(const std::string& url, const std::string& format) {
    json::value body;
    body[U("url")] = json::value::string(utility::conversions::to_string_t(url));
    body[U("format")] = json::value::string(utility::conversions::to_string_t(format));
    return body;
}

int countLines(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return 0;
    }
    int count = 0;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        ++count;
    }
    return count;
}

} // namespace

class ApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        yt::process::resetShutdownForTests();
        yt::deps::clearReadyCacheForTests();
        output_ = makeTestDir();
        setenv("YTCONV_FAKE_LOG_DIR", output_.string().c_str(), 1);
        yt::Config config;
        config.bind = "127.0.0.1";
        config.allow_unauthenticated_localhost = true;
        config.output_dir = output_.string();
        config.yt_dlp_path = fakePath("yt-dlp");
        config.ffmpeg_path = fakePath("ffmpeg");
        config.child_timeout_sec = 5;
        config.download_timeout_sec = 15;
        config.convert_timeout_sec = 15;
        config.max_concurrent = 1;
        config.queue_depth = 8;
        config.log_level = "ERROR";
        yt::applyLogConfig(config);

        std::exception_ptr last;
        for (int port = 18731; port < 18741; ++port) {
            config.port = port;
            try {
                server_ = std::make_unique<yt::api::ApiServer>(config);
                server_->start();
                base_ = "http://127.0.0.1:" + std::to_string(port);
                break;
            } catch (...) {
                last = std::current_exception();
                server_.reset();
            }
        }
        if (!server_) {
            if (last) {
                std::rethrow_exception(last);
            }
            FAIL() << "Unable to bind API test port";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    void TearDown() override {
        if (server_) {
            server_->stop();
        }
        unsetenv("YTCONV_FAKE_LOG_DIR");
        unsetenv("YTCONV_FAKE_SLEEP");
        yt::process::resetShutdownForTests();
    }

    http_response request(const method& verb, const std::string& path,
                          const json::value& body = json::value::null()) {
        http_client client(utility::conversions::to_string_t(base_));
        http_request req(verb);
        req.set_request_uri(utility::conversions::to_string_t(path));
        if (!body.is_null()) {
            req.set_body(body);
        }
        return client.request(req).get();
    }

    json::value waitJob(const std::string& jobId) {
        for (int attempt = 0; attempt < 80; ++attempt) {
            auto response = request(methods::GET, "/v1/jobs/" + jobId);
            EXPECT_EQ(response.status_code(), status_codes::OK);
            auto body = response.extract_json().get();
            const auto status = utility::conversions::to_utf8string(body.at(U("status")).as_string());
            if (status == "succeeded" || status == "failed" || status == "canceled") {
                return body;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        ADD_FAILURE() << "job did not finish";
        return json::value::null();
    }

    std::filesystem::path output_;
    std::unique_ptr<yt::api::ApiServer> server_;
    std::string base_;
};

TEST_F(ApiTest, HealthReadyAndContract) {
    auto health = request(methods::GET, "/v1/healthz");
    EXPECT_EQ(health.status_code(), status_codes::OK);
    EXPECT_EQ(health.extract_json().get().at(U("status")).as_string(), U("ok"));
    EXPECT_TRUE(health.headers().has(U("Cache-Control")));

    auto ready = request(methods::GET, "/v1/readyz");
    EXPECT_EQ(ready.status_code(), status_codes::OK);

    auto missing = request(methods::GET, "/");
    EXPECT_EQ(missing.status_code(), status_codes::NotFound);

    auto getConvert = request(methods::GET, "/v1/conversions");
    EXPECT_EQ(getConvert.status_code(), status_codes::MethodNotAllowed);

    auto badUrl = request(methods::POST, "/v1/conversions",
                          postBody("https://example.com/watch?v=dQw4w9WgXcQ", "mp3"));
    EXPECT_EQ(badUrl.status_code(), status_codes::BadRequest);

    auto ok = request(methods::POST, "/v1/conversions",
                      postBody("https://www.youtube.com/watch?v=dQw4w9WgXcQ", "mp3"));
    EXPECT_EQ(ok.status_code(), status_codes::Accepted);
    const auto queued = ok.extract_json().get();
    EXPECT_EQ(queued.at(U("status")).as_string(), U("queued"));
    EXPECT_TRUE(queued.has_field(U("job_id")));
    const auto job = waitJob(utility::conversions::to_utf8string(queued.at(U("job_id")).as_string()));
    EXPECT_EQ(job.at(U("status")).as_string(), U("succeeded"));
    EXPECT_TRUE(job.has_field(U("output_path")));
    EXPECT_TRUE(job.has_field(U("download_ms")));
    EXPECT_TRUE(job.has_field(U("convert_ms")));

    auto metrics = request(methods::GET, "/v1/metrics");
    EXPECT_EQ(metrics.status_code(), status_codes::OK);
}

TEST_F(ApiTest, RequiresApiKeyWhenConfigured) {
    server_->stop();
    yt::Config config;
    config.bind = "127.0.0.1";
    config.port = server_->port() + 20;
    config.api_key = "test-key-value";
    config.allow_unauthenticated_localhost = false;
    config.output_dir = output_.string();
    config.yt_dlp_path = fakePath("yt-dlp");
    config.ffmpeg_path = fakePath("ffmpeg");
    yt::validateApiConfig(config);
    server_ = std::make_unique<yt::api::ApiServer>(config);
    server_->start();
    base_ = "http://127.0.0.1:" + std::to_string(config.port);

    http_client client(utility::conversions::to_string_t(base_));
    http_request unauthorizedReq(methods::POST);
    unauthorizedReq.set_request_uri(U("/v1/conversions"));
    unauthorizedReq.set_body(postBody("https://www.youtube.com/watch?v=dQw4w9WgXcQ", "mp3"));
    EXPECT_EQ(client.request(unauthorizedReq).get().status_code(), status_codes::Unauthorized);

    http_request authorizedReq(methods::POST);
    authorizedReq.set_request_uri(U("/v1/conversions"));
    authorizedReq.set_body(postBody("https://www.youtube.com/watch?v=dQw4w9WgXcQ", "mp3"));
    authorizedReq.headers().add(U("X-Api-Key"), U("test-key-value"));
    EXPECT_EQ(client.request(authorizedReq).get().status_code(), status_codes::Accepted);
}

TEST_F(ApiTest, HealthAnswersWhileDownloadRuns) {
    server_->stop();
    yt::Config config = yt::Config{};
    config.bind = "127.0.0.1";
    config.port = server_->port() + 30;
    config.allow_unauthenticated_localhost = true;
    config.output_dir = output_.string();
    config.yt_dlp_path = fakePath("hang");
    config.ffmpeg_path = fakePath("ffmpeg");
    config.download_timeout_sec = 30;
    config.max_concurrent = 1;
    config.queue_depth = 4;
    config.log_level = "ERROR";
    yt::applyLogConfig(config);
    server_ = std::make_unique<yt::api::ApiServer>(config);
    server_->start();
    base_ = "http://127.0.0.1:" + std::to_string(config.port);

    auto queued = request(methods::POST, "/v1/conversions",
                          postBody("https://www.youtube.com/watch?v=dQw4w9WgXcQ", "mp3"));
    EXPECT_EQ(queued.status_code(), status_codes::Accepted);
    const auto started = std::chrono::steady_clock::now();
    auto health = request(methods::GET, "/v1/healthz");
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_EQ(health.status_code(), status_codes::OK);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 1000);
    const auto jobId =
        utility::conversions::to_utf8string(queued.extract_json().get().at(U("job_id")).as_string());
    EXPECT_EQ(request(methods::DEL, "/v1/jobs/" + jobId).status_code(), status_codes::OK);
}

TEST_F(ApiTest, QueueFullReturnsBusy) {
    server_->stop();
    yt::Config config;
    config.bind = "127.0.0.1";
    config.port = server_->port() + 40;
    config.allow_unauthenticated_localhost = true;
    config.output_dir = output_.string();
    config.yt_dlp_path = fakePath("hang");
    config.ffmpeg_path = fakePath("ffmpeg");
    config.download_timeout_sec = 30;
    config.max_concurrent = 1;
    config.queue_depth = 2;
    config.log_level = "ERROR";
    yt::applyLogConfig(config);
    server_ = std::make_unique<yt::api::ApiServer>(config);
    server_->start();
    base_ = "http://127.0.0.1:" + std::to_string(config.port);

    auto first = request(methods::POST, "/v1/conversions",
                         postBody("https://www.youtube.com/watch?v=dQw4w9WgXcQ", "mp3"));
    auto second = request(methods::POST, "/v1/conversions",
                          postBody("https://www.youtube.com/watch?v=jNQXAC9IVRw", "mp3"));
    auto third = request(methods::POST, "/v1/conversions",
                         postBody("https://www.youtube.com/watch?v=9bZkp7q19f0", "mp3"));
    EXPECT_EQ(first.status_code(), status_codes::Accepted);
    EXPECT_EQ(second.status_code(), status_codes::Accepted);
    EXPECT_EQ(third.status_code(), status_codes::ServiceUnavailable);
}

TEST_F(ApiTest, CancelOneJobLeavesTheOtherRunning) {
    server_->stop();
    setenv("YTCONV_FAKE_SLEEP", "1.5", 1);
    yt::Config config;
    config.bind = "127.0.0.1";
    config.port = server_->port() + 50;
    config.allow_unauthenticated_localhost = true;
    config.output_dir = output_.string();
    config.yt_dlp_path = fakePath("yt-dlp");
    config.ffmpeg_path = fakePath("ffmpeg");
    config.download_timeout_sec = 20;
    config.max_concurrent = 2;
    config.queue_depth = 4;
    config.log_level = "ERROR";
    yt::applyLogConfig(config);
    server_ = std::make_unique<yt::api::ApiServer>(config);
    server_->start();
    base_ = "http://127.0.0.1:" + std::to_string(config.port);

    auto first = request(methods::POST, "/v1/conversions",
                         postBody("https://www.youtube.com/watch?v=dQw4w9WgXcQ", "mp3"));
    auto second = request(methods::POST, "/v1/conversions",
                          postBody("https://www.youtube.com/watch?v=jNQXAC9IVRw", "wav"));
    ASSERT_EQ(first.status_code(), status_codes::Accepted);
    ASSERT_EQ(second.status_code(), status_codes::Accepted);
    const auto firstId =
        utility::conversions::to_utf8string(first.extract_json().get().at(U("job_id")).as_string());
    const auto secondId =
        utility::conversions::to_utf8string(second.extract_json().get().at(U("job_id")).as_string());
    EXPECT_EQ(request(methods::DEL, "/v1/jobs/" + firstId).status_code(), status_codes::OK);
    const auto canceled = waitJob(firstId);
    const auto finished = waitJob(secondId);
    EXPECT_EQ(canceled.at(U("status")).as_string(), U("canceled"));
    EXPECT_EQ(finished.at(U("status")).as_string(), U("succeeded"));
    unsetenv("YTCONV_FAKE_SLEEP");
}

TEST_F(ApiTest, RejectsOversizedBodyAndUnsafeRequestId) {
    std::string huge(9000, 'x');
    http_client client(utility::conversions::to_string_t(base_));
    http_request oversized(methods::POST);
    oversized.set_request_uri(U("/v1/conversions"));
    oversized.headers().add(U("Content-Type"), U("application/json"));
    oversized.set_body(utility::conversions::to_string_t(huge));
    EXPECT_EQ(client.request(oversized).get().status_code(), status_codes::BadRequest);

    http_request badId(methods::POST);
    badId.set_request_uri(U("/v1/conversions"));
    badId.headers().add(U("Content-Type"), U("application/json"));
    badId.headers().add(U("X-Request-Id"), U("bad\nid"));
    badId.set_body(postBody("https://www.youtube.com/watch?v=dQw4w9WgXcQ", "wav"));
    auto accepted = client.request(badId).get();
    EXPECT_EQ(accepted.status_code(), status_codes::Accepted);
    const auto requestId =
        utility::conversions::to_utf8string(accepted.extract_json().get().at(U("request_id")).as_string());
    EXPECT_EQ(requestId.find('\n'), std::string::npos);
    EXPECT_NE(requestId, "bad\nid");
}

TEST_F(ApiTest, ReadyCheckIsCached) {
    auto first = request(methods::GET, "/v1/readyz");
    auto second = request(methods::GET, "/v1/readyz");
    EXPECT_EQ(first.status_code(), status_codes::OK);
    EXPECT_EQ(second.status_code(), status_codes::OK);
    EXPECT_EQ(countLines(output_ / "yt-dlp.count"), 1);
}

TEST(ReadyCache, FailureIsNotCached) {
    yt::deps::clearReadyCacheForTests();
    const auto before = yt::metrics::global().ready_spawns.load();
    yt::Config config;
    config.yt_dlp_path = "/no/such/yt-dlp-ytconv";
    config.ffmpeg_path = "/no/such/ffmpeg-ytconv";
    config.ready_ttl_sec = 60;
    EXPECT_FALSE(yt::deps::checkToolsCached(config).ok);
    EXPECT_FALSE(yt::deps::checkToolsCached(config).ok);
    EXPECT_EQ(yt::metrics::global().ready_spawns.load(), before + 2);
}
