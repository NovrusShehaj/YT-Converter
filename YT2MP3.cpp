#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <functional> // for std::bind
#include <regex> 
#include <stdexcept>
#include "YT2MP3.h"
#include "yt2mp3-API.h"

using namespace std;

bool isValidURL(const string& url) {
    // Simple regex to check if the URL is a valid YouTube URL
    regex url_regex("https?://(www\\.)?youtube\\.com/watch\\?v=[^&]+");
    return regex_match(url, url_regex);
}

bool isValidFormat(const string& format) {
    // Check if the format is one of the allowed formats
    return format == "mp3" || format == "mp4" || format == "wav";
}

string extractVideoID(const string& url) {
    size_t found = url.find("v=");
    if (found != string::npos) {
        return url.substr(found + 2);
    } else {
        throw invalid_argument("Invalid YouTube URL");
    }
}

void downloadVideo(const string& url, const string& videoID) {
    string downloadCommand = "yt-dlp -f 'bestvideo[ext=mp4]+bestaudio[ext=m4a]/mp4' -o 'temp_" + videoID + ".mp4' '" + url + "'";
    if (system(downloadCommand.c_str()) != 0) {
        throw runtime_error("yt-dlp command failed");
    }
}

void convertVideo(const string& format, const string& videoID) {
    string convertCommand;
    if (format == "mp3") {
        convertCommand = "ffmpeg -i temp_" + videoID + ".mp4 -vn -ar 44100 -ac 2 -b:a 192k -codec:a libmp3lame output_" + videoID + ".mp3";
    } else if (format == "mp4") {
        convertCommand = "ffmpeg -i temp_" + videoID + ".mp4 output_" + videoID + ".mp4";
    } else if (format == "wav") {
        convertCommand = "ffmpeg -i temp_" + videoID + ".mp4 output_" + videoID + ".wav";
    } else {
        throw invalid_argument("Invalid format");
    }
    if (system(convertCommand.c_str()) != 0) {
        throw runtime_error("ffmpeg command failed");
    }
}

void processVideo(const string& url, const string& format) {
    if (!isValidURL(url) || !isValidFormat(format)) {
        throw invalid_argument("Invalid URL or format");
    }

    // Extract the video ID from the URL
    string videoID = extractVideoID(url);

    downloadVideo(url, videoID);
    convertVideo(format, videoID);

    cout << "The audio file has been saved as output_" + videoID + "." + format << endl;
}

int main(int argc, char* argv[]) {

    /*main_API();
    string url;
    cout << "Enter the YouTube URL: ";
    cin >> url;

    string format;
    cout << "Enter the output format (mp3, mp4, wav): ";
    cin >> format;*/

    if (argc != 3) {
        cerr << "Usage: " << argv[0] << " <YouTube URL> <format (mp3, mp4, wav)>" << endl;
        return 1;
    }

    string url = argv[1];
    string format = argv[2];

    try {
        processVideo(url, format);
    } catch (const exception& e) {
        cerr << "Error: " << e.what() << endl;
        return 1;
    }

    return 0;

}
