#include "request_gate.h"
#include "error.h"
#include "logger.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace yt::api {

#ifdef _WIN32

// The gate is POSIX-only. On Windows the API is unsupported (see docs/ARCHITECTURE.md).
struct RequestGate::Impl {};
RequestGate::RequestGate(GateLimits, std::string) : impl_(std::make_unique<Impl>()) {}
RequestGate::~RequestGate() = default;
void RequestGate::bind(const std::string&, int) {
    throw Error(ErrorCode::ConfigError, "The HTTP API is not supported on Windows");
}
void RequestGate::start(int) {}
void RequestGate::stop() {}
GateStats RequestGate::stats() const {
    return {};
}
int findFreeLoopbackPort() {
    return 0;
}

#else

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kChunk = 4096;
constexpr std::size_t kMaxChunkLine = 1024;
constexpr std::size_t kRelayBuffer = 65536;
constexpr std::size_t kLingerBytes = 65536;
constexpr auto kLingerTime = std::chrono::milliseconds(500);

void closeFd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

void setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t");
    return value.substr(begin, end - begin + 1);
}

std::string jsonError(int status, const char* reason, const std::string& message) {
    const std::string body =
        R"({"status":"error","error_code":"invalid_input","message":")" + message + "\"}";
    return "HTTP/1.1 " + std::to_string(status) + " " + reason +
           "\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n"
           "Connection: close\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\n\r\n" + body;
}

std::string busyResponse() {
    const std::string body =
        R"({"status":"error","error_code":"busy","message":"Too many open connections"})";
    return "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\n"
           "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\n\r\n" + body;
}

std::string peerAddress(const sockaddr_storage& addr) {
    char text[INET6_ADDRSTRLEN] = {0};
    if (addr.ss_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(&addr);
        ::inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text));
    } else if (addr.ss_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        ::inet_ntop(AF_INET6, &in6->sin6_addr, text, sizeof(text));
    }
    return text;
}

enum class Phase {
    Head,       // reading request line and headers
    Body,       // reading a Content-Length body
    Chunked,    // decoding a chunked body
    Connecting, // connecting to the internal listener
    Sending,    // writing the normalized request upstream
    Relaying,   // copying the response back to the client
    Responding, // writing a gate-generated response
    Lingering,  // response sent; discarding a bounded amount of unread input before close
    Done
};

enum class ChunkPhase { Size, Data, DataEnd, Trailer };

struct Connection {
    int client = -1;
    int upstream = -1;
    std::string peer;
    Phase phase = Phase::Head;
    Clock::time_point deadline;
    bool pending = true; // counted against max_pending_reads

    std::string head;
    std::string body;
    std::size_t content_length = 0;
    ChunkPhase chunk = ChunkPhase::Size;
    std::size_t chunk_remaining = 0;
    std::string line;
    std::size_t trailer_bytes = 0;

    std::string method;
    std::string target;
    std::vector<std::pair<std::string, std::string>> headers;

    std::string outbound; // to upstream
    std::size_t outbound_sent = 0;
    std::string response; // to client
    std::size_t response_sent = 0;
    bool upstream_eof = false;
    bool response_rewritten = false;
    std::size_t linger_left = kLingerBytes;
};

} // namespace

struct RequestGate::Impl {
    GateLimits limits;
    std::string secret;
    int listenFd = -1;
    int wake[2] = {-1, -1};
    int upstreamPort = 0;
    std::thread thread;
    std::atomic<bool> stopping{false};
    // After an accept() failure such as EMFILE, stop polling the listener briefly instead of
    // spinning on a socket that stays readable.
    Clock::time_point acceptPausedUntil{};
    std::list<Connection> connections;

    mutable std::mutex statsMutex;
    GateStats stats;

    void loop();
    void acceptAll();
    void handleClientRead(Connection& conn);
    bool consumeBody(Connection& conn, const char* data, std::size_t n);
    bool parseHead(Connection& conn, const std::string& head);
    void forward(Connection& conn);
    void reject(Connection& conn, int status, const char* reason, const std::string& message);
    void relayFromUpstream(Connection& conn);
    void writeToClient(Connection& conn);
    void writeToUpstream(Connection& conn);
    void finish(Connection& conn);
    void notePeak(const Connection& conn);
    std::size_t pendingCount() const;

