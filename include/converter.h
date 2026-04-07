#ifndef YT_CONVERTER_CONVERTER_H
#define YT_CONVERTER_CONVERTER_H

#include <string>
#include <optional>

namespace yt::converter {

/**
 * @brief Validates if a string is a valid YouTube URL
 * @param url The URL string to validate
 * @return true if the URL is a valid YouTube URL, false otherwise
 * 
 * Supports both www and non-www YouTube URLs with the following format:
 * https://www.youtube.com/watch?v=VIDEO_ID
 * https://youtube.com/watch?v=VIDEO_ID
 */
bool isValidURL(const std::string& url);

/**
 * @brief Validates if a format string is a supported output format
 * @param format The format string to validate (should be lowercase)
 * @return true if the format is supported (mp3, mp4, wav), false otherwise
 */
bool isValidFormat(const std::string& format);

/**
 * @brief Extracts the video ID from a YouTube URL
 * @param url The YouTube URL to extract the video ID from
 * @return The extracted video ID, or std::nullopt if extraction fails
 */
std::optional<std::string> extractVideoID(const std::string& url);

/**
 * @brief Downloads a video from YouTube using yt-dlp
 * @param url The YouTube URL to download from
 * @param videoID The video ID (used for naming the temporary file)
 * @throws std::runtime_error if the download fails
 * 
 * Downloads the best available video and audio combination and saves it
 * as a temporary MP4 file: temp_VIDEO_ID.mp4
 */
void downloadVideo(const std::string& url, const std::string& videoID);

/**
 * @brief Converts a video to the specified format using ffmpeg
 * @param format The output format (mp3, mp4, or wav)
 * @param videoID The video ID (used for naming input and output files)
 * @throws std::invalid_argument if the format is not supported
 * @throws std::runtime_error if the conversion fails
 * 
 * Input file: temp_VIDEO_ID.mp4
 * Output file: output_VIDEO_ID.FORMAT
 * 
 * Conversion specifications:
 * - MP3: 44100 Hz, stereo, 192k bitrate, libmp3lame codec
 * - MP4: Direct copy with no re-encoding
 * - WAV: Direct conversion maintaining quality
 */
void convertVideo(const std::string& format, const std::string& videoID);

/**
 * @brief Processes a complete YouTube video download and conversion workflow
 * @param url The YouTube URL to process
 * @param format The desired output format (mp3, mp4, or wav)
 * @return The output filename on success
 * @throws std::invalid_argument if URL or format is invalid
 * @throws std::runtime_error if download or conversion fails
 * 
 * This function orchestrates the full process:
 * 1. Validates the URL and format
 * 2. Extracts the video ID
 * 3. Downloads the video
 * 4. Converts to the requested format
 * 
 * Output file: output_VIDEO_ID.FORMAT
 */
std::string processVideo(const std::string& url, const std::string& format);

/**
 * @brief Gets the full output filename for a given video and format
 * @param videoID The video ID
 * @param format The output format
 * @return The complete output filename (e.g., output_abc123.mp3)
 */
std::string getOutputFilename(const std::string& videoID, const std::string& format);

} // namespace yt::converter

#endif // YT_CONVERTER_CONVERTER_H