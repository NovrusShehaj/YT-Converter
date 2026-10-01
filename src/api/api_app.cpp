#include "api_app.h"
#include "converter.h"
#include "dependencies.h"
#include "error.h"
#include "job_limiter.h"
#include "job_queue.h"
#include "logger.h"
#include "metrics.h"
#include "process.h"

#include <cpprest/http_listener.h>
#include <cpprest/json.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>
#include <utility>

using namespace web;
using namespace web::http;
using namespace web::http::experimental::listener;

namespace yt::api {
namespace {

std::atomic<bool> g_shutdown{false};
std::atomic<bool> g_dumpMetrics{false};

std::string toUtf8(const utility::string_t& value) {
    return utility::conversions::to_utf8string(value);
}

utility::string_t toT(const std::string& value) {
    return utility::conversions::to_string_t(value);
}

std::string normalizePath(std::string path) {
    if (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

std::string makeRequestId() {
    std::random_device device;
    std::mt19937 rng(device());
    std::uniform_int_distribution<unsigned int> dist(0, 0xffffffffu);
    std::ostringstream ss;
    ss << std::hex << dist(rng);
    return ss.str();
}

bool isSafeJobId(const std::string& id) {
    if (id.empty() || id.size() > 80) {
        return false;
    }
    for (char ch : id) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && ch != '_' && ch != '-') {
            return false;
        }
    }
    return true;
}

bool isLoopbackRemote(const std::string& remote) {
    return remote.find("127.0.0.1") != std::string::npos || remote.find("::1") != std::string::npos ||
           remote == "localhost";
}

json::value errorBody(const yt::Error& error) {
    json::value body;
    body[U("status")] = json::value::string(U("error"));
    body[U("error_code")] = json::value::string(toT(error.codeString()));
    body[U("message")] = json::value::string(toT(error.message()));
    return body;
}

json::value errorBody(ErrorCode code, const std::string& message) {
    return errorBody(Error(code, message));
}

void replyJson(const http_request& request, int status, const json::value& body) {
    http_response response(static_cast<status_code>(status));
    response.headers().add(U("Cache-Control"), U("no-store"));
    response.headers().add(U("Content-Type"), U("application/json"));
    response.set_body(body);
    request.reply(response);
}

void onSignal(int) { g_shutdown.store(true); }

#ifndef _WIN32
void onUsr1(int) { g_dumpMetrics.store(true); }
#endif

} // namespace

void requestApiShutdown() { g_shutdown.store(true); }

bool apiShutdownRequested() { return g_shutdown.load(); }

class ApiServer::Impl {
public:
    explicit Impl(Config cfg) : config(std::move(cfg)), limiter(config.max_concurrent) {}

    Config config;
    JobLimiter limiter;
    jobs::Queue jobQueue;
    std::unique_ptr<http_listener> listener;
    std::atomic<bool> running{false};

    std::string url() const {
        if (config.bind.find(':') != std::string::npos && config.bind != "localhost") {
            return "http://[" + config.bind + "]:" + std::to_string(config.port);
        }
        return "http://" + config.bind + ":" + std::to_string(config.port);
    }

    bool authorize(const http_request& request) const {
        if (config.api_key.empty()) {
            return true;
        }
        const auto& headers = request.headers();
        if (!headers.has(U("X-Api-Key"))) {
            return false;
        }
        return constantTimeEquals(toUtf8(headers.find(U("X-Api-Key"))->second), config.api_key);
    }

    bool outputWritable() const {
        try {
            const std::string root = resolveOutputRoot(config.output_dir);
            const auto probe = std::filesystem::path(root) / ".ytconv-write-test";
            {
                std::ofstream out(probe);
                if (!out) {
                    return false;
                }
                out << "ok";
            }
            std::filesystem::remove(probe);
            return true;
        } catch (...) {
            return false;
        }
    }

