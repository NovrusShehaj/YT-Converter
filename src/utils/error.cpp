#include "error.h"

namespace yt {

Error::Error(ErrorCode code, std::string message)
    : std::runtime_error(message), code_(code), message_(std::move(message)) {}

const char* Error::codeString() const noexcept { return errorCodeString(code_); }

int Error::httpStatus() const noexcept { return errorHttpStatus(code_); }

int Error::exitCode() const noexcept { return errorExitCode(code_); }

const char* errorCodeString(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Ok:
        return "ok";
    case ErrorCode::InvalidInput:
        return "invalid_input";
    case ErrorCode::InvalidUrl:
        return "invalid_url";
    case ErrorCode::UnsupportedHost:
        return "unsupported_host";
    case ErrorCode::UnsupportedFormat:
        return "unsupported_format";
    case ErrorCode::PlaylistOnly:
        return "playlist_only";
    case ErrorCode::ChannelUrl:
        return "channel_url";
    case ErrorCode::BinaryNotFound:
        return "binary_not_found";
    case ErrorCode::DownloadFailed:
        return "download_failed";
    case ErrorCode::ConversionFailed:
        return "conversion_failed";
    case ErrorCode::Timeout:
        return "timeout";
    case ErrorCode::DiskFull:
        return "disk_full";
    case ErrorCode::Busy:
        return "busy";
    case ErrorCode::Canceled:
        return "canceled";
    case ErrorCode::Unauthorized:
        return "unauthorized";
    case ErrorCode::ConfigError:
        return "config_error";
    case ErrorCode::Internal:
        return "internal_error";
    }
    return "internal_error";
}

int errorHttpStatus(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Ok:
        return 200;
    case ErrorCode::InvalidInput:
    case ErrorCode::InvalidUrl:
    case ErrorCode::UnsupportedHost:
    case ErrorCode::UnsupportedFormat:
    case ErrorCode::PlaylistOnly:
    case ErrorCode::ChannelUrl:
        return 400;
    case ErrorCode::Unauthorized:
        return 401;
    case ErrorCode::BinaryNotFound:
    case ErrorCode::Busy:
        return 503;
    case ErrorCode::Timeout:
        return 504;
    case ErrorCode::DiskFull:
        return 507;
    case ErrorCode::DownloadFailed:
    case ErrorCode::ConversionFailed:
    case ErrorCode::Canceled:
    case ErrorCode::ConfigError:
    case ErrorCode::Internal:
        return 500;
    }
    return 500;
}

int errorExitCode(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Ok:
        return 0;
    case ErrorCode::InvalidInput:
    case ErrorCode::InvalidUrl:
    case ErrorCode::UnsupportedHost:
    case ErrorCode::UnsupportedFormat:
    case ErrorCode::PlaylistOnly:
    case ErrorCode::ChannelUrl:
        return 2;
    case ErrorCode::BinaryNotFound:
    case ErrorCode::ConfigError:
        return 3;
    case ErrorCode::DownloadFailed:
        return 4;
    case ErrorCode::ConversionFailed:
        return 5;
    case ErrorCode::Timeout:
        return 4;
    case ErrorCode::Canceled:
        return 130;
    case ErrorCode::DiskFull:
    case ErrorCode::Busy:
    case ErrorCode::Unauthorized:
    case ErrorCode::Internal:
        return 1;
    }
    return 1;
}

} // namespace yt
