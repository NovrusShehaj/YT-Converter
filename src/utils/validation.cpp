#include "validation.h"
#include <regex>
#include <algorithm>
#include <cctype>

namespace yt::validation {

bool isValidURL(const std::string& url) {
    if (url.empty()) {
        return false;
    }

    // Regex pattern for YouTube URLs
    // Matches: http(s)://(www.)youtube.com/watch?v=VIDEO_ID
    std::regex youtube_url_pattern(
        R"(https?://(www\.)?youtube\.com/watch\?v=[a-zA-Z0-9_-]+)"
    );

    return std::regex_search(url, youtube_url_pattern);
}

bool isValidFormat(const std::string& format) {
    if (format.empty()) {
        return false;
    }

    std::string normalized = normalizeFormat(format);
    return normalized == "mp3" || normalized == "mp4" || normalized == "wav";
}

bool isValidString(const std::string& input, size_t minLength) {
    return input.length() >= minLength && !input.empty();
}

std::vector<std::string> getSupportedFormats() {
    return {"mp3", "mp4", "wav"};
}

std::string getURLValidationError(const std::string& url) {
    if (url.empty()) {
        return "URL cannot be empty";
    }

    if (url.find("youtube.com") == std::string::npos && 
        url.find("youtu.be") == std::string::npos) {
        return "URL must be a valid YouTube URL (youtube.com)";
    }

    if (url.find("watch?v=") == std::string::npos && 
        url.find("youtu.be/") == std::string::npos) {
        return "URL must contain a valid video ID (watch?v=VIDEO_ID)";
    }

    return "Invalid YouTube URL format";
}

std::string getFormatValidationError(const std::string& format) {
    if (format.empty()) {
        return "Format cannot be empty";
    }

    std::string supported_formats = "mp3, mp4, wav";
    return "Invalid format '" + format + "'. Supported formats are: " + supported_formats;
}

std::string normalizeFormat(const std::string& format) {
    std::string result = format;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

std::optional<std::string> validateConverterInput(const std::string& url, const std::string& format) {
    // Check URL validity
    if (!isValidURL(url)) {
        return getURLValidationError(url);
    }

    // Check format validity
    if (!isValidFormat(format)) {
        return getFormatValidationError(format);
    }

    return std::nullopt;
}

} // namespace yt::validation