    void handle(http_request request) {
        auto& logger = yt::logger::Logger::getInstance();
        const std::string path = normalizePath(toUtf8(request.relative_uri().path()));
        const auto method = request.method();
        std::string requestId = makeRequestId();
        if (request.headers().has(U("X-Request-Id"))) {
            requestId = toUtf8(request.headers().find(U("X-Request-Id"))->second);
            // Only accept safe characters: A-Za-z0-9_- and max 64 chars
            if (!std::all_of(requestId.begin(), requestId.end(), [](char c) {
                    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
                }) || requestId.size() > 64) {
                requestId = makeRequestId();
            }
        }
        logger.setContext({requestId, {}});

        if (g_shutdown.load()) {
            replyJson(request, 503, errorBody(ErrorCode::Busy, "Server is shutting down"));
            logger.clearContext();
            return;
        }

        try {
            if (path == "/v1/healthz" && method == methods::GET) {
                json::value body;
                body[U("status")] = json::value::string(U("ok"));
                replyJson(request, 200, body);
                logger.clearContext();
                return;
            }
            if (path.rfind("/v1/jobs/", 0) == 0 && method == methods::GET) {
                const std::string jobId = path.substr(strlen("/v1/jobs/"));
                if (!isSafeJobId(jobId)) {
                    replyJson(request, 404, errorBody(ErrorCode::InvalidInput, "Not found"));
                    logger.clearContext();
                    return;
                }
                handleJobStatus(request, jobId);
                logger.clearContext();
                return;
            }
            if (path.rfind("/v1/jobs/", 0) == 0 && method == methods::DELETE) {
                const std::string jobId = path.substr(strlen("/v1/jobs/"));
                if (!isSafeJobId(jobId)) {
                    replyJson(request, 404, errorBody(ErrorCode::InvalidInput, "Not found"));
                    logger.clearContext();
                    return;
                }
                handleJobCancel(request, jobId);
                logger.clearContext();
                return;
            }
            if (path == "/v1/readyz" && method == methods::GET) {
                const auto tools = yt::deps::checkToolsCached(config);
                if (!tools.ok || !outputWritable()) {
                    const std::string message =
                        tools.ok ? "Output directory is not writable" : tools.message;
                    replyJson(request, 503, errorBody(ErrorCode::BinaryNotFound, message));
                    logger.clearContext();
                    return;
                }
                json::value body;
                body[U("status")] = json::value::string(U("ready"));
                replyJson(request, 200, body);
                logger.clearContext();
                return;
            }
            if (path == "/v1/metrics" && method == methods::GET) {
                if (!isLoopbackRemote(toUtf8(request.remote_address()))) {
                    replyJson(request, 404, errorBody(ErrorCode::InvalidInput, "Not found"));
                    logger.clearContext();
                    return;
                }
                replyJson(request, 200, json::value::parse(toT(yt::metrics::toJson())));
                logger.clearContext();
                return;
            }
            if (path == "/v1/conversions") {
                if (method == methods::GET) {
                    http_response response(status_codes::MethodNotAllowed);
                    response.headers().add(U("Cache-Control"), U("no-store"));
                    response.headers().add(U("Allow"), U("POST"));
                    response.set_body(errorBody(ErrorCode::InvalidInput, "Use POST /v1/conversions"));
                    request.reply(response);
                    logger.clearContext();
                    return;
                }
                if (method != methods::POST) {
                    replyJson(request, 405, errorBody(ErrorCode::InvalidInput, "Method not allowed"));
                    logger.clearContext();
                    return;
                }
                if (!authorize(request)) {
                    replyJson(request, 401, errorBody(ErrorCode::Unauthorized, "Missing or invalid API key"));
                    logger.clearContext();
                    return;
                }
                handleConvert(request, requestId);
                logger.clearContext();
                return;
            }

            replyJson(request, 404, errorBody(ErrorCode::InvalidInput, "Not found"));
        } catch (const std::exception& error) {
            logger.critical(std::string("Unhandled API error: ") + error.what());
            replyJson(request, 500, errorBody(ErrorCode::Internal, "Internal server error"));
        }
        logger.clearContext();
    }

