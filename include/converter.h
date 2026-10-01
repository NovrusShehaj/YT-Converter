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
    std::function<void(const std::string& stage, int percent)> on_progress;
    std::shared_ptr<std::atomic<bool>> cancel;
};

struct ConversionResult {
    std::string output_path;
    std::string job_id;
    std::string video_id;
    bool reused = false;
    std::uint64_t download_ms = 0;
    std::uint64_t convert_ms = 0;
    std::uint64_t bytes_downloaded = 0;
};

ConversionResult processVideo(const ConversionRequest& request);
ConversionResult processVideo(const std::string& url, const std::string& format);
std::string getOutputFilename(const std::string& videoID, const std::string& format);
void setFreeSpaceBytesForTests(std::optional<std::uintmax_t> bytes);

} // namespace yt::converter

#endif // YT_CONVERTER_CONVERTER_H
