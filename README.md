# YT-Converter

A command-line application that converts YouTube videos to MP3, MP4, or WAV formats using youtube-dl/yt-dlp and ffmpeg.

## Features

- Download YouTube videos using youtube-dl/yt-dlp
- Convert videos to MP3, MP4, or WAV formats using ffmpeg
- Command-line interface for easy automation
- Input validation for YouTube URLs and format types
- Error handling for download and conversion failures

## Prerequisites

- C++17 compiler
- youtube-dl or yt-dlp (`brew install yt-dlp`)
- ffmpeg (`brew install ffmpeg`)
- Boost libraries (`brew install boost`)

## Installation

1. Clone the repository:
```sh
git clone https://github.com/yourusername/YT-Converter.git
cd YT-Converter
```
2. Compile the application:
```sh
g++ -std=c++17 -o YT2MP3 YT2MP3.cpp
```

## Usage

Run the program with a YouTube URL and desired output format:
```sh
./YT2MP3 "https://www.youtube.com/watch?v=VIDEO_ID" FORMAT
```
Where FORMAT can be:

- mp3 - Convert to MP3 audio
- mp4 - Keep as MP4 video
- wav - Convert to WAV audio

## Example
```sh
./YT2MP3 "https://www.youtube.com/watch?v=r6tMTzEiGPI" mp3
```

## How it Works

1. URL Validation: Uses regex to validate YouTube URL format
2. Video ID Extraction: Extracts video ID from the URL
3. Download: Uses yt-dlp to download the video in best quality
4. Conversion: Uses ffmpeg to convert the video to the desired format
    - MP3: Converts to 192kbps stereo audio
    - MP4: Keeps original video format
    - WAV: Converts to uncompressed audio

## Troubleshooting

1. If you get a 403 Forbidden error:
```sh
brew upgrade yt-dlp
```

2. If ffmpeg fails:
```sh
brew upgrade ffmpeg
```

3. Common Issues:
    - URL must be in quotes to handle special characters
    - Check internet connection
    - Ensure sufficient disk space
    - Verify video is not age-restricted or private

## Project Structure
.
├── YT2MP3.cpp      # Main source file
├── YT2MP3.h        # Header file
├── README.md       # Documentation
└── build/          # Build directory

## License
This project is licensed under the MIT License. See the LICENSE file for details.
