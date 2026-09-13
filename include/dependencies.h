#ifndef YT_CONVERTER_DEPENDENCIES_H
#define YT_CONVERTER_DEPENDENCIES_H

#include "config.h"

#include <string>

namespace yt::deps {

struct PreflightResult {
    bool ok = false;
    std::string yt_dlp_version;
    std::string ffmpeg_version;
    std::string message;
};

PreflightResult checkTools(const Config& config);
void requireTools(const Config& config);
std::string installHint();

} // namespace yt::deps

#endif // YT_CONVERTER_DEPENDENCIES_H
