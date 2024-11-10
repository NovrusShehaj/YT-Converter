# YT-Converter

YT-Converter is a command-line and API-based application that converts YouTube links into MP3, MP4, or WAV files. This project leverages `youtube-dl` for downloading videos and `ffmpeg` for converting them into the desired format.

## Features

- Convert YouTube videos to MP3, MP4, or WAV formats.
- Command-line interface for direct usage.
- REST API for programmatic access.

## Prerequisites

- `youtube-dl`: Ensure `youtube-dl` is installed and accessible in your system's PATH.
- `ffmpeg`: Ensure `ffmpeg` is installed and accessible in your system's PATH.
- C++17 or later
- CMake 3.10 or later
- Boost libraries
- cpprestsdk

## Installation

1. Clone the repository:
    ```sh
    git clone https://github.com/yourusername/YT-Converter.git
    cd YT-Converter
    ```

2. Build the project using CMake:
    ```sh
    mkdir build
    cd build
    cmake ..
    make
    ```

## Usage

### Command-Line Interface

1. Run the command-line program:
    ```sh
    ./YT2MP3
    ```

2. Follow the prompts to enter the YouTube URL and the desired output format (mp3, mp4, wav).

### API Interface

1. Run the API server:
    ```sh
    ./yt2mp3-API
    ```

2. Make a GET request to the API with the YouTube URL and desired format:
    ```sh
    curl "http://localhost:8080/convert?url=<YouTube_URL>&format=<format>"
    ```


## Source Files

- `YT2MP3.cpp`: Contains the main logic for the command-line interface.
- `yt2mp3-API.cpp`: Contains the main logic for the API interface.
- `YT2MP3.h`: Header file for the command-line interface.
- `yt2mp3-API.h`: Header file for the API interface.

## Functions

### Command-Line Interface

- `bool isValidURL(const string& url)`: Validates the YouTube URL.
- `bool isValidFormat(const string& format)`: Validates the output format.
- `string extractVideoID(const string& url)`: Extracts the video ID from the YouTube URL.
- `void downloadVideo(const string& url, const string& videoID)`: Downloads the video using `youtube-dl`.
- `void convertVideo(const string& format, const string& videoID)`: Converts the downloaded video to the specified format using `ffmpeg`.
- `void processVideo(const string& url, const string& format)`: Processes the video by downloading and converting it.
- `int main()`: Entry point for the command-line interface.

### API Interface

- `void handle_request(http_request request)`: Handles incoming HTTP requests.
- `int main_API()`: Entry point for the API interface.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- [youtube-dl](https://github.com/ytdl-org/youtube-dl)
- [ffmpeg](https://ffmpeg.org/)
- [cpprestsdk](https://github.com/microsoft/cpprestsdk)
- [Boost Libraries](https://www.boost.org/)

