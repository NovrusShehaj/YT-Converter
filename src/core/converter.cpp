#include "converter.h"
#include "validation.h"
#include "logger.h"
#include <cstdlib>
#include <stdexcept>
#include <sstream>

namespace yt::converter {

std::string processVideo(const std::string& url, const std::string& format) {
    auto& logger = yt::logging::Logger::getInstance();
    
    logger.info("Starting video processing");
    logger.debug("URL: " + url + ", Format: " + format);
    
    // Validate inputs
    auto validationError = yt::validation::validateConverterInput(url, format);
    if (validationError.has_value()) {
        logger.error(validationError.value());
        throw std::invalid_argument(validationError.value());
    }

    // Extract video ID
    auto videoID = extractVideoID(url);
    if (!videoID.has_value()) {
        std::string error = "Failed to extract video ID from URL: " + url;
        logger.error(error);
        throw std::invalid_argument(error);
    }

    logger.info("Extracted video ID: " + videoID.value());

    try {
        // Download the video
        logger.info("Downloading video from YouTube...");
        downloadVideo(url, videoID.value());
        logger.info("Download completed successfully");

        // Convert the video
        logger.info("Converting video to " + format + "...");
        convertVideo(format, videoID.value());
        logger.info("Conversion completed successfully");

        std::string outputFile = getOutputFilename(videoID.value(), format);
        logger.info("Output file: " + outputFile);

        return outputFile;
    }
    catch (const std::exception& e) {
        logger.error("Video processing failed: " + std::string(e.what()));
        throw;
    }
}

std::optional<std::string> extractVideoID(const std::string& url) {
    auto& logger = yt::logging::Logger::getInstance();
    logger.debug("Extracting video ID from URL");

    // Try to find the video ID after "v="
    size_t found = url.find("v=");
    if (found != std::string::npos) {
        std::string videoID = url.substr(found + 2);
        
        // Extract only the video ID part (until & or end of string)
        size_t ampPos = videoID.find('&');
        if (ampPos != std::string::npos) {
            videoID = videoID.substr(0, ampPos);
        }

        if (!videoID.empty()) {
            logger.debug("Video ID extracted: " + videoID);
            return videoID;
        }
    }

    logger.warning("Could not extract video ID from URL");
    return std::nullopt;
}

void downloadVideo(const std::string& url, const std::string& videoID) {
    auto& logger = yt::logging::Logger::getInstance();
    logger.debug("Downloading video with ID: " + videoID);

    // Build the yt-dlp command
    std::string downloadCommand = "yt-dlp -f 'bestvideo[ext=mp4]+bestaudio[ext=m4a]/mp4' "
                                  "-o 'temp_" + videoID + ".mp4' '" + url + "'";
    
    logger.debug("Executing: yt-dlp command");
    
    int exitCode = system(downloadCommand.c_str());
    if (exitCode != 0) {
        std::string error = "yt-dlp command failed with exit code: " + std::to_string(exitCode);
        logger.error(error);
        throw std::runtime_error(error);
    }

    logger.info("Video downloaded successfully");
}

void convertVideo(const std::string& format, const std::string& videoID) {
    auto& logger = yt::logging::Logger::getInstance();
    logger.debug("Converting video with ID: " + videoID + " to format: " + format);

    std::string tempFile = "temp_" + videoID + ".mp4";
    std::string outputFile = getOutputFilename(videoID, format);
    std::string normalizedFormat = yt::validation::normalizeFormat(format);
    std::string convertCommand;

    if (normalizedFormat == "mp3") {
        convertCommand = "ffmpeg -i " + tempFile + " -vn -ar 44100 -ac 2 -b:a 192k "
                        "-codec:a libmp3lame " + outputFile;
        logger.debug("Using MP3 conversion settings (44100 Hz, stereo, 192k bitrate)");
    }
    else if (normalizedFormat == "mp4") {
        convertCommand = "ffmpeg -i " + tempFile + " -c copy " + outputFile;
        logger.debug("Using MP4 conversion (stream copy)");
    }
    else if (normalizedFormat == "wav") {
        convertCommand = "ffmpeg -i " + tempFile + " " + outputFile;
        logger.debug("Using WAV conversion");
    }
    else {
        std::string error = "Unsupported format: " + format;
        logger.error(error);
        throw std::invalid_argument(error);
    }

    logger.debug("Executing ffmpeg conversion...");
    int exitCode = system(convertCommand.c_str());
    
    if (exitCode != 0) {
        std::string error = "ffmpeg command failed with exit code: " + std::to_string(exitCode);
        logger.error(error);
        throw std::runtime_error(error);
    }

    logger.info("Conversion to " + format + " completed successfully");
}

std::string getOutputFilename(const std::string& videoID, const std::string& format) {
    std::string normalizedFormat = yt::validation::normalizeFormat(format);
    return "output_" + videoID + "." + normalizedFormat;
}

} // namespace yt::converter