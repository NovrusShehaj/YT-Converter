#ifndef YT_CONVERTER_VALIDATION_H
#define YT_CONVERTER_VALIDATION_H

#include <string>
#include <optional>
#include <vector>

namespace yt::validation {

/**
 * @brief Validates if a string is a valid YouTube URL
 * @param url The URL string to validate
 * @return true if the URL is a valid YouTube URL, false otherwise
 * 
 * Supports both www and non-www YouTube URLs with the following formats:
 * - https://www.youtube.com/watch?v=VIDEO_ID
 * - https://youtube.com/watch?v=VIDEO_ID
 * - http://www.youtube.com/watch?v=VIDEO_ID
 * - http://youtube.com/watch?v=VIDEO_ID
 * 
 * @note The function uses regex matching to validate the URL format
 */
bool isValidURL(const std::string& url);

/**
 * @brief Validates if a format string is a supported output format
 * @param format The format string to validate
 * @return true if the format is supported (mp3, mp4, or wav), false otherwise
 * 
 * Supported formats are case-insensitive:
 * - mp3: MPEG-1 Audio Layer III
 * - mp4: MPEG-4 Video
 * - wav: Waveform Audio File Format
 */
bool isValidFormat(const std::string& format);

/**
 * @brief Validates if a string is not empty and meets minimum length requirements
 * @param input The string to validate
 * @param minLength The minimum required length (default: 1)
 * @return true if the input is valid, false otherwise
 */
bool isValidString(const std::string& input, size_t minLength = 1);

/**
 * @brief Returns a list of supported output formats
 * @return A vector containing all supported format strings (lowercase)
 */
std::vector<std::string> getSupportedFormats();

/**
 * @brief Gets a human-readable error message for URL validation failure
 * @param url The invalid URL
 * @return A descriptive error message
 */
std::string getURLValidationError(const std::string& url);

/**
 * @brief Gets a human-readable error message for format validation failure
 * @param format The invalid format
 * @return A descriptive error message with supported formats listed
 */
std::string getFormatValidationError(const std::string& format);

/**
 * @brief Normalizes a format string to lowercase
 * @param format The format string to normalize
 * @return The normalized (lowercase) format string
 */
std::string normalizeFormat(const std::string& format);

/**
 * @brief Validates command-line arguments for the converter
 * @param url The YouTube URL to validate
 * @param format The output format to validate
 * @return An optional containing error message if validation fails, std::nullopt if valid
 */
std::optional<std::string> validateConverterInput(const std::string& url, const std::string& format);

} // namespace yt::validation

#endif // YT_CONVERTER_VALIDATION_H