    template <typename F> void bump(F&& update) {
        std::lock_guard<std::mutex> lock(statsMutex);
        update(stats);
    }
};

RequestGate::RequestGate(GateLimits limits, std::string secret) : impl_(std::make_unique<Impl>()) {
    impl_->limits = limits;
    impl_->secret = std::move(secret);
}

RequestGate::~RequestGate() {
    stop();
}

void RequestGate::bind(const std::string& host, int port) {
    std::string bindHost = host;
    if (bindHost == "*") {
        bindHost = "0.0.0.0";
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    addrinfo* results = nullptr;
    const std::string service = std::to_string(port);
    if (::getaddrinfo(bindHost.c_str(), service.c_str(), &hints, &results) != 0) {
        throw Error(ErrorCode::ConfigError, "Cannot resolve bind address " + host);
    }
    int fd = -1;
    for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        const int yes = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (::bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && ::listen(fd, 128) == 0) {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(results);
    if (fd < 0) {
        throw Error(ErrorCode::ConfigError, "Cannot listen on " + host + ":" +
                                                std::to_string(port) + ": " + std::strerror(errno));
    }
    setNonBlocking(fd);
    impl_->listenFd = fd;
}

void RequestGate::start(int upstreamPort) {
    impl_->upstreamPort = upstreamPort;
    if (::pipe(impl_->wake) != 0) {
        throw Error(ErrorCode::Internal, "Cannot create gate wake pipe");
    }
    setNonBlocking(impl_->wake[0]);
    setNonBlocking(impl_->wake[1]);
    impl_->stopping.store(false);
    impl_->thread = std::thread([impl = impl_.get()] { impl->loop(); });
}

void RequestGate::stop() {
    if (!impl_->thread.joinable()) {
        closeFd(impl_->listenFd);
        return;
    }
    impl_->stopping.store(true);
    const char byte = 1;
    [[maybe_unused]] const auto written = ::write(impl_->wake[1], &byte, 1);
    impl_->thread.join();
    closeFd(impl_->wake[0]);
    closeFd(impl_->wake[1]);
}

GateStats RequestGate::stats() const {
    std::lock_guard<std::mutex> lock(impl_->statsMutex);
    return impl_->stats;
}

std::size_t RequestGate::Impl::pendingCount() const {
    return static_cast<std::size_t>(std::count_if(connections.begin(), connections.end(),
                                                  [](const Connection& c) { return c.pending; }));
}

void RequestGate::Impl::notePeak(const Connection& conn) {
    const std::size_t held = conn.head.size() + conn.body.size() + conn.line.size();
    bump([held](GateStats& s) { s.peak_request_bytes = std::max(s.peak_request_bytes, held); });
}

void RequestGate::Impl::finish(Connection& conn) {
    closeFd(conn.client);
    closeFd(conn.upstream);
    conn.phase = Phase::Done;
    conn.pending = false;
}

void RequestGate::Impl::reject(Connection& conn, int status, const char* reason,
                               const std::string& message) {
    // Stop reading the request (no unbounded drain); send the response, then linger briefly.
    conn.pending = false;
    closeFd(conn.upstream);
    conn.head.clear();
    conn.body.clear();
    conn.line.clear();
    conn.response = jsonError(status, reason, message);
    conn.response_sent = 0;
    conn.phase = Phase::Responding;
    conn.deadline = Clock::now() + std::chrono::milliseconds(limits.write_timeout_ms);
}

void RequestGate::Impl::acceptAll() {
    while (true) {
        sockaddr_storage addr{};
        socklen_t len = sizeof(addr);
        const int fd = ::accept(listenFd, reinterpret_cast<sockaddr*>(&addr), &len);
        if (fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                acceptPausedUntil = Clock::now() + std::chrono::milliseconds(100);
            }
            return;
        }
        setNonBlocking(fd);
        bump([](GateStats& s) { ++s.accepted; });
        if (connections.size() >= static_cast<std::size_t>(limits.max_connections)) {
            // Hard cap on descriptors: best-effort 503 without tracking the socket.
            const std::string response = busyResponse();
            [[maybe_unused]] const auto sent =
                ::send(fd, response.data(), response.size(), MSG_NOSIGNAL);
            ::close(fd);
            bump([](GateStats& s) { ++s.rejected_busy; });
            continue;
        }
        Connection conn;
        conn.client = fd;
        conn.peer = peerAddress(addr);
        conn.deadline = Clock::now() + std::chrono::milliseconds(limits.read_timeout_ms);
        if (pendingCount() >= static_cast<std::size_t>(limits.max_pending_reads)) {
            conn.pending = false;
            conn.response = busyResponse();
            conn.phase = Phase::Responding;
            conn.deadline = Clock::now() + std::chrono::milliseconds(limits.write_timeout_ms);
            bump([](GateStats& s) { ++s.rejected_busy; });
        }
        connections.push_back(std::move(conn));
    }
}

bool RequestGate::Impl::parseHead(Connection& conn, const std::string& head) {
    std::size_t lineEnd = head.find("\r\n");
    const std::string requestLine = head.substr(0, lineEnd);
    const auto firstSpace = requestLine.find(' ');
    const auto secondSpace = requestLine.find(' ', firstSpace + 1);
    if (firstSpace == std::string::npos || secondSpace == std::string::npos ||
        requestLine.find(' ', secondSpace + 1) != std::string::npos) {
        return false;
    }
    conn.method = requestLine.substr(0, firstSpace);
    conn.target = requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    const std::string version = requestLine.substr(secondSpace + 1);
    if (conn.method.empty() || conn.target.empty() || conn.target[0] != '/' ||
        (version != "HTTP/1.1" && version != "HTTP/1.0")) {
        return false;
    }
    for (char c : conn.method) {
        if (!std::isupper(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    std::size_t pos = lineEnd + 2;
    while (pos < head.size()) {
        const std::size_t end = head.find("\r\n", pos);
        const std::string line = head.substr(pos, end - pos);
        pos = end + 2;
        if (line.empty()) {
            break;
        }
        if (line[0] == ' ' || line[0] == '\t') {
            return false; // obsolete line folding
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos || colon == 0) {
            return false;
        }
        const std::string name = line.substr(0, colon);
        for (char c : name) {
            if (std::isspace(static_cast<unsigned char>(c)) ||
                std::iscntrl(static_cast<unsigned char>(c))) {
                return false;
            }
        }
        conn.headers.emplace_back(name, trim(line.substr(colon + 1)));
    }
    return true;
}

void RequestGate::Impl::forward(Connection& conn) {
    conn.pending = false;
    std::string out = conn.method + " " + conn.target + " HTTP/1.1\r\n";
    for (const auto& [name, value] : conn.headers) {
        const std::string key = lower(name);
        if (key == "connection" || key == "keep-alive" || key == "proxy-connection" ||
            key == "transfer-encoding" || key == "te" || key == "trailer" || key == "upgrade" ||
            key == "content-length" || key == lower(kGateSecretHeader) ||
            key == lower(kGatePeerHeader) || key == "expect") {
            continue;
        }
        out.append(name).append(": ").append(value).append("\r\n");
    }
    out.append(kGateSecretHeader).append(": ").append(secret).append("\r\n");
    out.append(kGatePeerHeader).append(": ").append(conn.peer).append("\r\n");
    out.append("Content-Length: ")
        .append(std::to_string(conn.body.size()))
        .append("\r\nConnection: close\r\n\r\n");
    out += conn.body;
    conn.outbound = std::move(out);
    conn.outbound_sent = 0;
    conn.head.clear();
    conn.head.shrink_to_fit();
    conn.body.clear();
    conn.body.shrink_to_fit();
    conn.headers.clear();

    conn.upstream = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (conn.upstream < 0) {
        reject(conn, 503, "Service Unavailable", "Internal listener unavailable");
        return;
    }
    setNonBlocking(conn.upstream);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(upstreamPort));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int rc = ::connect(conn.upstream, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS) {
        reject(conn, 503, "Service Unavailable", "Internal listener unavailable");
        return;
    }
    conn.phase = Phase::Connecting;
    bump([](GateStats& s) { ++s.forwarded; });
}

// Feeds request-body bytes. Returns false once the connection has been rejected or forwarded.
bool RequestGate::Impl::consumeBody(Connection& conn, const char* data, std::size_t n) {
    if (conn.phase == Phase::Body) {
        const std::size_t take = std::min(n, conn.content_length - conn.body.size());
        conn.body.append(data, take);
        notePeak(conn);
        if (conn.body.size() == conn.content_length) {
            forward(conn);
            return false;
        }
        return true;
    }
    // Chunked decoding with every intermediate buffer bounded.
    for (std::size_t i = 0; i < n;) {
        switch (conn.chunk) {
        case ChunkPhase::Size:
        case ChunkPhase::Trailer: {
            const char c = data[i++];
            conn.line.push_back(c);
            if (conn.chunk == ChunkPhase::Trailer) {
                ++conn.trailer_bytes;
            }
            if (conn.line.size() > kMaxChunkLine || conn.trailer_bytes > limits.max_head_bytes) {
                bump([](GateStats& s) { ++s.rejected_malformed; });
                reject(conn, 400, "Bad Request", "Malformed chunked request body");
                return false;
            }
            if (conn.line.size() < 2 || conn.line.compare(conn.line.size() - 2, 2, "\r\n") != 0) {
                continue;
            }
            const std::string text = conn.line.substr(0, conn.line.size() - 2);
            conn.line.clear();
            if (conn.chunk == ChunkPhase::Trailer) {
                if (text.empty()) {
                    forward(conn);
                    return false;
                }
                continue;
            }
            const std::string sizeText = trim(text.substr(0, text.find(';')));
            if (sizeText.empty() || sizeText.size() > 16 ||
                !std::all_of(sizeText.begin(), sizeText.end(),
                             [](unsigned char c) { return std::isxdigit(c) != 0; })) {
                bump([](GateStats& s) { ++s.rejected_malformed; });
                reject(conn, 400, "Bad Request", "Malformed chunked request body");
                return false;
            }
            const std::size_t size = std::stoull(sizeText, nullptr, 16);
            if (size > limits.max_body_bytes - conn.body.size()) {
                bump([](GateStats& s) { ++s.rejected_oversized; });
                reject(conn, 400, "Bad Request", "Request body exceeds 8 KB");
                return false;
            }
            if (size == 0) {
                conn.chunk = ChunkPhase::Trailer;
            } else {
                conn.chunk_remaining = size;
                conn.chunk = ChunkPhase::Data;
            }
            break;
        }
        case ChunkPhase::Data: {
            const std::size_t take = std::min(n - i, conn.chunk_remaining);
            conn.body.append(data + i, take);
            i += take;
            conn.chunk_remaining -= take;
            notePeak(conn);
            if (conn.chunk_remaining == 0) {
                conn.chunk = ChunkPhase::DataEnd;
            }
            break;
        }
        case ChunkPhase::DataEnd: {
            conn.line.push_back(data[i++]);
            if (conn.line.size() == 2) {
                if (conn.line != "\r\n") {
                    bump([](GateStats& s) { ++s.rejected_malformed; });
                    reject(conn, 400, "Bad Request", "Malformed chunked request body");
                    return false;
                }
                conn.line.clear();
                conn.chunk = ChunkPhase::Size;
            }
            break;
        }
        }
    }
    notePeak(conn);
    return true;
}

void RequestGate::Impl::handleClientRead(Connection& conn) {
    char buffer[kChunk];
    std::size_t want = sizeof(buffer);
    if (conn.phase == Phase::Head) {
        // Never read past the header limit (plus the terminator) in one step.
        want = std::min<std::size_t>(want, limits.max_head_bytes + 4 - conn.head.size());
    } else if (conn.phase == Phase::Body) {
        want = std::min<std::size_t>(want, conn.content_length - conn.body.size());
    }
    const ssize_t n = ::recv(conn.client, buffer, want, 0);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        bump([](GateStats& s) { ++s.client_disconnects; });
        finish(conn); // disconnected mid-request: nothing is forwarded or enqueued
        return;
    }
    if (n < 0) {
        return;
    }
    const auto count = static_cast<std::size_t>(n);
    if (conn.phase != Phase::Head) {
        consumeBody(conn, buffer, count);
        return;
    }
    conn.head.append(buffer, count);
    notePeak(conn);
    const auto end = conn.head.find("\r\n\r\n");
    if (end == std::string::npos) {
        if (conn.head.size() > limits.max_head_bytes) {
            bump([](GateStats& s) { ++s.rejected_malformed; });
            reject(conn, 431, "Request Header Fields Too Large", "Request headers are too large");
        }
        return;
    }
    const std::string head = conn.head.substr(0, end + 4);
    const std::string rest = conn.head.substr(end + 4);
    conn.head.clear();
    if (!parseHead(conn, head)) {
        bump([](GateStats& s) { ++s.rejected_malformed; });
        reject(conn, 400, "Bad Request", "Malformed HTTP request");
        return;
    }
    int lengthHeaders = 0;
    std::string lengthValue;
    bool chunked = false;
    bool otherEncoding = false;
    for (const auto& [name, value] : conn.headers) {
        const std::string key = lower(name);
        if (key == "content-length") {
            ++lengthHeaders;
            lengthValue = value;
        } else if (key == "transfer-encoding") {
            if (lower(value) == "chunked" && !chunked) {
                chunked = true;
            } else {
                otherEncoding = true;
            }
        }
    }
    if (otherEncoding || (chunked && lengthHeaders > 0) || lengthHeaders > 1) {
        bump([](GateStats& s) { ++s.rejected_malformed; });
        reject(conn, 400, "Bad Request", "Unsupported or conflicting body framing");
        return;
    }
    if (chunked) {
        conn.phase = Phase::Chunked;
        conn.chunk = ChunkPhase::Size;
    } else if (lengthHeaders == 1) {
        if (lengthValue.empty() || lengthValue.size() > 19 ||
            !std::all_of(lengthValue.begin(), lengthValue.end(),
                         [](unsigned char c) { return std::isdigit(c) != 0; })) {
            bump([](GateStats& s) { ++s.rejected_malformed; });
            reject(conn, 400, "Bad Request", "Invalid Content-Length");
            return;
        }
        const unsigned long long length = std::stoull(lengthValue);
        if (length > limits.max_body_bytes) {
            // Fast rejection from a valid header; the body is never read.
            bump([](GateStats& s) { ++s.rejected_oversized; });
            reject(conn, 400, "Bad Request", "Request body exceeds 8 KB");
            return;
        }
        conn.content_length = static_cast<std::size_t>(length);
        conn.phase = Phase::Body;
        if (conn.content_length == 0) {
            forward(conn);
            return;
        }
    } else {
        forward(conn);
        return;
    }
    if (!rest.empty()) {
        consumeBody(conn, rest.data(), rest.size());
    }
}

void RequestGate::Impl::writeToUpstream(Connection& conn) {
    if (conn.phase == Phase::Connecting) {
        int error = 0;
        socklen_t len = sizeof(error);
        ::getsockopt(conn.upstream, SOL_SOCKET, SO_ERROR, &error, &len);
        if (error != 0) {
            reject(conn, 503, "Service Unavailable", "Internal listener unavailable");
            return;
        }
        conn.phase = Phase::Sending;
    }
    while (conn.outbound_sent < conn.outbound.size()) {
        const ssize_t n = ::send(conn.upstream, conn.outbound.data() + conn.outbound_sent,
                                 conn.outbound.size() - conn.outbound_sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                return;
            }
            reject(conn, 503, "Service Unavailable", "Internal listener unavailable");
            return;
        }
        conn.outbound_sent += static_cast<std::size_t>(n);
    }
    conn.outbound.clear();
    conn.outbound.shrink_to_fit();
    conn.phase = Phase::Relaying;
}

void RequestGate::Impl::relayFromUpstream(Connection& conn) {
    char buffer[kChunk];
    const ssize_t n = ::recv(conn.upstream, buffer, sizeof(buffer), 0);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        closeFd(conn.upstream);
        if (!conn.response_rewritten && conn.response.empty()) {
            reject(conn, 502, "Bad Gateway", "Internal listener closed the connection");
            return;
        }
        conn.upstream_eof = true;
        conn.response_rewritten = true;
        return;
    }
    if (n < 0) {
        return;
    }
    if (conn.response_sent >= conn.response.size()) {
        // New bytes to deliver: the client must keep accepting them.
        conn.deadline = Clock::now() + std::chrono::milliseconds(limits.write_timeout_ms);
    }
    conn.response.append(buffer, static_cast<std::size_t>(n));
    if (!conn.response_rewritten) {
        // One request per client connection: announce the close after the status line.
        const auto statusEnd = conn.response.find("\r\n");
        if (statusEnd != std::string::npos) {
            conn.response.insert(statusEnd + 2, "Connection: close\r\n");
            conn.response_rewritten = true;
        } else if (conn.response.size() >= kRelayBuffer) {
            conn.response_rewritten = true; // not a recognizable status line; pass through
        }
    }
}

