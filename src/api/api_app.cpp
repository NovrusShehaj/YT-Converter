#include "api_app.h"
#include "converter.h"
#include "dependencies.h"
#include "error.h"
#include "job_queue.h"
#include "logger.h"
#include "metrics.h"
#include "process.h"
#include "request_gate.h"
#include "validation.h"

#include <cpprest/streams.h>
#include <cpprest/http_listener.h>
#include <cpprest/json.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
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
#include <vector>

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

bool isLoopbackPeer(const std::string& peer) {
    return peer == "127.0.0.1" || peer == "::1" || peer == "::ffff:127.0.0.1";
}

std::string headerValue(const http_request& request, const char* name) {
    const auto& headers = request.headers();
    const auto key = toT(name);
    if (!headers.has(key)) {
        return {};
    }
    return toUtf8(headers.find(key)->second);
}

std::string randomSecret() {
    std::random_device device;
    std::ostringstream ss;
    for (int i = 0; i < 4; ++i) {
        ss << std::hex << std::setw(8) << std::setfill('0') << device();
    }
    return ss.str();
}

// Reads at most `limit` body bytes with asynchronous stream reads; never blocks a listener
// thread. The gate already guarantees a complete body of at most 8 KiB, so this is a second,
// independent bound rather than the only one.
class BodyReader : public std::enable_shared_from_this<BodyReader> {
  public:
    BodyReader(const concurrency::streams::istream& stream, std::size_t limit)
        : source_(stream.streambuf()), buffer_(limit), limit_(limit) {}

    pplx::task<std::string> read() {
        auto self = shared_from_this();
        if (used_ >= limit_) {
            return pplx::task_from_result(text());
        }
        return source_.getn(buffer_.data() + used_, limit_ - used_).then([self](std::size_t count) {
            if (count == 0) {
                return pplx::task_from_result(self->text());
            }
            self->used_ += count;
            return self->read();
        });
    }

  private:
    std::string text() const {
        return std::string(reinterpret_cast<const char*>(buffer_.data()), used_);
    }

    concurrency::streams::streambuf<std::uint8_t> source_;
    std::vector<std::uint8_t> buffer_; // fixed capacity: never grows past the limit
    std::size_t used_ = 0;
    std::size_t limit_;
};

// Counts request continuations still running so shutdown can wait for them before the server
// state they use is destroyed.
struct Continuations {
    std::mutex mutex;
    std::condition_variable cv;
    int active = 0;

    struct Token {
        explicit Token(std::shared_ptr<Continuations> owner) : owner_(std::move(owner)) {
            std::lock_guard<std::mutex> lock(owner_->mutex);
            ++owner_->active;
        }
        ~Token() {
            {
                std::lock_guard<std::mutex> lock(owner_->mutex);
                --owner_->active;
            }
            owner_->cv.notify_all();
        }
        Token(const Token&) = delete;
        Token& operator=(const Token&) = delete;

      private:
        std::shared_ptr<Continuations> owner_;
    };

    bool waitIdle(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, timeout, [this] { return active == 0; });
    }
};

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

