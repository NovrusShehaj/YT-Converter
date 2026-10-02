// GoogleTest must precede cpprest: cpprest defines a U() macro that breaks gtest templates.
#include <gtest/gtest.h>

#include "api_app.h"
#include "dependencies.h"
#include "metrics.h"
#include "process.h"
#include "test_helpers.h"

#include <cpprest/http_client.h>
#include <cpprest/json.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <thread>

using namespace web;
using namespace web::http;
using namespace web::http::client;
namespace fs = std::filesystem;

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
                          const json::value& body = json::value::null(),
                          const std::string& apiKey = {}) {
        http_client client(utility::conversions::to_string_t(base_));
        http_request req(verb);
        req.set_request_uri(utility::conversions::to_string_t(path));
        if (!apiKey.empty()) {
            req.headers().add(U("X-Api-Key"), utility::conversions::to_string_t(apiKey));
        }
        if (!body.is_null()) {
            req.set_body(body);
        }
        return client.request(req).get();
    }

    // Replaces the running server with one using `config`, trying a range of local ports.
    void restartServer(yt::Config config) {
        if (server_) {
            server_->stop();
            server_.reset();
        }
        yt::applyLogConfig(config);
        std::exception_ptr last;
        for (int port = 18800; port < 18900; ++port) {
            config.port = port;
            try {
                server_ = std::make_unique<yt::api::ApiServer>(config);
                server_->start();
                base_ = "http://127.0.0.1:" + std::to_string(port);
                return;
            } catch (...) {
                last = std::current_exception();
                server_.reset();
            }
        }
        if (last) {
            std::rethrow_exception(last);
        }
    }

    yt::Config baseConfig() const {
        yt::Config config;
        config.bind = "127.0.0.1";
        config.allow_unauthenticated_localhost = true;
        config.output_dir = output_.string();
        config.yt_dlp_path = fakePath("yt-dlp");
        config.ffmpeg_path = fakePath("ffmpeg");
        config.download_timeout_sec = 20;
        config.convert_timeout_sec = 20;
        config.max_concurrent = 1;
        config.queue_depth = 8;
        config.log_level = "ERROR";
        return config;
    }

    static std::string field(const json::value& body, const char* name) {
        const auto key = utility::conversions::to_string_t(name);
        if (!body.is_object() || !body.has_field(key) || !body.at(key).is_string()) {
            return {};
        }
        return utility::conversions::to_utf8string(body.at(key).as_string());
    }

    json::value waitJob(const std::string& jobId) {
        for (int attempt = 0; attempt < 80; ++attempt) {
            auto response = request(methods::GET, "/v1/jobs/" + jobId);
            EXPECT_EQ(response.status_code(), status_codes::OK);
            auto body = response.extract_json().get();
            const auto status =
                utility::conversions::to_utf8string(body.at(U("status")).as_string());
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
    const auto job =
        waitJob(utility::conversions::to_utf8string(queued.at(U("job_id")).as_string()));
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
    const auto jobId = utility::conversions::to_utf8string(
        queued.extract_json().get().at(U("job_id")).as_string());
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
    const auto secondId = utility::conversions::to_utf8string(
        second.extract_json().get().at(U("job_id")).as_string());
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
    // Header values cannot carry raw newlines on the wire; use other unsafe characters.
    badId.headers().add(U("X-Request-Id"), U("bad id;<x>"));
    badId.set_body(postBody("https://www.youtube.com/watch?v=dQw4w9WgXcQ", "wav"));
    auto accepted = client.request(badId).get();
    EXPECT_EQ(accepted.status_code(), status_codes::Accepted);
    const auto requestId = utility::conversions::to_utf8string(
        accepted.extract_json().get().at(U("request_id")).as_string());
    EXPECT_EQ(requestId.find(' '), std::string::npos);
    EXPECT_NE(requestId, "bad id;<x>");
}

TEST_F(ApiTest, ReadyCheckIsCached) {
    auto first = request(methods::GET, "/v1/readyz");
    auto second = request(methods::GET, "/v1/readyz");
    EXPECT_EQ(first.status_code(), status_codes::OK);
    EXPECT_EQ(second.status_code(), status_codes::OK);
    EXPECT_EQ(countLines(output_ / "yt-dlp.version.count"), 1);
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

TEST_F(ApiTest, JobRoutesEnforceConfiguredApiKey) {
    const std::string key = "job-route-key-1234";
    setenv("YTCONV_FAKE_SLEEP", "30", 1);
    auto config = baseConfig();
    config.api_key = key;
    config.allow_unauthenticated_localhost = false;
    yt::validateApiConfig(config);
    restartServer(config);

    auto missingPost = request(methods::POST, "/v1/conversions",
                               postBody("https://www.youtube.com/watch?v=dQw4w9WgXcQ", "mp3"));
    EXPECT_EQ(missingPost.status_code(), status_codes::Unauthorized);

    auto created = request(methods::POST, "/v1/conversions",
                           postBody("https://www.youtube.com/watch?v=dQw4w9WgXcQ", "mp3"), key);
    ASSERT_EQ(created.status_code(), status_codes::Accepted);
    const std::string jobId = field(created.extract_json().get(), "job_id");
    ASSERT_FALSE(jobId.empty());

    // Wait until the (sleeping) download child is running.
    std::string status;
    for (int attempt = 0; attempt < 100 && status != "running"; ++attempt) {
        status = field(request(methods::GET, "/v1/jobs/" + jobId, json::value::null(), key)
                           .extract_json()
                           .get(),
                       "status");
        if (status != "running") {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    ASSERT_EQ(status, "running");

    for (const std::string& badKey : {std::string(), std::string("wrong-key-value")}) {
        for (const auto& verb : {methods::GET, methods::DEL}) {
            auto denied = request(verb, "/v1/jobs/" + jobId, json::value::null(), badKey);
            EXPECT_EQ(denied.status_code(), status_codes::Unauthorized) << verb;
            const auto body = denied.extract_json().get();
            EXPECT_EQ(field(body, "error_code"), "unauthorized");
            EXPECT_FALSE(body.has_field(U("output_path")));
            EXPECT_FALSE(body.has_field(U("job_id")));
            EXPECT_FALSE(body.has_field(U("video_id")));
        }
        // An unknown ID is indistinguishable from an existing one without the key.
        EXPECT_EQ(request(methods::GET, "/v1/jobs/no-such-job", json::value::null(), badKey)
                      .status_code(),
                  status_codes::Unauthorized);
        EXPECT_EQ(request(methods::DEL, "/v1/jobs/no-such-job", json::value::null(), badKey)
                      .status_code(),
                  status_codes::Unauthorized);
    }

    // Unauthorized DELETEs left the job and its child untouched.
    auto still = request(methods::GET, "/v1/jobs/" + jobId, json::value::null(), key);
    ASSERT_EQ(still.status_code(), status_codes::OK);
    EXPECT_EQ(field(still.extract_json().get(), "status"), "running");
    EXPECT_EQ(countLines(output_ / "yt-dlp.count"), 1);

    EXPECT_EQ(request(methods::GET, "/v1/jobs/no-such-job", json::value::null(), key).status_code(),
              status_codes::NotFound);
    EXPECT_EQ(request(methods::DEL, "/v1/jobs/no-such-job", json::value::null(), key).status_code(),
              status_codes::NotFound);
    EXPECT_EQ(request(methods::DEL, "/v1/jobs/" + jobId, json::value::null(), key).status_code(),
              status_codes::OK);
    EXPECT_EQ(request(methods::DEL, "/v1/jobs/" + jobId, json::value::null(), key).status_code(),
              status_codes::Conflict);

    // Probes stay public.
    EXPECT_EQ(request(methods::GET, "/v1/healthz").status_code(), status_codes::OK);
    EXPECT_NE(request(methods::GET, "/v1/readyz").status_code(), status_codes::Unauthorized);
}

TEST_F(ApiTest, UnauthenticatedLocalhostModeKeepsJobRoutesOpen) {
    EXPECT_EQ(request(methods::GET, "/v1/jobs/no-such-job").status_code(), status_codes::NotFound);
    auto created = request(methods::POST, "/v1/conversions",
                           postBody("https://www.youtube.com/watch?v=dQw4w9WgXcQ", "wav"));
    ASSERT_EQ(created.status_code(), status_codes::Accepted);
    const std::string jobId = field(created.extract_json().get(), "job_id");
    const auto job = waitJob(jobId);
    EXPECT_EQ(field(job, "status"), "succeeded");
    EXPECT_EQ(request(methods::DEL, "/v1/jobs/" + jobId).status_code(), status_codes::Conflict);
}

TEST_F(ApiTest, RefreshAndForceAreIndependentOverHttp) {
    auto config = baseConfig();
    config.ffmpeg_path = fakePath("ffmpeg-copy");
    restartServer(config);
    const std::string url = "https://www.youtube.com/watch?v=dQw4w9WgXcQ";
    const auto finalPath = output_ / "dQw4w9WgXcQ.mp3";
    auto convert = [&](const char* content, const char* flag) {
        setenv("YTCONV_FAKE_CONTENT", content, 1);
        auto body = postBody(url, "mp3");
        if (flag != nullptr) {
            body[utility::conversions::to_string_t(flag)] = json::value::boolean(true);
        }
        auto response = request(methods::POST, "/v1/conversions", body);
        const auto status = response.status_code();
        const auto json = response.extract_json().get();
        if (status == status_codes::Accepted) {
            return std::make_pair(status, waitJob(field(json, "job_id")));
        }
        return std::make_pair(status, json);
    };

    auto first = convert("FIRST", nullptr);
    ASSERT_EQ(first.first, status_codes::Accepted);
    EXPECT_EQ(readFile(finalPath), "OUT:FIRST");

    auto reused = convert("IGNORED", nullptr);
    EXPECT_EQ(reused.first, status_codes::OK);
    EXPECT_TRUE(reused.second.at(U("reused")).as_bool());

    auto forced = convert("IGNORED", "force");
    ASSERT_EQ(forced.first, status_codes::Accepted);
    EXPECT_EQ(field(forced.second, "status"), "succeeded");
    EXPECT_FALSE(forced.second.at(U("reused")).as_bool());
    EXPECT_EQ(readFile(finalPath), "OUT:FIRST"); // re-encoded from the cached source
    EXPECT_EQ(countLines(output_ / "yt-dlp.count"), 1);
    EXPECT_EQ(countLines(output_ / "ffmpeg.count"), 2);

    auto refreshed = convert("THIRD", "refresh");
    ASSERT_EQ(refreshed.first, status_codes::Accepted);
    EXPECT_EQ(field(refreshed.second, "status"), "succeeded");
    EXPECT_EQ(readFile(finalPath), "OUT:THIRD"); // new source generation
    EXPECT_EQ(countLines(output_ / "yt-dlp.count"), 2);
    unsetenv("YTCONV_FAKE_CONTENT");
}

// ---------------------------------------------------------------------------------------------
// Synchronous mode (finding 7): same queue, limits, sharing, and shutdown behavior as async.

class SyncApiTest : public ApiTest {
  protected:
    void SetUp() override {
        ApiTest::SetUp();
        gate_ = makeTestDir();
        setenv("YTCONV_YTDLP_GATE_DIR", gate_.string().c_str(), 1);
        auto config = baseConfig();
        config.sync_conversions = true;
        config.max_concurrent = 1;
        config.queue_depth = 2;
        restartServer(config);
    }

    void TearDown() override {
        openGate(gate_);
        unsetenv("YTCONV_YTDLP_GATE_DIR");
        ApiTest::TearDown();
    }

    pplx::task<http_response> postAsync(const std::string& video, const std::string& format) {
        auto client = std::make_shared<http_client>(utility::conversions::to_string_t(base_));
        http_request req(methods::POST);
        req.set_request_uri(U("/v1/conversions"));
        req.set_body(postBody("https://www.youtube.com/watch?v=" + video, format));
        return client->request(req).then([client](http_response response) { return response; });
    }

    fs::path gate_;
};

TEST_F(SyncApiTest, SyncRequestsObeyOperationAndChildLimits) {
    auto first = postAsync("dQw4w9WgXcQ", "mp3");
    ASSERT_TRUE(waitUntil([&] { return gateStartedPids(gate_).size() == 1; }));
    auto second = postAsync("jNQXAC9IVRw", "mp3");
    // Give the second request time to be admitted (it queues behind the single worker).
    ASSERT_TRUE(waitUntil([&] {
        auto metrics = request(methods::GET, "/v1/metrics").extract_json().get();
        return metrics.has_field(U("operations_queued")) &&
               metrics.at(U("operations_queued")).as_integer() == 1;
    }));
    // Queue depth 2 is exhausted: a third distinct sync request is rejected immediately.
    const auto rejectStarted = std::chrono::steady_clock::now();
    auto third = postAsync("9bZkp7q19f0", "mp3").get();
    EXPECT_EQ(third.status_code(), status_codes::ServiceUnavailable);
    EXPECT_LT(std::chrono::steady_clock::now() - rejectStarted, std::chrono::seconds(2));

    // Health stays responsive while sync responses are outstanding.
    const auto healthStarted = std::chrono::steady_clock::now();
    EXPECT_EQ(request(methods::GET, "/v1/healthz").status_code(), status_codes::OK);
    EXPECT_LT(std::chrono::steady_clock::now() - healthStarted, std::chrono::seconds(1));
    // Only one child ever ran at a time.
    EXPECT_EQ(gateStartedPids(gate_).size(), 1u);

    openGate(gate_);
    auto firstResponse = first.get();
    auto secondResponse = second.get();
    EXPECT_EQ(firstResponse.status_code(), status_codes::OK);
    EXPECT_EQ(secondResponse.status_code(), status_codes::OK);
    const auto body = firstResponse.extract_json().get();
    EXPECT_EQ(field(body, "status"), "success");
    EXPECT_FALSE(field(body, "output_path").empty());
    EXPECT_TRUE(body.has_field(U("download_ms")));
}

TEST_F(SyncApiTest, SyncAndAsyncSubscribersShareOneOperation) {
    auto sync = postAsync("dQw4w9WgXcQ", "mp3");
    ASSERT_TRUE(waitUntil([&] { return gateStartedPids(gate_).size() == 1; }));
    // An async-mode server is not needed: a second sync request attaches to the same operation.
    auto attached = postAsync("dQw4w9WgXcQ", "mp3");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    openGate(gate_);
    EXPECT_EQ(sync.get().status_code(), status_codes::OK);
    EXPECT_EQ(attached.get().status_code(), status_codes::OK);
    EXPECT_EQ(countLines(output_ / "yt-dlp.count"), 1);
    EXPECT_EQ(countLines(output_ / "ffmpeg.count"), 1);
}

TEST_F(SyncApiTest, SyncFailuresKeepTypedStatusCodes) {
    openGate(gate_);
    auto config = baseConfig();
    config.sync_conversions = true;
    config.yt_dlp_path = fakePath("fail");
    restartServer(config);
    auto failed = postAsync("dQw4w9WgXcQ", "mp3").get();
    EXPECT_EQ(failed.status_code(), status_codes::InternalError);
    EXPECT_EQ(field(failed.extract_json().get(), "error_code"), "download_failed");

    config.yt_dlp_path = "/no/such/yt-dlp-ytconv";
    restartServer(config);
    auto missing = postAsync("dQw4w9WgXcQ", "wav").get();
    EXPECT_EQ(missing.status_code(), status_codes::ServiceUnavailable);
    EXPECT_EQ(field(missing.extract_json().get(), "error_code"), "binary_not_found");
}

TEST_F(SyncApiTest, ShutdownResolvesOutstandingSyncResponses) {
    auto outstanding = postAsync("dQw4w9WgXcQ", "mp3");
    ASSERT_TRUE(waitUntil([&] { return gateStartedPids(gate_).size() == 1; }));
    const auto started = std::chrono::steady_clock::now();
    server_->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(10));
    auto response = outstanding.get();
    EXPECT_EQ(response.status_code(), status_codes::ServiceUnavailable);
    server_.reset();
}

// ---------------------------------------------------------------------------------------------
// Request bodies (finding 9): bounded reads at the listener boundary, including chunked and slow
// input. Raw sockets make partial and malformed traffic possible.

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

class RawClient {
  public:
    explicit RawClient(int port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<std::uint16_t>(port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        connected_ = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }
    ~RawClient() { close(); }
    RawClient(const RawClient&) = delete;
    RawClient& operator=(const RawClient&) = delete;

    bool connected() const { return connected_; }

    bool send(const std::string& data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    // Reads until the peer closes or the timeout passes.
    std::string readAll(std::chrono::milliseconds timeout, bool* closed = nullptr) {
        std::string data;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd pfd{fd_, POLLIN, 0};
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now())
                                  .count();
            if (::poll(&pfd, 1, static_cast<int>(std::max<long long>(1, left))) <= 0) {
                continue;
            }
            char buffer[4096];
            const ssize_t n = ::recv(fd_, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                if (closed != nullptr) {
                    *closed = true;
                }
                return data;
            }
            data.append(buffer, static_cast<std::size_t>(n));
        }
        return data;
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

  private:
    int fd_ = -1;
    bool connected_ = false;
};

int statusOf(const std::string& response) {
    if (response.rfind("HTTP/1.1 ", 0) != 0 || response.size() < 12) {
        return 0;
    }
    return std::stoi(response.substr(9, 3));
}

std::string jsonOfSize(std::size_t size) {
    std::string json = R"({"url":"https://www.youtube.com/watch?v=dQw4w9WgXcQ","format":"mp3")";
    json += std::string(size - json.size() - 1, ' ');
    json += "}";
    return json;
}

std::string postHead(const std::string& extraHeaders) {
    return "POST /v1/conversions HTTP/1.1\r\nHost: 127.0.0.1\r\n"
           "Content-Type: application/json\r\n" +
           extraHeaders + "\r\n";
}

} // namespace

class GateTest : public ApiTest {
  protected:
    void SetUp() override {
        ApiTest::SetUp();
        setenv("YTCONV_YTDLP_GATE_DIR", makeTestDir().string().c_str(), 1);
        auto config = baseConfig();
        config.request_read_timeout_sec = 1;
        config.max_pending_reads = 4;
        config.max_connections = 16;
        restartServer(config);
        port_ = std::stoi(base_.substr(base_.rfind(':') + 1));
    }

    void TearDown() override {
        unsetenv("YTCONV_YTDLP_GATE_DIR");
        ApiTest::TearDown();
    }

    std::string exchange(const std::string& request,
                         std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        RawClient client(port_);
        EXPECT_TRUE(client.connected());
        client.send(request);
        return client.readAll(timeout);
    }

    bool nothingEnqueued() {
        auto metrics = this->request(methods::GET, "/v1/metrics").extract_json().get();
        return metrics.at(U("operations_queued")).as_integer() == 0 &&
               metrics.at(U("operations_running")).as_integer() == 0 &&
               !fs::exists(output_ / "yt-dlp.count");
    }

    int port_ = 0;
};

TEST_F(GateTest, ExactLimitBodyIsAcceptedAndOneMoreByteIsRejected) {
    const std::string exact = jsonOfSize(8192);
    ASSERT_EQ(exact.size(), 8192u);
    const auto accepted = exchange(postHead("Content-Length: 8192\r\n") + exact);
    EXPECT_EQ(statusOf(accepted), 202) << accepted;

    const std::string over = jsonOfSize(8193);
    const auto rejected = exchange(postHead("Content-Length: 8193\r\n") + over);
    EXPECT_EQ(statusOf(rejected), 400) << rejected;
    EXPECT_NE(rejected.find("Request body exceeds 8 KB"), std::string::npos);
}

TEST_F(GateTest, OversizedKnownLengthIsRejectedBeforeTheBodyArrives) {
    RawClient client(port_);
    const auto started = std::chrono::steady_clock::now();
    client.send(postHead("Content-Length: 10000000000\r\n")); // no body bytes at all
    bool closed = false;
    const auto response = client.readAll(std::chrono::seconds(3), &closed);
    EXPECT_EQ(statusOf(response), 400) << response;
    EXPECT_TRUE(closed);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
    EXPECT_LT(server_->requestStats().peak_request_bytes, 16384u + 4096u);
    EXPECT_TRUE(nothingEnqueued());
}

TEST_F(GateTest, ChunkedBodiesAreDecodedWithABoundedBuffer) {
    const std::string json = jsonOfSize(100);
    std::ostringstream chunked;
    chunked << std::hex << 0x40 << "\r\n" << json.substr(0, 0x40) << "\r\n";
    chunked << std::hex << (json.size() - 0x40) << ";ext=1\r\n" << json.substr(0x40) << "\r\n";
    chunked << "0\r\nX-Trailer: ok\r\n\r\n";
    const auto accepted = exchange(postHead("Transfer-Encoding: chunked\r\n") + chunked.str());
    EXPECT_EQ(statusOf(accepted), 202) << accepted;

    // Keep streaming 1 KiB chunks; the gate must answer 400 once 8 KiB is exceeded, without
    // buffering the rest.
    RawClient client(port_);
    client.send(postHead("Transfer-Encoding: chunked\r\n"));
    const std::string piece = "400\r\n" + std::string(1024, 'x') + "\r\n";
    for (int i = 0; i < 1024; ++i) {
        if (!client.send(piece)) {
            break; // the gate closed the connection
        }
    }
    const auto response = client.readAll(std::chrono::seconds(3));
    EXPECT_EQ(statusOf(response), 400) << response;
    EXPECT_LE(server_->requestStats().peak_request_bytes, 8192u + 4096u + 1024u);

    // A single huge declared chunk is rejected from its size line.
    const auto huge = exchange(postHead("Transfer-Encoding: chunked\r\n") + "FFFFFFFFFF\r\n");
    EXPECT_EQ(statusOf(huge), 400) << huge;
}

TEST_F(GateTest, SlowAndIncompleteBodiesTimeOutWhileHealthStaysResponsive) {
    RawClient slow(port_);
    slow.send(postHead("Content-Length: 100\r\n") + "{\"url\":");
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(request(methods::GET, "/v1/healthz").status_code(), status_codes::OK);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(500));
    bool closed = false;
    const auto response = slow.readAll(std::chrono::seconds(5), &closed);
    EXPECT_EQ(statusOf(response), 408) << response;
    EXPECT_TRUE(closed);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(3));
    EXPECT_TRUE(nothingEnqueued());
}

TEST_F(GateTest, MalformedFramingIsRejected) {
    const std::vector<std::string> heads = {
        "Content-Length: abc\r\n",
        "Content-Length: -1\r\n",
        "Content-Length: 1, 2\r\n",
        "Content-Length: 10\r\nContent-Length: 10\r\n",
        "Content-Length: 10\r\nTransfer-Encoding: chunked\r\n",
        "Transfer-Encoding: gzip\r\n",
        " folded: header\r\n",
    };
    for (const auto& head : heads) {
        const auto response = exchange(postHead(head) + "0123456789");
        EXPECT_EQ(statusOf(response), 400) << head << response;
    }
    const auto badChunk = exchange(postHead("Transfer-Encoding: chunked\r\n") + "zz\r\n");
    EXPECT_EQ(statusOf(badChunk), 400) << badChunk;
    const auto badLine = exchange("NOT-HTTP\r\n\r\n");
    EXPECT_EQ(statusOf(badLine), 400) << badLine;
    const auto bigHead =
        exchange("GET /v1/healthz HTTP/1.1\r\nX-Big: " + std::string(20000, 'a') + "\r\n\r\n");
    EXPECT_EQ(statusOf(bigHead), 431) << bigHead.substr(0, 100);
    EXPECT_TRUE(nothingEnqueued());
}

TEST_F(GateTest, DisconnectMidBodyEnqueuesNothing) {
    const auto before = server_->requestStats().client_disconnects;
    {
        RawClient client(port_);
        client.send(postHead("Content-Length: 100\r\n") + std::string(50, ' '));
    }
    ASSERT_TRUE(waitUntil([&] { return server_->requestStats().client_disconnects > before; }));
    EXPECT_TRUE(nothingEnqueued());
}

TEST_F(GateTest, PendingReadsAreBoundedAndReleasedByTimeout) {
    std::vector<std::unique_ptr<RawClient>> slow;
    for (int i = 0; i < 4; ++i) {
        slow.push_back(std::make_unique<RawClient>(port_));
        slow.back()->send(postHead("Content-Length: 100\r\n"));
    }
    ASSERT_TRUE(waitUntil([&] { return server_->requestStats().pending_reads == 4; }));
    const auto rejected = exchange("GET /v1/healthz HTTP/1.1\r\n\r\n");
    EXPECT_EQ(statusOf(rejected), 503) << rejected;
    // The read timeout frees the slots; health works again.
    ASSERT_TRUE(waitUntil([&] { return server_->requestStats().pending_reads == 0; },
                          std::chrono::seconds(5)));
    EXPECT_EQ(request(methods::GET, "/v1/healthz").status_code(), status_codes::OK);
}

TEST_F(GateTest, ShutdownClosesHalfReadRequests) {
    RawClient slow(port_);
    slow.send(postHead("Content-Length: 100\r\n") + "{");
    ASSERT_TRUE(waitUntil([&] { return server_->requestStats().pending_reads == 1; }));
    const auto started = std::chrono::steady_clock::now();
    server_->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(5));
    bool closed = false;
    slow.readAll(std::chrono::seconds(2), &closed);
    EXPECT_TRUE(closed);
    server_.reset();
}

TEST_F(GateTest, InternalListenerRejectsRequestsThatBypassTheGate) {
    http_client direct(utility::conversions::to_string_t(
        "http://127.0.0.1:" + std::to_string(server_->internalPortForTests())));
    http_request req(methods::GET);
    req.set_request_uri(U("/v1/healthz"));
    EXPECT_EQ(direct.request(req).get().status_code(), status_codes::Forbidden);
}
#endif
