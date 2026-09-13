#ifndef YT_CONVERTER_VALIDATION_H
#define YT_CONVERTER_VALIDATION_H

#include "error.h"

#include <optional>
#include <string>
#include <vector>

namespace yt::validation {

struct VideoRef {
    std::string id;
    std::string canonical_url;
};

struct ParseOutcome {
    std::optional<VideoRef> video;
    ErrorCode code = ErrorCode::InvalidUrl;
    std::string message;

    bool ok() const { return video.has_value(); }
};

bool isValidVideoId(const std::string& id);
ParseOutcome parseYouTubeUrl(const std::string& url);
bool isValidURL(const std::string& url);
bool isValidFormat(const std::string& format);
std::vector<std::string> getSupportedFormats();
std::string getURLValidationError(const std::string& url);
std::string getFormatValidationError(const std::string& format);
std::string normalizeFormat(const std::string& format);
std::optional<std::string> validateConverterInput(const std::string& url,
                                                  const std::string& format);
VideoRef requireVideo(const std::string& url);
std::string requireFormat(const std::string& format);

} // namespace yt::validation

#endif // YT_CONVERTER_VALIDATION_H
