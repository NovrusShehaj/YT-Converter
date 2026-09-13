#ifndef YT_CONVERTER_CONFIG_H
#define YT_CONVERTER_CONFIG_H

#include <string>

namespace yt {

struct Config {
    std::string bind = "127.0.0.1";
    int port = 8080;
    std::string log_level = "INFO";
    std::string log_format = "text";
    std::string output_dir = "./output";
    int max_concurrent = 1;
    int child_timeout_sec = 900;
    int socket_timeout_sec = 30;
    std::string max_filesize = "500M";
    int retries = 2;
    int max_height = 1080;
    std::string yt_dlp_path = "yt-dlp";
    std::string ffmpeg_path = "ffmpeg";
    std::string api_key;
    bool allow_remote = false;
    bool allow_unauthenticated_localhost = false;
    bool log_urls = false;
    bool force = false;
    bool no_unicode = false;
    bool reuse_completed = true;
};

Config loadConfigFromEnv();
void applyLogConfig(const Config& config);
bool isLoopbackBind(const std::string& bind);
bool isWildcardBind(const std::string& bind);
bool constantTimeEquals(const std::string& left, const std::string& right);
void validateApiConfig(const Config& config);
std::string resolveOutputRoot(const std::string& output_dir);

} // namespace yt

#endif // YT_CONVERTER_CONFIG_H
