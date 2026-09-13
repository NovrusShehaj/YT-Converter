#ifndef YT_CONVERTER_CONVERTER_H
#define YT_CONVERTER_CONVERTER_H

#include "config.h"

#include <string>

namespace yt::converter {

struct ConversionRequest {
    std::string url;
    std::string format;
    Config config;
    std::string request_id;
    std::string job_id;
    bool show_progress = false;
};

struct ConversionResult {
    std::string output_path;
    std::string job_id;
    std::string video_id;
    bool reused = false;
};

ConversionResult processVideo(const ConversionRequest& request);
ConversionResult processVideo(const std::string& url, const std::string& format);
std::string getOutputFilename(const std::string& videoID, const std::string& format);

} // namespace yt::converter

#endif // YT_CONVERTER_CONVERTER_H
