#ifndef YT_CONVERTER_REQUEST_GATE_H
#define YT_CONVERTER_REQUEST_GATE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace yt::api {

// cpprestsdk's asio listener reads every request body (Content-Length or chunked) into memory
// before or while the handler runs, and applies no read timeout, so an application-level size
// check cannot bound memory. The gate owns the public socket instead: it reads each request with
// strict limits and deadlines, rejects violations itself, and forwards only validated requests
// (normalized to Content-Length, Connection: close) to cpprest on a private loopback port.
struct GateLimits {
    std::size_t max_body_bytes = 8192;  // decoded body bytes; one more is a rejection
    std::size_t max_head_bytes = 16384; // request line, headers, and chunked trailers
    int read_timeout_ms = 10000;        // whole request (head and body) must arrive in time
    int write_timeout_ms = 10000;       // a client must keep accepting response bytes
    int max_pending_reads = 32;         // connections still sending their request
    int max_connections = 128;          // all client connections, incl. awaiting responses
};

struct GateStats {
    std::uint64_t accepted = 0;
    std::uint64_t forwarded = 0;
    std::uint64_t rejected_oversized = 0;
    std::uint64_t rejected_malformed = 0;
    std::uint64_t rejected_timeout = 0;
    std::uint64_t rejected_busy = 0;
    std::uint64_t client_disconnects = 0;
    // Largest number of request bytes the gate ever held for one connection.
    std::size_t peak_request_bytes = 0;
    std::size_t open_connections = 0;
    std::size_t pending_reads = 0;
};

// Header names the gate adds to forwarded requests (client-supplied copies are dropped).
constexpr const char* kGateSecretHeader = "X-Ytconv-Gate";
constexpr const char* kGatePeerHeader = "X-Ytconv-Peer";

class RequestGate {
  public:
    RequestGate(const GateLimits& limits, std::string secret);
    ~RequestGate();
    RequestGate(const RequestGate&) = delete;
    RequestGate& operator=(const RequestGate&) = delete;

    // Binds the public listener. Throws yt::Error(ConfigError) if the address cannot be bound.
    void bind(const std::string& host, int port);
    // Starts the event loop, forwarding to 127.0.0.1:upstreamPort.
    void start(int upstreamPort);
    // Closes the listener and every connection (including half-read requests) and joins.
    void stop();
    GateStats stats() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Returns an unused loopback TCP port for the internal listener.
int findFreeLoopbackPort();

} // namespace yt::api

#endif // YT_CONVERTER_REQUEST_GATE_H
