#include <cpprest/http_listener.h>
#include <cpprest/json.h>
#include "yt2mp3.h"
#include "yt2mp3-API.h"

using namespace web;
using namespace web::http;
using namespace web::http::experimental::listener;

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

int main_API() {
    http_listener listener(U("http://localhost:8080"));

    listener.support(methods::GET, handle_request);

    try {
        listener
            .open()
            .then([&listener]() { std::cout << "Starting to listen on " << listener.uri().to_string() << std::endl; })
            .wait();

        while (true);
    } catch (std::exception const & e) {
        std::cout << e.what() << std::endl;
    }

    return 0;
}