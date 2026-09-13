#ifndef YT_CONVERTER_PROCESS_H
#define YT_CONVERTER_PROCESS_H

#include <cstddef>
#include <string>
#include <vector>

namespace yt::process {

struct RunOptions {
    int timeout_ms = 900000;
    bool capture_stdout = true;
    bool capture_stderr = true;
    bool inherit_stderr = false;
    std::size_t max_output_bytes = 8192;
};

struct RunResult {
    int exit_code = 1;
    std::string stdout_text;
    std::string stderr_text;
    bool timed_out = false;
    bool not_found = false;
    bool canceled = false;
};

RunResult run(const std::vector<std::string>& argv, const RunOptions& options = {});
void requestShutdown();
bool shutdownRequested();
void resetShutdownForTests();

} // namespace yt::process

#endif // YT_CONVERTER_PROCESS_H