void RequestGate::Impl::writeToClient(Connection& conn) {
    while (conn.response_sent < conn.response.size()) {
        const ssize_t n = ::send(conn.client, conn.response.data() + conn.response_sent,
                                 conn.response.size() - conn.response_sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                return;
            }
            finish(conn);
            return;
        }
        conn.response_sent += static_cast<std::size_t>(n);
        conn.deadline = Clock::now() + std::chrono::milliseconds(limits.write_timeout_ms);
    }
    if (conn.phase == Phase::Relaying && conn.response_rewritten) {
        conn.response.clear();
        conn.response_sent = 0;
    }
    const bool complete =
        conn.phase == Phase::Responding || (conn.phase == Phase::Relaying && conn.upstream_eof &&
                                            conn.response_sent >= conn.response.size());
    if (complete) {
        // Half-close, then discard at most kLingerBytes for at most kLingerTime so the client
        // reads the response instead of a reset caused by unread request bytes.
        ::shutdown(conn.client, SHUT_WR);
        conn.phase = Phase::Lingering;
        conn.deadline = Clock::now() + kLingerTime;
    }
}

void RequestGate::Impl::loop() {
    std::vector<pollfd> fds;
    std::vector<std::pair<Connection*, bool>> owners; // connection, is-upstream
    while (!stopping.load()) {
        fds.clear();
        owners.clear();
        fds.push_back({wake[0], POLLIN, 0});
        owners.emplace_back(nullptr, false);
        auto now = Clock::now();
        const bool acceptPaused = now < acceptPausedUntil;
        fds.push_back({acceptPaused ? -1 : listenFd, POLLIN, 0}); // negative fd: ignored
        owners.emplace_back(nullptr, false);
        auto nextDeadline = now + std::chrono::milliseconds(200);
        for (auto& conn : connections) {
            short clientEvents = 0;
            short upstreamEvents = 0;
            switch (conn.phase) {
            case Phase::Head:
            case Phase::Body:
            case Phase::Chunked:
            case Phase::Lingering:
                clientEvents = POLLIN;
                break;
            case Phase::Connecting:
            case Phase::Sending:
                upstreamEvents = POLLOUT;
                clientEvents = POLLIN; // notice a client that goes away
                break;
            case Phase::Relaying:
                clientEvents = POLLIN;
                if (conn.response_sent < conn.response.size() &&
                    (conn.response_rewritten || conn.upstream_eof)) {
                    clientEvents |= POLLOUT;
                }
                if (conn.upstream >= 0 && conn.response.size() < kRelayBuffer) {
                    upstreamEvents = POLLIN;
                }
                break;
            case Phase::Responding:
                clientEvents = POLLOUT;
                break;
            case Phase::Done:
                break;
            }
            if (conn.client >= 0 && clientEvents != 0) {
                fds.push_back({conn.client, clientEvents, 0});
                owners.emplace_back(&conn, false);
            }
            if (conn.upstream >= 0 && upstreamEvents != 0) {
                fds.push_back({conn.upstream, upstreamEvents, 0});
                owners.emplace_back(&conn, true);
            }
            // A relaying connection only has a deadline while response bytes are pending; the
            // wait for the response itself is bounded by the conversion timeouts instead.
            const bool relayPending =
                conn.phase == Phase::Relaying && conn.response_sent < conn.response.size();
            if ((conn.phase != Phase::Relaying && conn.phase != Phase::Connecting &&
                 conn.phase != Phase::Sending) ||
                relayPending) {
                nextDeadline = std::min(nextDeadline, conn.deadline);
            }
        }
        const auto waitMs = std::max<long long>(
            0, std::chrono::duration_cast<std::chrono::milliseconds>(nextDeadline - now).count());
        ::poll(fds.data(), fds.size(), static_cast<int>(waitMs));
        if (stopping.load()) {
            break;
        }
        if (fds[1].revents & POLLIN) {
            acceptAll();
        }
        for (std::size_t i = 2; i < fds.size(); ++i) {
            Connection* conn = owners[i].first;
            const short revents = fds[i].revents;
            if (conn == nullptr || revents == 0 || conn->phase == Phase::Done) {
                continue;
            }
            if (owners[i].second) {
                if (conn->phase == Phase::Connecting || conn->phase == Phase::Sending) {
                    writeToUpstream(*conn);
                } else if (conn->phase == Phase::Relaying) {
                    relayFromUpstream(*conn);
                }
                continue;
            }
            switch (conn->phase) {
            case Phase::Head:
            case Phase::Body:
            case Phase::Chunked:
                handleClientRead(*conn);
                break;
            case Phase::Connecting:
            case Phase::Sending:
            case Phase::Relaying: {
                if (revents & POLLOUT) {
                    writeToClient(*conn);
                }
                if (conn->phase != Phase::Done && (revents & (POLLIN | POLLHUP | POLLERR))) {
                    // Extra client bytes after the request are discarded; EOF ends the exchange.
                    char discard[kChunk];
                    const ssize_t n = ::recv(conn->client, discard, sizeof(discard), 0);
                    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                        bump([](GateStats& s) { ++s.client_disconnects; });
                        finish(*conn);
                    }
                }
                break;
            }
            case Phase::Responding:
                writeToClient(*conn);
                break;
            case Phase::Lingering: {
                char discard[kChunk];
                const ssize_t n =
                    ::recv(conn->client, discard, std::min(sizeof(discard), conn->linger_left), 0);
                if (n <= 0 && !(n < 0 && (errno == EAGAIN || errno == EINTR))) {
                    finish(*conn);
                } else if (n > 0) {
                    conn->linger_left -= std::min(conn->linger_left, static_cast<std::size_t>(n));
                    if (conn->linger_left == 0) {
                        finish(*conn);
                    }
                }
                break;
            }
            case Phase::Done:
                break;
            }
        }
        // Relaying connections flush as soon as data or EOF arrives.
        for (auto& conn : connections) {
            if (conn.phase == Phase::Relaying && conn.client >= 0 &&
                conn.response_sent < conn.response.size() &&
                (conn.response_rewritten || conn.upstream_eof)) {
                writeToClient(conn);
            } else if (conn.phase == Phase::Relaying && conn.upstream_eof &&
                       conn.response_sent >= conn.response.size()) {
                writeToClient(conn);
            }
        }
        now = Clock::now();
        for (auto& conn : connections) {
            if (conn.phase == Phase::Done || now < conn.deadline) {
                continue;
            }
            if (conn.phase == Phase::Head || conn.phase == Phase::Body ||
                conn.phase == Phase::Chunked) {
                bump([](GateStats& s) { ++s.rejected_timeout; });
                reject(conn, 408, "Request Timeout", "Request was not received in time");
            } else if (conn.phase == Phase::Responding || conn.phase == Phase::Lingering) {
                finish(conn);
            } else if (conn.phase == Phase::Relaying && conn.response_sent < conn.response.size()) {
                finish(conn); // the client stopped reading our response
            }
        }
        connections.remove_if([](const Connection& c) { return c.phase == Phase::Done; });
        bump([this](GateStats& s) {
            s.open_connections = connections.size();
            s.pending_reads = pendingCount();
        });
    }
    // Shutdown: close everything, including half-read requests (nothing was forwarded for them).
    for (auto& conn : connections) {
        finish(conn);
    }
    connections.clear();
    closeFd(listenFd);
    bump([](GateStats& s) {
        s.open_connections = 0;
        s.pending_reads = 0;
    });
}

int findFreeLoopbackPort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        throw Error(ErrorCode::Internal, "Cannot create a socket");
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        throw Error(ErrorCode::Internal, "Cannot reserve a loopback port");
    }
    const int port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

#endif

} // namespace yt::api