// Final response for a synchronous conversion, built from the same snapshot as job status.
void replySync(const http_request& request, const jobs::JobSnapshot& job, bool shuttingDown) {
    if (job.state == jobs::JobState::Succeeded) {
        json::value body = jobJson(job);
        body[U("status")] = json::value::string(U("success"));
        replyJson(request, 200, body);
        return;
    }
    if (job.state == jobs::JobState::Canceled && shuttingDown) {
        replyJson(request, 503, errorBody(ErrorCode::Busy, "Server is shutting down"));
        return;
    }
    const ErrorCode code = job.code == ErrorCode::Ok ? ErrorCode::Internal : job.code;
    json::value body = errorBody(code, job.message);
    body[U("job_id")] = json::value::string(toT(job.job_id));
    replyJson(request, errorHttpStatus(code), body);
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
    // Read by synchronous completion callbacks; shared so it outlives this object if needed.
    std::shared_ptr<std::atomic<bool>> stopping_ = std::make_shared<std::atomic<bool>>(false);
    jobs::Queue jobQueue;
    std::unique_ptr<http_listener> listener;
    std::unique_ptr<RequestGate> gate;
    std::shared_ptr<Continuations> continuations = std::make_shared<Continuations>();
    std::string gateSecret = randomSecret();
    int internalPort = 0;
    std::atomic<bool> running{false};

    std::string internalUrl() const { return "http://127.0.0.1:" + std::to_string(internalPort); }

    // Only requests relayed by the gate reach the handlers. The internal listener is bound to
    // loopback, and the per-process secret stops other local processes from bypassing the gate.
    bool fromGate(const http_request& request) const {
#ifdef _WIN32
        (void)request;
        return true;
#else
        return constantTimeEquals(headerValue(request, kGateSecretHeader), gateSecret);
#endif
    }

    std::string peerOf(const http_request& request) const {
#ifdef _WIN32
        return toUtf8(request.remote_address());
#else
        return headerValue(request, kGatePeerHeader);
#endif
    }

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

        if (!fromGate(request)) {
            replyJson(request, 403, errorBody(ErrorCode::Unauthorized, "Forbidden"));
            logger.clearContext();
            return;
        }
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
                if (!isLoopbackPeer(peerOf(request))) {
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
        // Strict Content-Length validation; a valid oversized length is rejected before reading.
        if (request.headers().has(U("Content-Length"))) {
            const std::string length = headerValue(request, "Content-Length");
            const bool digits = !length.empty() && length.size() <= 19 &&
                                std::all_of(length.begin(), length.end(),
                                            [](unsigned char c) { return std::isdigit(c) != 0; });
            if (!digits) {
                replyJson(request, 400,
                          errorBody(ErrorCode::InvalidInput, "Invalid Content-Length"));
                return;
            }
            if (std::stoull(length) > kMaxBodyBytes) {
                replyJson(request, 400,
                          errorBody(ErrorCode::InvalidInput, "Request body exceeds 8 KB"));
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

        // Read at most 8,193 bytes asynchronously; the extra byte detects overflow. JSON is only
        // parsed once a complete body of at most 8,192 bytes is available.
        auto token = std::make_shared<Continuations::Token>(continuations);
        auto reader = std::make_shared<BodyReader>(request.body(), kMaxBodyBytes + 1);
        reader->read().then([this, request, requestId, token](const pplx::task<std::string>& task) {
            auto& logger = yt::logger::Logger::getInstance();
            logger.setContext({requestId, {}});
            try {
                std::string raw;
                try {
                    raw = task.get();
                } catch (...) {
                    replyJson(request, 400,
                              errorBody(ErrorCode::InvalidInput, "Request body must be JSON"));
                    logger.clearContext();
                    return;
                }
                if (raw.size() > kMaxBodyBytes) {
                    replyJson(request, 400,
                              errorBody(ErrorCode::InvalidInput, "Request body exceeds 8 KB"));
                } else {
                    convertBody(request, requestId, raw);
                }
            } catch (const std::exception& error) {
                logger.critical(std::string("Unhandled API error: ") + error.what());
                replyJson(request, 500, errorBody(ErrorCode::Internal, "Internal server error"));
            }
            logger.clearContext();
        });
    }

    void convertBody(const http_request& request, const std::string& requestId,
                     const std::string& raw) {
        auto& logger = yt::logger::Logger::getInstance();
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

        yt::converter::ConversionRequest conversion;
        conversion.url = url;
        conversion.format = normalizedFormat;
        conversion.config = config;
        conversion.config.force = force;
        conversion.request_id = requestId;
        conversion.refresh = refresh;
        conversion.show_progress = config.log_level != "ERROR";
        conversion.job_id = makeJobId(videoId, normalizedFormat);

        // Same eligibility rule as the converter's own checks (see converter.h).
        if (const auto reused = yt::converter::findReusableOutput(conversion)) {
            json::value ok;
            ok[U("status")] = json::value::string(U("success"));
            ok[U("error_code")] = json::value::string(U("ok"));
            ok[U("message")] = json::value::string(U("Reused existing output"));
            ok[U("output_path")] = json::value::string(toT(reused->output_path));
            ok[U("job_id")] = json::value::string(toT(videoId + "-" + normalizedFormat));
            ok[U("reused")] = json::value::boolean(true);
            ok[U("request_id")] = json::value::string(toT(requestId));
            replyJson(request, 200, ok);
            return;
        }

        // Synchronous mode uses the same queue, admission limits, sharing, and cancellation as
        // asynchronous mode. The response stays outstanding until the job's terminal callback
        // replies; no listener thread blocks. The callback captures only the request and a
        // shared shutdown flag, never server state, and Queue::stop() resolves it.
        jobs::TerminalCallback onTerminal;
        if (config.sync_conversions) {
            onTerminal = [request, stopping = stopping_](const jobs::JobSnapshot& job) {
                replySync(request, job, stopping->load());
            };
        }

        logger.info("Conversion request received");
        jobs::SubmitResult submitted;
        // A generated ID never replaces an existing record; regenerate on the rare collision.
        for (int attempt = 0; attempt < 4; ++attempt) {
            submitted = jobQueue.submit(conversion, videoId, conversion.job_id, onTerminal);
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
        if (config.sync_conversions) {
            return; // replied by onTerminal
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
    impl_->stopping_->store(false);
    const Config& config = impl_->config;
#ifndef _WIN32
    // The gate owns the public socket; binding first surfaces port conflicts before anything
    // else starts.
    GateLimits gateLimits;
    gateLimits.max_body_bytes = kMaxBodyBytes;
    gateLimits.read_timeout_ms = config.request_read_timeout_sec * 1000;
    gateLimits.max_pending_reads = config.max_pending_reads;
    gateLimits.max_connections = config.max_connections;
    impl_->gate = std::make_unique<RequestGate>(gateLimits, impl_->gateSecret);
    impl_->gate->bind(config.bind, config.port);
#endif
    try {
        jobs::Limits limits;
        limits.workers = config.max_concurrent;
        limits.max_operations = config.queue_depth;
        limits.max_active_jobs = config.max_active_jobs;
        limits.history_max = config.job_history_max;
        limits.history_ttl_sec = config.job_history_ttl_sec;
        impl_->jobQueue.start(limits);

        auto* impl = impl_.get();
        for (int attempt = 0;; ++attempt) {
#ifndef _WIN32
            impl_->internalPort = findFreeLoopbackPort();
            const std::string url = impl_->internalUrl();
#else
            const std::string url = impl_->url();
#endif
            impl_->listener = std::make_unique<http_listener>(toT(url));
            impl_->listener->support(
                methods::GET, [impl](const http_request& request) { impl->handle(request); });
            impl_->listener->support(
                methods::POST, [impl](const http_request& request) { impl->handle(request); });
            impl_->listener->support(
                methods::DEL, [impl](const http_request& request) { impl->handle(request); });
            try {
                impl_->listener->open().wait();
                break;
            } catch (...) {
                impl_->listener.reset();
                if (attempt >= 4) {
                    throw;
                }
            }
        }
#ifndef _WIN32
        impl_->gate->start(impl_->internalPort);
#endif
    } catch (...) {
        stop();
        throw;
    }
    impl_->running.store(true);
    yt::logger::Logger::getInstance().info("HTTP listener opened on " + impl_->url());
}

void ApiServer::stop() {
    impl_->running.store(false);
    impl_->stopping_->store(true);
    // Resolves outstanding synchronous responses (503) and joins workers before the listener
    // and the rest of the server state go away.
    impl_->jobQueue.stop();
    // Closing the internal listener completes in-flight exchanges; the gate is still running, so
    // their responses reach clients.
    if (impl_->listener) {
        impl_->listener->close().wait();
        impl_->listener.reset();
    }
    // Body-read continuations use server state; none may outlive it.
    if (!impl_->continuations->waitIdle(std::chrono::seconds(10))) {
        yt::logger::Logger::getInstance().critical("Request continuations did not finish");
    }
    // Closes remaining client connections, including half-read requests.
    if (impl_->gate) {
        impl_->gate->stop();
        impl_->gate.reset();
    }
}

int ApiServer::internalPortForTests() const {
    return impl_->internalPort;
}

GateStats ApiServer::requestStats() const {
    return impl_->gate ? impl_->gate->stats() : GateStats{};
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
