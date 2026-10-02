#ifndef YT_CONVERTER_CONVERTER_H
#define YT_CONVERTER_CONVERTER_H

#include "config.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace yt::converter {

struct ConversionRequest {
    std::string url;
    std::string format;
    Config config;
    std::string request_id;
    std::string job_id;
    bool show_progress = false;
    bool refresh = false;
    // Admission time (nanoseconds since the Unix epoch). A refresh only accepts a source
    // generation whose download started at or after this time. 0 means "now" at processVideo().
    std::int64_t admitted_at = 0;
    std::function<void(const std::string& stage, int percent)> on_progress;
    // Cancellation token for this operation. A shared download keeps running while any other
    // subscriber still needs it, even after this token is set.
    std::shared_ptr<std::atomic<bool>> cancel;
};

struct ConversionResult {
    std::string output_path;
    std::string job_id;
    std::string video_id;
    // True only when an already completed output file satisfied the request.
    bool reused = false;
    std::uint64_t download_ms = 0;
    std::uint64_t convert_ms = 0;
    // Size of the source file used. When this operation downloaded it, an approximation of the
    // downloaded media bytes (not wire traffic); otherwise cached or shared bytes, not new work.
    std::uint64_t source_bytes = 0;
    std::uint64_t output_bytes = 0;
    std::string source_generation;
    bool source_cache_hit = false;
    bool shared_download = false;
    bool downloaded = false;
    bool published = false;
    // A newer publication of the same output satisfied this request, so it did not publish.
    bool superseded = false;
};

// Reuse policy, shared by the CLI, the queue, and the HTTP fast path.
//
// | reuse_completed | force  | refresh | behavior                                              |
// | yes             | no     | no      | reuse a valid completed output, else encode           |
// | yes             | yes    | no      | replace the output; a fresh cached source is allowed  |
// | any             | any    | yes     | new source generation, then replace the output        |
// | no              | any    | no      | encode and publish again; cached source allowed       |
//
// Completed-output reuse requires reuse_completed && !force && !refresh plus a usable file.
bool allowsCompletedOutputReuse(const ConversionRequest& request);
// Returns the completed-output result when the policy allows reuse and the output is usable.
std::optional<ConversionResult> findReusableOutput(const ConversionRequest& request);

ConversionResult processVideo(const ConversionRequest& request);
ConversionResult processVideo(const std::string& url, const std::string& format);
std::string getOutputFilename(const std::string& videoID, const std::string& format);
void setFreeSpaceBytesForTests(std::optional<std::uintmax_t> bytes);
// Test instrumentation: operations that have joined another operation's in-flight download and
// are waiting for it. A test waits on this to know a subscriber's interest is registered.
int sharedDownloadWaitersForTests();

} // namespace yt::converter

#endif // YT_CONVERTER_CONVERTER_H
