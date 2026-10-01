#ifndef YT_CONVERTER_PROCESS_H
#define YT_CONVERTER_PROCESS_H

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace yt::process {

struct RunOptions {
    int timeout_ms = 900000;
    bool capture_stdout = true;
    bool capture_stderr = true;
    bool inherit_stderr = false;
    // Captured stdout/stderr text keeps at most the last max_output_bytes of each stream.
    std::size_t max_output_bytes = 8192;
    // Callback lines are bounded independently of capture. A line longer than max_line_bytes keeps
    // its first max_line_bytes bytes, the rest is discarded up to the newline, and exactly one
    // truncated callback is delivered for it. A final unterminated line follows the same rule.
    // CR bytes are dropped; LF ends a line. Pipes keep draining while a line is being discarded.
    std::size_t max_line_bytes = 8192;
    // Called on the run() thread, never under a process-registry lock. If it throws, the child's
    // process group is killed and reaped and the exception propagates from run().
    using LineCallback = void (*)(const std::string& line, void* user);
    LineCallback on_line = nullptr;
    void* on_line_user = nullptr;
    std::shared_ptr<std::atomic<bool>> cancel;
};

struct RunResult {
    int exit_code = 1;
    std::string stdout_text;
    std::string stderr_text;
    bool timed_out = false;
    bool not_found = false;
    bool canceled = false;
    // Largest pending callback-line buffer observed for either stream (test instrumentation).
    std::size_t peak_line_bytes = 0;
};

RunResult run(const std::vector<std::string>& argv, const RunOptions& options = {});
void requestShutdown();
bool shutdownRequested();
void resetShutdownForTests();

} // namespace yt::process

#endif // YT_CONVERTER_PROCESS_H