    void handleJobStatus(http_request& request, const std::string& jobId) {
        auto& logger = yt::logger::Logger::getInstance();
        auto* job = jobQueue.findJob(jobId);
        if (!job) {
            replyJson(request, 404, errorBody(ErrorCode::InvalidInput, "Job not found"));
            return;
        }
        json::value body;
        body[U("job_id")] = json::value::string(toT(job->job_id));
        body[U("video_id")] = json::value::string(toT(job->video_id));
        body[U("status")] = json::value::string(U("running"));
        body[U("queued_at")] = json::value::string(toT(std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                job->queued_at.time_since_epoch()).count())));
        replyJson(request, 200, body);
    }

    void handleJobCancel(http_request& request, const std::string& jobId) {
        auto& logger = yt::logger::Logger::getInstance();
        auto* job = jobQueue.findJob(jobId);
        if (!job) {
            replyJson(request, 404, errorBody(ErrorCode::InvalidInput, "Job not found"));
            return;
        }
        jobQueue.cancelJob(jobId);
        json::value body;
        body[U("job_id")] = json::value::string(toT(jobId));
        body[U("status")] = json::value::string(U("canceled"));
        replyJson(request, 200, body);
    }

    void handleConvert(http_request& request, const std::string& requestId) {
        auto& logger = yt::logger::Logger::getInstance();
        json::value body;
        
        // Reject large bodies before parsing
        utility::string_t contentType;
        if (request.headers().has(U("Content-Type"))) {
            contentType = request.headers().find(U("Content-Type"))->second;
        }
        if (contentType.find(U("application/json")) == utility::string_t::npos) {
            replyJson(request, 400, errorBody(ErrorCode::InvalidInput, "Content-Type must be application/json"));
            return;
        }
        
        try {
            body = request.extract_json().get();
        } catch (...) {
            replyJson(request, 400, errorBody(ErrorCode::InvalidInput, "Request body must be JSON"));
            return;
        }
        if (!body.has_field(U("url")) || !body.has_field(U("format")) ||
            !body.at(U("url")).is_string() || !body.at(U("format")).is_string()) {
            replyJson(request, 400,
                      errorBody(ErrorCode::InvalidInput, "JSON must include string fields url and format"));
            return;
        }

        const std::string url = toUtf8(body.at(U("url")).as_string());
        const std::string format = toUtf8(body.at(U("format")).as_string());

        // Validate first
        std::optional<std::string> validationError = yt::validation::validateConverterInput(url, format);
        if (validationError.has_value()) {
            replyJson(request, 400, errorBody(ErrorCode::InvalidInput, validationError.value()));
            return;
        }

        logger.info("Conversion request received");

        // Check if output already exists
        const auto video = yt::validation::parseYouTubeUrl(url);
        const std::string videoId = video.video->id;
        const std::string normalizedFormat = yt::validation::requireFormat(format);
        const std::string outputFilename = yt::converter::getOutputFilename(videoId, normalizedFormat);
        const fs::path outputRoot = fs::path(resolveOutputRoot(config.output_dir));
        const fs::path finalPath = outputRoot / outputFilename;

        if (config.reuse_completed && !config.force && fs::exists(finalPath) && fs::file_size(finalPath) > 0) {
            json::value ok;
            ok[U("status")] = json::value::string(U("success"));
            ok[U("error_code")] = json::value::string(U("ok"));
            ok[U("message")] = json::value::string(U("Reused existing output"));
            ok[U("output_path")] = json::value::string(toT(fs::weakly_canonical(finalPath).string()));
            ok[U("job_id")] = json::value::string(toT(videoId + "-" + normalizedFormat));
            ok[U("reused")] = json::value::boolean(true);
            replyJson(request, 200, ok);
            return;
        }

        // Create job and enqueue
        const std::string jobId = videoId + "-" + normalizedFormat;
        yt::converter::ConversionRequest conversion;
        conversion.url = url;
        conversion.format = format;
        conversion.config = config;
        conversion.request_id = requestId;
        conversion.job_id = jobId;
        conversion.show_progress = (config.log_level != "ERROR");

        yt::jobs::QueuedJob queuedJob;
        queuedJob.job_id = jobId;
        queuedJob.video_id = videoId;
        queuedJob.request = conversion;
        queuedJob.queued_at = std::chrono::steady_clock::now();

        jobQueue.enqueue(queuedJob);

        json::value queued;
        queued[U("status")] = json::value::string(U("queued"));
        queued[U("error_code")] = json::value::string(U("ok"));
        queued[U("job_id")] = json::value::string(toT(jobId));
        queued[U("status_url")] = json::value::string(toT("/v1/jobs/" + jobId));
        replyJson(request, 202, queued);
    }
};

ApiServer::ApiServer(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}

ApiServer::~ApiServer() {
    try {
        stop();
    } catch (...) {
    }
}

void ApiServer::start() {
    if (impl_->running.load()) {
        return;
    }
    impl_->jobQueue.setWorkerCount(config.max_concurrent);
    impl_->jobQueue.setQueueDepth(8);
    impl_->jobQueue.start();
    impl_->listener = std::make_unique<http_listener>(toT(impl_->url()));
    auto* impl = impl_.get();
    impl_->listener->support(methods::GET, [impl](const http_request& request) { impl->handle(request); });
    impl_->listener->support(methods::POST, [impl](const http_request& request) { impl->handle(request); });
    impl_->listener->open().wait();
    impl_->running.store(true);
    yt::logger::Logger::getInstance().info("HTTP listener opened on " + impl_->url());
}

void ApiServer::stop() {
    if (!impl_->running.exchange(false) && !impl_->listener) {
        return;
    }
    impl_->jobQueue.stop();
    if (impl_->listener) {
        impl_->listener->close().wait();
        impl_->listener.reset();
    }
}

void ApiServer::runUntilSignal() {
#ifndef _WIN32
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGUSR1, onUsr1);
#endif
    start();
    while (!g_shutdown.load()) {
        if (g_dumpMetrics.exchange(false)) {
            yt::metrics::logSnapshot();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    yt::logger::Logger::getInstance().info("Shutdown requested");
    yt::process::requestShutdown();
    stop();
}

bool ApiServer::isRunning() const { return impl_->running.load(); }

std::string ApiServer::listenUrl() const { return impl_->url(); }

int ApiServer::port() const { return impl_->config.port; }

} // namespace yt::api
