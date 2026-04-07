#include "converter.h"
#include "validation.h"
#include "logger.h"
#include <cpprest/http_listener.h>
#include <cpprest/json.h>
#include <csignal>
#include <thread>
#include <chrono>
#include <atomic>

using namespace web;
using namespace web::http;
using namespace web::http::experimental::listener;

/**
 * @file server.cpp
 * @brief HTTP API server for YouTube converter
 * 
 * RESTful API endpoints:
 * GET /convert?url=<YouTube URL>&format=<mp3|mp4|wav>
 * 
 * Response (success):
 * {
 *   "status": "success",
 *   "output_file": "output_VIDEO_ID.FORMAT"
 * }
 * 
 * Response (error):
 * {
 *   "status": "error",
 *   "error": "Error description"
 * }
 */

// Global flag for graceful shutdown
static std::atomic<bool> shouldShutdown(false);

/**
 * @brief Signal handler for graceful shutdown
 * @param signal The signal number
 */
void signalHandler(int signal) {
    auto& logger = yt::logging::Logger::getInstance();
    if (signal == SIGINT) {
        logger.info("Received SIGINT signal, shutting down gracefully...");
        shouldShutdown = true;
    }
}

/**
 * @brief Handle HTTP GET requests for video conversion
 * @param request The HTTP request object
 */
void handleRequest(http_request request) {
    auto& logger = yt::logging::Logger::getInstance();
    
    try {
        // Parse query parameters
        auto queryParams = uri::split_query(request.request_uri().query());
        
        auto urlIt = queryParams.find(U("url"));
        auto formatIt = queryParams.find(U("format"));

        // Validate that required parameters are present
        if (urlIt == queryParams.end() || formatIt == queryParams.end()) {
            logger.warning("Request missing required parameters");
            
            json::value errorResponse;
            errorResponse[U("status")] = json::value::string(U("error"));
            errorResponse[U("error")] = json::value::string(
                U("Missing required parameters. Please provide 'url' and 'format'")
            );
            
            request.reply(status_codes::BadRequest, errorResponse);
            return;
        }

        // Convert from wide strings to UTF-8
        std::string url = utility::conversions::to_utf8string(urlIt->second);
        std::string format = utility::conversions::to_utf8string(formatIt->second);

        logger.info("API request received - URL: " + url + ", Format: " + format);
        logger.debug("Client IP: " + request.remote_address());

        // Validate input
        auto validationError = yt::validation::validateConverterInput(url, format);
        if (validationError) {
            logger.warning("Validation failed: " + *validationError);
            
            json::value errorResponse;
            errorResponse[U("status")] = json::value::string(U("error"));
            errorResponse[U("error")] = json::value::string(
                utility::conversions::to_string_t(*validationError)
            );
            
            request.reply(status_codes::BadRequest, errorResponse);
            return;
        }

        // Process the video
        logger.info("Starting video conversion via API");
        std::string outputFile = yt::converter::processVideo(url, format);
        
        logger.info("API conversion successful: " + outputFile);

        // Send success response
        json::value successResponse;
        successResponse[U("status")] = json::value::string(U("success"));
        successResponse[U("output_file")] = json::value::string(
            utility::conversions::to_string_t(outputFile)
        );
        successResponse[U("message")] = json::value::string(
            utility::conversions::to_string_t("Video converted successfully")
        );

        request.reply(status_codes::OK, successResponse);
    }
    catch (const std::invalid_argument& e) {
        auto& logger = yt::logging::Logger::getInstance();
        logger.warning("Invalid argument error: " + std::string(e.what()));
        
        json::value errorResponse;
        errorResponse[U("status")] = json::value::string(U("error"));
        errorResponse[U("error")] = json::value::string(
            utility::conversions::to_string_t(std::string(e.what()))
        );
        
        request.reply(status_codes::BadRequest, errorResponse);
    }
    catch (const std::runtime_error& e) {
        auto& logger = yt::logging::Logger::getInstance();
        logger.error("Runtime error: " + std::string(e.what()));
        
        json::value errorResponse;
        errorResponse[U("status")] = json::value::string(U("error"));
        errorResponse[U("error")] = json::value::string(
            utility::conversions::to_string_t(std::string(e.what()))
        );
        
        request.reply(status_codes::InternalError, errorResponse);
    }
    catch (const std::exception& e) {
        auto& logger = yt::logging::Logger::getInstance();
        logger.critical("Unexpected error: " + std::string(e.what()));
        
        json::value errorResponse;
        errorResponse[U("status")] = json::value::string(U("error"));
        errorResponse[U("error")] = json::value::string(U("Internal server error"));
        
        request.reply(status_codes::InternalError, errorResponse);
    }
}

/**
 * @brief Print server information and usage
 */
void printServerInfo() {
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "YouTube Converter - REST API Server" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    std::cout << "\nServer started on http://localhost:8080" << std::endl;
    std::cout << "\nAPI Endpoint:" << std::endl;
    std::cout << "  GET /convert?url=<URL>&format=<FORMAT>" << std::endl;
    std::cout << "\nParameters:" << std::endl;
    std::cout << "  url    - YouTube video URL" << std::endl;
    std::cout << "  format - Output format (mp3, mp4, or wav)" << std::endl;
    std::cout << "\nExample Request:" << std::endl;
    std::cout << "  http://localhost:8080/convert?url=https://www.youtube.com/watch?v=dQw4w9WgXcQ&format=mp3" << std::endl;
    std::cout << "\nPress Ctrl+C to stop the server" << std::endl;
    std::cout << std::string(60, '=') << std::endl << std::endl;
}

int main() {
    auto& logger = yt::logging::Logger::getInstance();
    
    // Configure logger for API server
    logger.setLogLevel(yt::logging::LogLevel::Debug);
    logger.setShowTimestamp(true);

    logger.info("Initializing YouTube Converter API Server");

    // Set up signal handler for graceful shutdown
    signal(SIGINT, signalHandler);

    try {
        // Create HTTP listener on port 8080
        http_listener listener(U("http://localhost:8080"));
        
        // Set up request handler for /convert endpoint
        listener.support(methods::GET, handleRequest);

        logger.info("Starting HTTP listener...");
        
        // Open the listener
        listener
            .open()
            .then([&listener]() {
                auto& logger = yt::logging::Logger::getInstance();
                logger.info("HTTP listener opened successfully");
            })
            .wait();

        printServerInfo();

        // Keep the server running until shutdown signal
        while (!shouldShutdown) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        logger.info("Shutting down server...");
        listener.close().wait();
        logger.info("Server stopped");
    }
    catch (const std::exception& e) {
        logger.critical("Server error: " + std::string(e.what()));
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}