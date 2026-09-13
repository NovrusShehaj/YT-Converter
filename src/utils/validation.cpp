#include "validation.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace yt::validation {
namespace {

std::string trim(const std::string& input) {
    std::size_t begin = 0;
    std::size_t end = input.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(input[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
        --end;
    }
    return input.substr(begin, end - begin);
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool isYoutubeHost(const std::string& host) {
    return host == "youtube.com" || host == "www.youtube.com" || host == "m.youtube.com" ||
           host == "music.youtube.com";
}

bool isYoutuBeHost(const std::string& host) {
    return host == "youtu.be" || host == "www.youtu.be";
}

bool isAllowedHost(const std::string& host) { return isYoutubeHost(host) || isYoutuBeHost(host); }

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> parts;
    std::string current;
    for (char ch : path) {
        if (ch == '/') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

std::string queryValue(const std::string& query, const std::string& key) {
    std::size_t start = 0;
    while (start < query.size()) {
        const std::size_t amp = query.find('&', start);
        const std::string pair =
            query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        const std::size_t eq = pair.find('=');
        const std::string name = eq == std::string::npos ? pair : pair.substr(0, eq);
        if (name == key) {
            return eq == std::string::npos ? std::string() : pair.substr(eq + 1);
        }
        if (amp == std::string::npos) {
            break;
        }
        start = amp + 1;
    }
    return {};
}

ParseOutcome fail(ErrorCode code, const std::string& message) {
    ParseOutcome outcome;
    outcome.code = code;
    outcome.message = message;
    return outcome;
}

ParseOutcome ok(const std::string& id) {
    ParseOutcome outcome;
    VideoRef ref;
    ref.id = id;
    ref.canonical_url = "https://www.youtube.com/watch?v=" + id;
    outcome.video = ref;
    outcome.code = ErrorCode::Ok;
    outcome.message.clear();
    return outcome;
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

} // namespace

bool isValidVideoId(const std::string& id) {
    if (id.size() != 11) {
        return false;
    }
    for (char ch : id) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && ch != '_' && ch != '-') {
            return false;
        }
    }
    return true;
}

ParseOutcome parseYouTubeUrl(const std::string& rawUrl) {
    const std::string url = trim(rawUrl);
    if (url.empty()) {
        return fail(ErrorCode::InvalidUrl, "URL cannot be empty");
    }

    const std::size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) {
        return fail(ErrorCode::InvalidUrl, "Invalid YouTube URL format");
    }
    const std::string scheme = toLower(url.substr(0, schemeEnd));
    if (scheme != "http" && scheme != "https") {
        return fail(ErrorCode::InvalidUrl, "Invalid YouTube URL format");
    }

    const std::string rest = url.substr(schemeEnd + 3);
    if (rest.empty()) {
        return fail(ErrorCode::InvalidUrl, "Invalid YouTube URL format");
    }
    const std::size_t slash = rest.find_first_of("/?#");
    const std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    if (authority.empty() || authority.find('@') != std::string::npos) {
        return fail(ErrorCode::InvalidUrl, "Invalid YouTube URL format");
    }

    std::string host = authority;
    std::string port;
    if (!authority.empty() && authority.front() == '[') {
        return fail(ErrorCode::UnsupportedHost, "URL must be a YouTube video link");
    }
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }
    host = toLower(host);
    if (!port.empty() && port != "80" && port != "443") {
        return fail(ErrorCode::InvalidUrl, "Invalid YouTube URL format");
    }
    if (!isAllowedHost(host)) {
        return fail(ErrorCode::UnsupportedHost, "URL must be a YouTube video link");
    }

    std::string path = "/";
    std::string query;
    if (slash != std::string::npos) {
        const char marker = rest[slash];
        const std::string after = rest.substr(slash);
        if (marker == '/') {
            const std::size_t q = after.find('?');
            const std::size_t h = after.find('#');
            std::size_t pathEnd = after.size();
            if (q != std::string::npos) {
                pathEnd = q;
            }
            if (h != std::string::npos && h < pathEnd) {
                pathEnd = h;
            }
            path = after.substr(0, pathEnd);
            if (q != std::string::npos) {
                const std::size_t queryEnd = (h != std::string::npos && h > q) ? h : after.size();
                query = after.substr(q + 1, queryEnd - (q + 1));
            }
        } else if (marker == '?') {
            const std::size_t h = after.find('#');
            query = after.substr(1, h == std::string::npos ? std::string::npos : h - 1);
        }
    }

    const auto parts = splitPath(path);
    if (isYoutuBeHost(host)) {
        if (parts.size() != 1 || !isValidVideoId(parts[0])) {
            return fail(ErrorCode::InvalidUrl, "Invalid YouTube URL format");
        }
        return ok(parts[0]);
    }

    if (parts.empty()) {
        return fail(ErrorCode::InvalidUrl, "Invalid YouTube URL format");
    }
    if (parts[0] == "playlist") {
        return fail(ErrorCode::PlaylistOnly, "Playlist-only URLs are not supported");
    }
    if (parts[0] == "channel" || parts[0] == "c" || parts[0] == "user" ||
        startsWith(parts[0], "@")) {
        return fail(ErrorCode::ChannelUrl, "Channel URLs are not supported");
    }
    if (parts[0] == "watch") {
        const std::string id = queryValue(query, "v");
        if (!isValidVideoId(id)) {
            return fail(ErrorCode::InvalidUrl, "Invalid YouTube URL format");
        }
        return ok(id);
    }
    if ((parts[0] == "shorts" || parts[0] == "embed" || parts[0] == "live") && parts.size() == 2 &&
        isValidVideoId(parts[1])) {
        return ok(parts[1]);
    }
    return fail(ErrorCode::InvalidUrl, "Invalid YouTube URL format");
}

bool isValidURL(const std::string& url) { return parseYouTubeUrl(url).ok(); }

bool isValidFormat(const std::string& format) {
    const std::string normalized = normalizeFormat(format);
    return normalized == "mp3" || normalized == "mp4" || normalized == "wav";
}

std::vector<std::string> getSupportedFormats() { return {"mp3", "mp4", "wav"}; }

std::string getURLValidationError(const std::string& url) {
    return parseYouTubeUrl(url).message;
}

std::string getFormatValidationError(const std::string& format) {
    if (format.empty()) {
        return "Format cannot be empty";
    }
    return "Invalid format '" + format + "'. Supported formats are: mp3, mp4, wav";
}

std::string normalizeFormat(const std::string& format) { return toLower(trim(format)); }

std::optional<std::string> validateConverterInput(const std::string& url,
                                                  const std::string& format) {
    const ParseOutcome parsed = parseYouTubeUrl(url);
    if (!parsed.ok()) {
        return parsed.message;
    }
    if (!isValidFormat(format)) {
        return getFormatValidationError(format);
    }
    return std::nullopt;
}

VideoRef requireVideo(const std::string& url) {
    const ParseOutcome parsed = parseYouTubeUrl(url);
    if (!parsed.ok()) {
        throw Error(parsed.code, parsed.message);
    }
    return *parsed.video;
}

std::string requireFormat(const std::string& format) {
    if (!isValidFormat(format)) {
        throw Error(ErrorCode::UnsupportedFormat, getFormatValidationError(format));
    }
    return normalizeFormat(format);
}

} // namespace yt::validation
