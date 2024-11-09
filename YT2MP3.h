#ifndef YT2MP3_H
#define YT2MP3_H

#include <string>

bool isValidURL(const std::string& url);
bool isValidFormat(const std::string& format);
std::string extractVideoID(const std::string& url);
void downloadVideo(const std::string& url, const std::string& videoID);
void convertVideo(const std::string& format, const std::string& videoID);
void processVideo(const std::string& url, const std::string& format);


#endif // YT2MP3_H