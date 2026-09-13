#ifndef YT_CONVERTER_ERROR_H
#define YT_CONVERTER_ERROR_H

#include <stdexcept>
#include <string>

namespace yt {

enum class ErrorCode {
    Ok,
    InvalidInput,
    InvalidUrl,
    UnsupportedHost,
    UnsupportedFormat,
    PlaylistOnly,
    ChannelUrl,
    BinaryNotFound,
    DownloadFailed,
    ConversionFailed,
    Timeout,
    DiskFull,
    Busy,
    Canceled,
    Unauthorized,
    ConfigError,
    Internal
};

class Error : public std::runtime_error {
public:
    Error(ErrorCode code, std::string message);

    ErrorCode code() const noexcept { return code_; }
    const std::string& message() const noexcept { return message_; }
    const char* codeString() const noexcept;
    int httpStatus() const noexcept;
    int exitCode() const noexcept;

private:
    ErrorCode code_;
    std::string message_;
};

const char* errorCodeString(ErrorCode code) noexcept;
int errorHttpStatus(ErrorCode code) noexcept;
int errorExitCode(ErrorCode code) noexcept;

} // namespace yt

#endif // YT_CONVERTER_ERROR_H
