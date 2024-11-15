#include <cpprest/http_listener.h>
#include <cpprest/json.h>
#include <csignal>
#include <thread>
#include <chrono>
#include <regex>
#include "yt2mp3-API.h"

using namespace web;
using namespace web::http;
using namespace web::http::experimental::listener;

static bool keep_running = true;

// Moving required functions from YT2MP3.cpp
bool isValidURL(const std::string& url) {
    std::regex url_regex("https?://(www\\.)?youtube\\.com/watch\\?v=[^&]+");
    return std::regex_match(url, url_regex);
}

bool isValidFormat(const std::string& format) {
    return format == "mp3" || format == "mp4" || format == "wav";
}

std::string extractVideoID(const std::string& url) {
    size_t found = url.find("v=");
    if (found != std::string::npos) {
        return url.substr(found + 2);
    }
    throw std::invalid_argument("Invalid YouTube URL");
}

void downloadVideo(const std::string& url, const std::string& videoID) {
    std::string downloadCommand = "yt-dlp -f 'bestvideo[ext=mp4]+bestaudio[ext=m4a]/mp4' -o 'temp_" + videoID + ".mp4' '" + url + "'";
    if (system(downloadCommand.c_str()) != 0) {
        throw std::runtime_error("yt-dlp command failed");
    }
}

void convertVideo(const std::string& format, const std::string& videoID) {
    std::string convertCommand;
    if (format == "mp3") {
        convertCommand = "ffmpeg -i temp_" + videoID + ".mp4 -vn -ar 44100 -ac 2 -b:a 192k -codec:a libmp3lame output_" + videoID + ".mp3";
    } else if (format == "mp4") {
        convertCommand = "ffmpeg -i temp_" + videoID + ".mp4 output_" + videoID + ".mp4";
    } else if (format == "wav") {
        convertCommand = "ffmpeg -i temp_" + videoID + ".mp4 output_" + videoID + ".wav";
    }
    if (system(convertCommand.c_str()) != 0) {
        throw std::runtime_error("ffmpeg command failed");
    }
}

void processVideo(const std::string& url, const std::string& format) {
    if (!isValidURL(url) || !isValidFormat(format)) {
        throw std::invalid_argument("Invalid URL or format");
    }
    std::string videoID = extractVideoID(url);
    downloadVideo(url, videoID);
    convertVideo(format, videoID);
}

void handle_signal(int signal) {
    keep_running = false;
}

void handle_request(http_request request) {
    auto http_get_vars = uri::split_query(request.request_uri().query());
    auto url_it = http_get_vars.find(U("url"));
    auto format_it = http_get_vars.find(U("format"));

    if (url_it == http_get_vars.end() || format_it == http_get_vars.end()) {
        request.reply(status_codes::BadRequest, U("Missing url or format"));
        return;
    }

    std::string url = utility::conversions::to_utf8string(url_it->second);
    std::string format = utility::conversions::to_utf8string(format_it->second);

    try {
        std::string videoID = extractVideoID(url);
        processVideo(url, format);
        request.reply(status_codes::OK, U("The audio file has been saved as output_" + videoID + "." + format));
    } catch (const std::exception& e) {
        request.reply(status_codes::InternalError, utility::conversions::to_string_t(e.what()));
    }
}

int main() {
    signal(SIGINT, handle_signal);
    
    http_listener listener(U("http://localhost:8080"));
    listener.support(methods::GET, handle_request);

    try {
        listener
            .open()
            .then([&listener]() {
                std::cout << "\nAPI Server started. Listening on http://localhost:8080" << std::endl;
                std::cout << "Use Ctrl+C to stop the server" << std::endl;
            })
            .wait();

        while (keep_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        listener.close().wait();
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}