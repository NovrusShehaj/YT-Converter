# YT-Converter

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C++](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![CMake](https://img.shields.io/badge/CMake-3.10%2B-064F8C.svg)](https://cmake.org/)
[![Status](https://img.shields.io/badge/Status-Active-brightgreen.svg)](#)

A powerful command-line tool and REST API for converting YouTube videos to MP3, MP4, and WAV formats. Built with modern C++ for high performance and reliability.

![YT-Converter](https://img.shields.io/badge/YouTube%20Converter-Multi--Format-red.svg)

## 📋 Table of Contents

- [Features](#-features)
- [Quick Start](#-quick-start)
- [Prerequisites](#-prerequisites)
- [Installation](#-installation)
- [Usage](#-usage)
  - [Command-Line Interface](#command-line-interface)
  - [REST API](#rest-api)
- [Architecture](#-architecture)
- [Project Structure](#-project-structure)
- [API Reference](#-api-reference)
- [Contributing](#-contributing)
- [License](#-license)
- [Acknowledgments](#-acknowledgments)
- [Troubleshooting](#-troubleshooting)

## ✨ Features

- **Multi-Format Conversion**: Convert YouTube videos to MP3, MP4, or WAV formats
- **Dual Interface**: Use as a command-line tool or access via REST API
- **Input Validation**: Comprehensive YouTube URL and format validation
- **Error Handling**: Robust error handling for download and conversion failures
- **High Quality**: Downloads in best available quality and maintains audio/video fidelity
- **Cross-Platform**: Compatible with macOS, Linux, and Windows
- **Asynchronous Processing**: Non-blocking API for concurrent conversions
- **Lightweight & Fast**: Efficient resource usage with minimal dependencies

## 🚀 Quick Start

### CLI Example
```sh
# Convert YouTube video to MP3
./yt2mp3-cli "https://www.youtube.com/watch?v=VIDEO_ID" mp3

# Convert to WAV format
./yt2mp3-cli "https://www.youtube.com/watch?v=VIDEO_ID" wav

# Convert to MP4 format
./yt2mp3-cli "https://www.youtube.com/watch?v=VIDEO_ID" mp4
```

### API Example
```sh
# Start the API server
./yt2mp3-API

# In another terminal, request a conversion
curl "http://localhost:8080?url=https://www.youtube.com/watch?v=VIDEO_ID&format=mp3"
```

## 📦 Prerequisites

Before installing YT-Converter, ensure you have the following installed:

| Requirement | Version | Installation |
|-----------|---------|--------------|
| C++ Compiler | C++17+ | `brew install gcc@11` or `brew install clang` |
| CMake | 3.10+ | `brew install cmake` |
| yt-dlp | Latest | `brew install yt-dlp` |
| ffmpeg | 4.0+ | `brew install ffmpeg` |
| Boost | 1.70+ | `brew install boost` |
| cpprestsdk | 2.10+ | `brew install cpprestsdk` |
| OpenSSL | 1.1+ | `brew install openssl` |

### macOS Installation
```sh
brew install cmake boost cpprestsdk openssl yt-dlp ffmpeg
```

### Linux Installation (Ubuntu/Debian)
```sh
sudo apt-get install build-essential cmake libboost-all-dev libcpprest-dev libssl-dev
sudo apt-get install ffmpeg
pip install yt-dlp
```

## 🔧 Installation

### 1. Clone the Repository
```sh
git clone https://github.com/yourusername/YT-Converter.git
cd YT-Converter
```

### 2. Create Build Directory
```sh
mkdir build
cd build
```

### 3. Build the Project
```sh
cmake ..
make
```

### 4. Verify Installation
```sh
# Test the CLI
./yt2mp3-cli --help

# Test the API
./yt2mp3-API
# Should output: Server listening on http://localhost:8080
```

## 💻 Usage

### Command-Line Interface

#### Basic Usage
```sh
./yt2mp3-cli "<YouTube_URL>" <FORMAT>
```

#### Parameters
- `<YouTube_URL>`: Full YouTube URL (must be quoted)
- `<FORMAT>`: Output format - `mp3`, `mp4`, or `wav`

#### Examples

**Convert to MP3 (192kbps stereo audio)**
```sh
./yt2mp3-cli "https://www.youtube.com/watch?v=r6tMTzEiGPI" mp3
# Output: Successfully saved as output_r6tMTzEiGPI.mp3
```

**Convert to MP4 (original format)**
```sh
./yt2mp3-cli "https://www.youtube.com/watch?v=r6tMTzEiGPI" mp4
# Output: Successfully saved as output_r6tMTzEiGPI.mp4
```

**Convert to WAV (uncompressed audio)**
```sh
./yt2mp3-cli "https://www.youtube.com/watch?v=r6tMTzEiGPI" wav
# Output: Successfully saved as output_r6tMTzEiGPI.wav
```

#### Output Directory Structure
```
YT-Converter/
├── MP3/           # MP3 conversions
├── MP4/           # MP4 downloads
├── WAV/           # WAV conversions
└── output_*.*     # Converted files
```

### REST API

#### Starting the Server
```sh
./yt2mp3-API
```

Expected output:
```
Server is listening on http://localhost:8080
```

#### API Endpoints

##### Convert Endpoint
- **URL**: `http://localhost:8080`
- **Method**: `GET`
- **Query Parameters**:
  - `url` (required): YouTube video URL
  - `format` (required): Output format (`mp3`, `mp4`, `wav`)

#### API Examples

**Convert to MP3 using cURL**
```sh
curl "http://localhost:8080?url=https://www.youtube.com/watch?v=VIDEO_ID&format=mp3"
```

**Convert to MP4 using Python**
```python
import requests

response = requests.get(
    "http://localhost:8080",
    params={
        "url": "https://www.youtube.com/watch?v=VIDEO_ID",
        "format": "mp4"
    }
)
print(response.json())
```

**Convert to WAV using JavaScript/Node.js**
```javascript
fetch('http://localhost:8080?url=https://www.youtube.com/watch?v=VIDEO_ID&format=wav')
  .then(response => response.json())
  .then(data => console.log(data))
  .catch(error => console.error('Error:', error));
```

#### Success Response
```json
{
    "status": 200,
    "message": "The audio file has been saved as output_VIDEO_ID.mp3"
}
```

#### Error Responses

**Missing Parameters (400)**
```json
{
    "status": 400,
    "error": "Missing url or format parameter"
}
```

**Invalid URL (400)**
```json
{
    "status": 400,
    "error": "Invalid YouTube URL format"
}
```

**Server Error (500)**
```json
{
    "status": 500,
    "error": "Failed to download or convert video"
}
```

#### HTTP Status Codes
| Code | Meaning |
|------|---------|
| 200 | Success - conversion completed |
| 400 | Bad Request - invalid URL or format |
| 500 | Server Error - download/conversion failed |
| 503 | Service Unavailable - server overloaded |

## 🏗️ Architecture

### System Overview
```
User Input (CLI/API)
        │
        ├─────────────────────────────────────────┐
        │                                          │
    [CLI Handler]                          [API Handler]
        │                                          │
        └─────────────────────┬────────────────────┘
                             │
                      [Validation Layer]
                             │
                   ┌──────────┴──────────┐
                   │                    │
              [URL Validator]    [Format Validator]
                   │                    │
                   └──────────┬─────────┘
                             │
                      [Processing Core]
                             │
                   ┌──────────┴──────────┐
                   │                    │
                [yt-dlp]           [ffmpeg]
                   │                    │
        ┌──────────┘                    │
        │                              │
    [Download]                    [Convert]
        │                              │
        └──────────────┬───────────────┘
                      │
                [Output Files]
                      │
        ┌─────────────┼─────────────┐
        │             │             │
      [MP3]         [MP4]         [WAV]
```

### Data Flow
1. **Input Validation**: URL and format validation using regex patterns
2. **Video ID Extraction**: Isolates video ID from the YouTube URL
3. **Download**: yt-dlp retrieves the video in best available quality
4. **Conversion**: ffmpeg converts to requested format with appropriate settings
5. **Output**: Files saved to format-specific directories

### Conversion Specifications

| Format | Audio Codec | Bitrate | Sample Rate | Channels |
|--------|-------------|---------|-------------|----------|
| MP3 | libmp3lame | 192 kbps | 48000 Hz | Stereo |
| WAV | PCM | Lossless | 48000 Hz | Stereo |
| MP4 | Original | Original | Original | Original |

## 📁 Project Structure

```
YT-Converter/
├── CMakeLists.txt              # Build configuration
├── README.md                   # This file
├── LICENSE                     # MIT License
├── .gitignore                  # Git ignore rules
├── CONTRIBUTING.md             # Contribution guidelines
├── CODE_OF_CONDUCT.md          # Community standards
│
├── src/                        # Source files
│   ├── cli/                    # Command-line interface
│   │   └── main.cpp
│   ├── api/                    # REST API server
│   │   ├── server.cpp
│   │   └── handlers.cpp
│   ├── core/                   # Core functionality
│   │   ├── converter.cpp
│   │   └── validator.cpp
│   └── utils/                  # Utility functions
│       ├── logger.cpp
│       └── helpers.cpp
│
├── include/                    # Header files
│   ├── converter.h
│   ├── validator.h
│   └── api.h
│
├── YT2MP3.h / YT2MP3.cpp       # Legacy CLI implementation
├── yt2mp3-API.h / yt2mp3-API.cpp # Legacy API implementation
│
├── docs/                       # Documentation
│   ├── ARCHITECTURE.md         # Technical architecture
│   └── API.md                  # API documentation
│
├── tests/                      # Unit tests
│   ├── test_validator.cpp
│   └── test_converter.cpp
│
├── MP3/, MP4/, WAV/            # Output directories
└── build/                      # Build artifacts (git-ignored)
```

## 📚 API Reference

### Core Functions

#### URL Validation
```cpp
bool isValidURL(const std::string& url);
```
Validates YouTube URL format using regex pattern matching.

#### Format Validation
```cpp
bool isValidFormat(const std::string& format);
```
Validates output format is one of: mp3, mp4, wav.

#### Video ID Extraction
```cpp
std::string extractVideoID(const std::string& url);
```
Extracts the video ID from YouTube URL.

#### Download Video
```cpp
void downloadVideo(const std::string& url, const std::string& videoID);
```
Downloads video using yt-dlp in best quality.

#### Convert Video
```cpp
void convertVideo(const std::string& format, const std::string& videoID);
```
Converts downloaded video to specified format using ffmpeg.

#### Process Video
```cpp
void processVideo(const std::string& url, const std::string& format);
```
Complete pipeline: validate → download → convert → save.

## 🤝 Contributing

We welcome contributions from the community! Please read our [CONTRIBUTING.md](CONTRIBUTING.md) guide for details on our code of conduct and the process for submitting pull requests.

### Quick Contributing Guide

1. **Fork** the repository
2. **Create** a feature branch (`git checkout -b feature/AmazingFeature`)
3. **Commit** your changes (`git commit -m 'Add AmazingFeature'`)
4. **Push** to the branch (`git push origin feature/AmazingFeature`)
5. **Open** a Pull Request

### Development Setup
```sh
git clone https://github.com/yourusername/YT-Converter.git
cd YT-Converter
mkdir build && cd build
cmake .. && make
```

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## 🙏 Acknowledgments

This project stands on the shoulders of giants:

- **[yt-dlp](https://github.com/yt-dlp/yt-dlp)** - Advanced YouTube downloader
- **[ffmpeg](https://ffmpeg.org/)** - Multimedia framework for audio/video processing
- **[cpprestsdk](https://github.com/microsoft/cpprestsdk)** - C++ REST SDK by Microsoft
- **[Boost Libraries](https://www.boost.org/)** - Peer-reviewed C++ libraries
- **[CMake](https://cmake.org/)** - Build system generator

Special thanks to all contributors and the open-source community.

## 🐛 Troubleshooting

### Common Issues and Solutions

#### 1. yt-dlp: 403 Forbidden Error
```sh
# Update yt-dlp to the latest version
brew upgrade yt-dlp
# or
pip install --upgrade yt-dlp
```

#### 2. ffmpeg Not Found
```sh
# Install ffmpeg
brew install ffmpeg

# Verify installation
ffmpeg -version
```

#### 3. CMake Configuration Fails
```sh
# Clear build directory and retry
rm -rf build
mkdir build && cd build
cmake ..
make
```

#### 4. API Port Already in Use
```sh
# Check what's using port 8080
lsof -i :8080

# Kill the process
kill -9 <PID>

# Or modify the port in the source code and rebuild
```

#### 5. URL Must Be Quoted
**Incorrect:**
```sh
./yt2mp3-cli https://www.youtube.com/watch?v=VIDEO_ID mp3
# Error: Shell expands URL
```

**Correct:**
```sh
./yt2mp3-cli "https://www.youtube.com/watch?v=VIDEO_ID" mp3
```

#### 6. Age-Restricted or Private Videos
Currently, YT-Converter cannot download age-restricted or private videos. These limitations are inherent to yt-dlp's functionality.

#### 7. Insufficient Disk Space
Ensure you have adequate disk space for downloads and conversions:
```sh
# Check available space
df -h

# Clean up old conversions
rm MP3/*.mp3
rm MP4/*.mp4
rm WAV/*.wav
```

### Getting Help

- Check the [Architecture](docs/ARCHITECTURE.md) documentation
- Review existing [GitHub Issues](https://github.com/yourusername/YT-Converter/issues)
- Open a new issue with detailed error messages and steps to reproduce

---

**Made with ❤️ by the YT-Converter team**

Last updated: 2024