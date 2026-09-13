#include "api_app.h"
#include "process.h"
#include "test_helpers.h"

#include <cpprest/http_client.h>
#include <cpprest/json.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <exception>
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

} // namespace

class ApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        yt::process::resetShutdownForTests();
        output_ = makeTestDir();
        setenv("YTCONV_FAKE_LOG_DIR", output_.string().c_str(), 1);
        yt::Config config;
        config.bind = "127.0.0.1";
        config.allow_unauthenticated_localhost = true;
        config.output_dir = output_.string();
        config.yt_dlp_path = fakePath("yt-dlp");
        config.ffmpeg_path = fakePath("ffmpeg");
        config.child_timeout_sec = 5;
        config.max_concurrent = 1;
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
    EXPECT_EQ(ok.status_code(), status_codes::OK);
    const auto body = ok.extract_json().get();
    EXPECT_EQ(body.at(U("status")).as_string(), U("success"));
    EXPECT_EQ(body.at(U("error_code")).as_string(), U("ok"));
    EXPECT_TRUE(body.has_field(U("output_path")));
    EXPECT_TRUE(body.has_field(U("job_id")));

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
    EXPECT_EQ(client.request(authorizedReq).get().status_code(), status_codes::OK);
}
