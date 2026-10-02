#include "api_app.h"
#include "converter.h"
#include "dependencies.h"
#include "error.h"
#include "job_queue.h"
#include "logger.h"
#include "metrics.h"
#include "process.h"
#include "validation.h"

#include <cpprest/http_listener.h>
#include <cpprest/json.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

using namespace web;
using namespace web::http;
using namespace web::http::experimental::listener;

namespace yt::api {
namespace {

constexpr std::size_t kMaxBodyBytes = 8192;

std::atomic<bool> g_shutdown{false};
std::atomic<bool> g_dumpMetrics{false};

namespace fs = std::filesystem;

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

std::string makeJobId(const std::string& videoId, const std::string& format) {
    std::random_device device;
    std::mt19937_64 rng((static_cast<std::uint64_t>(device()) << 32) ^ device());
    std::ostringstream ss;
    ss << videoId << '-' << format << '-' << std::hex << std::setw(16) << std::setfill('0')
       << rng();
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

bool isSafeRequestId(const std::string& id) {
    if (id.empty() || id.size() > 64) {
        return false;
    }
    return std::all_of(id.begin(), id.end(),
                       [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; });
}

bool isLoopbackRemote(const std::string& remote) {
    return remote.find("127.0.0.1") != std::string::npos ||
           remote.find("::1") != std::string::npos || remote == "localhost";
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

json::value jobJson(const jobs::JobSnapshot& job) {
    json::value body;
    body[U("job_id")] = json::value::string(toT(job.job_id));
    body[U("video_id")] = json::value::string(toT(job.video_id));
    body[U("status")] = json::value::string(toT(jobs::jobStateString(job.state)));
    body[U("stage")] = json::value::string(toT(job.stage));
    body[U("error_code")] = json::value::string(toT(job.error_code));
    body[U("message")] = json::value::string(toT(job.message));
    body[U("request_id")] = json::value::string(toT(job.request_id));
    body[U("reused")] = json::value::boolean(job.reused);
    body[U("download_ms")] = json::value::number(static_cast<double>(job.download_ms));
    body[U("convert_ms")] = json::value::number(static_cast<double>(job.convert_ms));
    body[U("bytes")] = json::value::number(static_cast<double>(job.bytes));
    body[U("queue_ms")] = json::value::number(static_cast<double>(job.queue_ms));
    if (job.percent >= 0) {
        body[U("percent")] = json::value::number(job.percent);
    }
    if (!job.output_path.empty()) {
        body[U("output_path")] = json::value::string(toT(job.output_path));
    }
    return body;
}

void onSignal(int) {
    g_shutdown.store(true);
}

#ifndef _WIN32
void onUsr1(int) {
    g_dumpMetrics.store(true);
}
#endif

} // namespace

void requestApiShutdown() {
    g_shutdown.store(true);
}

bool apiShutdownRequested() {
    return g_shutdown.load();
}

class ApiServer::Impl {
  public:
    explicit Impl(Config cfg) : config(std::move(cfg)) {}

    Config config;
    jobs::Queue jobQueue;
    std::unique_ptr<http_listener> listener;
    std::atomic<bool> running{false};

    std::string url() const {
        if (config.bind.find(':') != std::string::npos && config.bind != "localhost") {
            return "http://[" + config.bind + "]:" + std::to_string(config.port);
        }
        return "http://" + config.bind + ":" + std::to_string(config.port);
    }

    // Single route policy for every service-key protected route (conversions and jobs). With no
    // key configured, validateApiConfig only permits the explicit unauthenticated-localhost mode.
    // Health, readiness, and loopback-only metrics stay public by design.
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

    // Replies 401 and returns false when the request lacks the configured key. Called before any
    // lookup so an unauthorized caller learns nothing about whether a job exists.
    bool requireServiceKey(const http_request& request) const {
        if (authorize(request)) {
            return true;
        }
        replyJson(request, 401, errorBody(ErrorCode::Unauthorized, "Missing or invalid API key"));
        return false;
    }

    bool outputWritable() const {
        try {
            const std::string root = resolveOutputRoot(config.output_dir);
            const auto probe = fs::path(root) / ".ytconv-write-test";
            {
                std::ofstream out(probe);
                if (!out) {
                    return false;
                }
                out << "ok";
            }
            fs::remove(probe);
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
            const std::string supplied = toUtf8(request.headers().find(U("X-Request-Id"))->second);
            if (isSafeRequestId(supplied)) {
                requestId = supplied;
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
            if (path.rfind("/v1/jobs/", 0) == 0 &&
                (method == methods::GET || method == methods::DEL)) {
                if (requireServiceKey(request)) {
                    const std::string jobId = path.substr(std::string("/v1/jobs/").size());
                    if (method == methods::GET) {
                        handleJobStatus(request, jobId);
                    } else {
                        handleJobCancel(request, jobId);
                    }
                }
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
                    response.set_body(
                        errorBody(ErrorCode::InvalidInput, "Use POST /v1/conversions"));
                    request.reply(response);
                    logger.clearContext();
                    return;
                }
                if (method != methods::POST) {
                    replyJson(request, 405,
                              errorBody(ErrorCode::InvalidInput, "Method not allowed"));
                    logger.clearContext();
                    return;
                }
                if (requireServiceKey(request)) {
                    handleConvert(request, requestId);
                }
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
        if (!isSafeJobId(jobId)) {
            replyJson(request, 404, errorBody(ErrorCode::InvalidInput, "Not found"));
            return;
        }
        const auto job = jobQueue.find(jobId);
        if (!job) {
            replyJson(request, 404, errorBody(ErrorCode::InvalidInput, "Job not found"));
            return;
        }
        replyJson(request, 200, jobJson(*job));
    }

    void handleJobCancel(http_request& request, const std::string& jobId) {
        if (!isSafeJobId(jobId)) {
            replyJson(request, 404, errorBody(ErrorCode::InvalidInput, "Not found"));
            return;
        }
        const auto result = jobQueue.cancel(jobId);
        if (result == jobs::CancelResult::Missing) {
            replyJson(request, 404, errorBody(ErrorCode::InvalidInput, "Job not found"));
            return;
        }
        if (result == jobs::CancelResult::AlreadyFinished) {
            replyJson(request, 409, errorBody(ErrorCode::InvalidInput, "Job is already finished"));
            return;
        }
        json::value body;
        body[U("job_id")] = json::value::string(toT(jobId));
        body[U("status")] = json::value::string(U("canceled"));
        replyJson(request, 200, body);
    }

    void handleConvert(http_request& request, const std::string& requestId) {
        auto& logger = yt::logger::Logger::getInstance();
        if (request.headers().has(U("Content-Length"))) {
            try {
                const auto length =
                    std::stoll(toUtf8(request.headers().find(U("Content-Length"))->second));
                if (length > static_cast<long long>(kMaxBodyBytes)) {
                    replyJson(request, 400,
                              errorBody(ErrorCode::InvalidInput, "Request body exceeds 8 KB"));
                    return;
                }
            } catch (const std::exception&) {
                replyJson(request, 400,
                          errorBody(ErrorCode::InvalidInput, "Invalid Content-Length"));
                return;
            }
        }
        utility::string_t contentType;
        if (request.headers().has(U("Content-Type"))) {
            contentType = request.headers().find(U("Content-Type"))->second;
        }
        if (contentType.find(U("application/json")) == utility::string_t::npos) {
            replyJson(request, 400,
                      errorBody(ErrorCode::InvalidInput, "Content-Type must be application/json"));
            return;
        }

        std::string raw;
        try {
            raw = toUtf8(request.extract_string().get());
        } catch (...) {
            replyJson(request, 400,
                      errorBody(ErrorCode::InvalidInput, "Request body must be JSON"));
            return;
        }
        if (raw.size() > kMaxBodyBytes) {
            replyJson(request, 400,
                      errorBody(ErrorCode::InvalidInput, "Request body exceeds 8 KB"));
            return;
        }

        json::value body;
        try {
            body = json::value::parse(toT(raw));
        } catch (...) {
            replyJson(request, 400,
                      errorBody(ErrorCode::InvalidInput, "Request body must be JSON"));
            return;
        }
        if (!body.has_field(U("url")) || !body.has_field(U("format")) ||
            !body.at(U("url")).is_string() || !body.at(U("format")).is_string()) {
            replyJson(request, 400,
                      errorBody(ErrorCode::InvalidInput,
                                "JSON must include string fields url and format"));
            return;
        }

        const std::string url = toUtf8(body.at(U("url")).as_string());
        const std::string format = toUtf8(body.at(U("format")).as_string());
        const auto validationError = yt::validation::validateConverterInput(url, format);
        if (validationError.has_value()) {
            replyJson(request, 400, errorBody(ErrorCode::InvalidInput, validationError.value()));
            return;
        }

        bool refresh = false;
        if (body.has_field(U("refresh")) && body.at(U("refresh")).is_boolean()) {
            refresh = body.at(U("refresh")).as_bool();
        }
        bool force = config.force;
        if (body.has_field(U("force")) && body.at(U("force")).is_boolean()) {
            force = body.at(U("force")).as_bool();
        }

        const auto parsed = yt::validation::parseYouTubeUrl(url);
        if (!parsed.video.has_value()) {
            replyJson(request, 400, errorBody(parsed.code, parsed.message));
            return;
        }
        const std::string videoId = parsed.video->id;
        const std::string normalizedFormat = yt::validation::requireFormat(format);
        const fs::path outputRoot(resolveOutputRoot(config.output_dir));
        const fs::path finalPath =
            outputRoot / yt::converter::getOutputFilename(videoId, normalizedFormat);

        if (config.reuse_completed && !force && !refresh && fs::exists(finalPath) &&
            fs::file_size(finalPath) > 0) {
            json::value ok;
            ok[U("status")] = json::value::string(U("success"));
            ok[U("error_code")] = json::value::string(U("ok"));
            ok[U("message")] = json::value::string(U("Reused existing output"));
            ok[U("output_path")] =
                json::value::string(toT(fs::weakly_canonical(finalPath).string()));
            ok[U("job_id")] = json::value::string(toT(videoId + "-" + normalizedFormat));
            ok[U("reused")] = json::value::boolean(true);
            ok[U("request_id")] = json::value::string(toT(requestId));
            replyJson(request, 200, ok);
            return;
        }

        yt::converter::ConversionRequest conversion;
        conversion.url = url;
        conversion.format = normalizedFormat;
        conversion.config = config;
        conversion.config.force = force;
        conversion.request_id = requestId;
        conversion.refresh = refresh;
        conversion.show_progress = config.log_level != "ERROR";
        conversion.job_id = makeJobId(videoId, normalizedFormat);

        if (config.sync_conversions) {
            try {
                const auto result = yt::converter::processVideo(conversion);
                json::value ok;
                ok[U("status")] = json::value::string(U("success"));
                ok[U("error_code")] = json::value::string(U("ok"));
                ok[U("message")] = json::value::string(U("Conversion completed"));
                ok[U("output_path")] = json::value::string(toT(result.output_path));
                ok[U("job_id")] = json::value::string(toT(result.job_id));
                ok[U("reused")] = json::value::boolean(result.reused);
                ok[U("download_ms")] = json::value::number(static_cast<double>(result.download_ms));
                ok[U("convert_ms")] = json::value::number(static_cast<double>(result.convert_ms));
                ok[U("bytes")] = json::value::number(static_cast<double>(result.bytes_downloaded));
                ok[U("request_id")] = json::value::string(toT(requestId));
                replyJson(request, 200, ok);
            } catch (const yt::Error& error) {
                logger.warning(error.message());
                replyJson(request, error.httpStatus(), errorBody(error));
            }
            return;
        }

        logger.info("Conversion request received");
        jobs::SubmitResult submitted;
        // A generated ID never replaces an existing record; regenerate on the rare collision.
        for (int attempt = 0; attempt < 4; ++attempt) {
            submitted = jobQueue.submit(conversion, videoId, conversion.job_id);
            if (submitted.kind != jobs::SubmitKind::Duplicate) {
                break;
            }
            conversion.job_id = makeJobId(videoId, normalizedFormat);
        }
        if (submitted.kind == jobs::SubmitKind::Full || submitted.kind == jobs::SubmitKind::Busy ||
            submitted.kind == jobs::SubmitKind::Duplicate) {
            replyJson(request, 503, errorBody(ErrorCode::Busy, "Too many concurrent conversions"));
            return;
        }
        if (submitted.kind == jobs::SubmitKind::Stopping) {
            replyJson(request, 503, errorBody(ErrorCode::Busy, "Server is shutting down"));
            return;
        }
        json::value queued;
        queued[U("status")] = json::value::string(U("queued"));
        queued[U("error_code")] = json::value::string(U("ok"));
        queued[U("job_id")] = json::value::string(toT(submitted.snapshot.job_id));
        queued[U("status_url")] = json::value::string(toT("/v1/jobs/" + submitted.snapshot.job_id));
        queued[U("request_id")] = json::value::string(toT(requestId));
        replyJson(request, 202, queued);
    }
};

ApiServer::ApiServer(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}

ApiServer::~ApiServer() {
    try {
        stop();
    } catch (const std::exception& error) {
        yt::logger::Logger::getInstance().error(std::string("API shutdown failed: ") +
                                                error.what());
    } catch (...) {
        yt::logger::Logger::getInstance().error("API shutdown failed");
    }
}

void ApiServer::start() {
    if (impl_->running.load()) {
        return;
    }
    jobs::Limits limits;
    limits.workers = impl_->config.max_concurrent;
    limits.max_operations = impl_->config.queue_depth;
    limits.max_active_jobs = impl_->config.max_active_jobs;
    limits.history_max = impl_->config.job_history_max;
    limits.history_ttl_sec = impl_->config.job_history_ttl_sec;
    impl_->jobQueue.start(limits);
    impl_->listener = std::make_unique<http_listener>(toT(impl_->url()));
    auto* impl = impl_.get();
    impl_->listener->support(methods::GET,
                             [impl](const http_request& request) { impl->handle(request); });
    impl_->listener->support(methods::POST,
                             [impl](const http_request& request) { impl->handle(request); });
    impl_->listener->support(methods::DEL,
                             [impl](const http_request& request) { impl->handle(request); });
    impl_->listener->open().wait();
    impl_->running.store(true);
    yt::logger::Logger::getInstance().info("HTTP listener opened on " + impl_->url());
}

void ApiServer::stop() {
    impl_->running.store(false);
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

bool ApiServer::isRunning() const {
    return impl_->running.load();
}

std::string ApiServer::listenUrl() const {
    return impl_->url();
}

int ApiServer::port() const {
    return impl_->config.port;
}

} // namespace yt::api
