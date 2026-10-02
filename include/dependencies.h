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
// Readiness is a current advisory probe of the configured tools, cached per tool configuration
// for ready_ttl_sec after a success. Failures are never cached.
PreflightResult checkToolsCached(const Config& config);
// Production invalidation: called when a conversion cannot launch a tool. The next readiness
// check probes again immediately; a restored tool makes readiness succeed again. Probes already
// running when this is called cannot republish their (older) results.
void invalidateReadiness();
void clearReadyCacheForTests();

} // namespace yt::deps

#endif // YT_CONVERTER_DEPENDENCIES_H
