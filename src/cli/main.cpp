#include "converter.h"
#include "validation.h"
#include "logger.h"
#include <iostream>
#include <string>

/**
 * @file main.cpp
 * @brief Command-line interface for the YouTube converter
 * 
 * Usage: yt-converter <YouTube URL> <format>
 * 
 * Formats: mp3, mp4, wav
 * Example: yt-converter "https://www.youtube.com/watch?v=dQw4w9WgXcQ" mp3
 */

void printUsage(const char* programName) {
    std::cerr << "YouTube Converter - Command Line Interface\n\n"
              << "Usage: " << programName << " <URL> <format>\n\n"
              << "Arguments:\n"
              << "  URL      - YouTube video URL\n"
              << "             (e.g., https://www.youtube.com/watch?v=VIDEO_ID)\n"
              << "  format   - Output format: mp3, mp4, or wav\n\n"
              << "Examples:\n"
              << "  " << programName << " \"https://www.youtube.com/watch?v=dQw4w9WgXcQ\" mp3\n"
              << "  " << programName << " \"https://youtube.com/watch?v=jNQXAC9IVRw\" mp4\n"
              << "\nSupported Formats:\n"
              << "  mp3  - MPEG-1 Audio Layer III (audio only)\n"
              << "  mp4  - MPEG-4 Video Container\n"
              << "  wav  - Waveform Audio File Format\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    auto& logger = yt::logger::Logger::getInstance();
    
    // Configure logger for CLI use
    logger.setLogLevel(yt::logger::LogLevel::INFO);

    logger.info("YouTube Converter CLI");

    // Validate command-line arguments
    if (argc != 3) {
        logger.error("Invalid number of arguments provided");
        printUsage(argv[0]);
        return 1;
    }

    std::string url = argv[1];
    std::string format = argv[2];

    logger.debug("URL: " + url);
    logger.debug("Format: " + format);

    try {
        // Process the video using the shared converter library
        std::string outputFile = yt::converter::processVideo(url, format);
        
        std::cout << "\n" << std::string(50, '=') << std::endl;
        std::cout << "✓ Conversion completed successfully!" << std::endl;
        std::cout << "Output file: " << outputFile << std::endl;
        std::cout << std::string(50, '=') << std::endl << std::endl;
        
        return 0;
    }
    catch (const std::invalid_argument& e) {
        logger.error("Validation error: " + std::string(e.what()));
        std::cerr << "\n✗ Error: Invalid input\n"
                  << "Details: " << e.what() << std::endl;
        printUsage(argv[0]);
        return 1;
    }
    catch (const std::runtime_error& e) {
        logger.error("Runtime error: " + std::string(e.what()));
        std::cerr << "\n✗ Error: Conversion failed\n"
                  << "Details: " << e.what() << "\n\n"
                  << "Troubleshooting:\n"
                  << "  - Ensure yt-dlp is installed: pip install yt-dlp\n"
                  << "  - Ensure ffmpeg is installed\n"
                  << "  - Check that you have internet connectivity\n"
                  << "  - Verify the YouTube URL is correct and public\n"
                  << std::endl;
        return 1;
    }
    catch (const std::exception& e) {
        logger.critical("Unexpected error: " + std::string(e.what()));
        std::cerr << "\n✗ Unexpected error: " << e.what() << std::endl;
        return 1;
    }
}