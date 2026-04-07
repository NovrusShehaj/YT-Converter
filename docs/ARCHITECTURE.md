# YT-Converter Architecture

## Table of Contents

- [Overview](#overview)
- [System Architecture](#system-architecture)
- [Component Design](#component-design)
- [Data Flow](#data-flow)
- [Key Design Decisions](#key-design-decisions)
- [Technology Stack](#technology-stack)
- [Performance Considerations](#performance-considerations)
- [Security Considerations](#security-considerations)
- [Future Improvements](#future-improvements)

## Overview

YT-Converter is a dual-interface application for downloading and converting YouTube videos to multiple formats. It provides both a command-line interface (CLI) and a REST API server, built on modern C++ for high performance and reliability.

### Core Objectives

- **Reliability**: Robust error handling and validation
- **Performance**: Efficient resource usage with minimal dependencies
- **Flexibility**: Support for multiple output formats (MP3, MP4, WAV)
- **Accessibility**: Easy-to-use CLI and programmatic API interface
- **Maintainability**: Clean separation of concerns and modular design

## System Architecture

### High-Level Architecture Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                        User Interface                        │
├──────────────────────────┬──────────────────────────────────┤
│     CLI Interface        │        REST API Server            │
│  (YT2MP3.cpp)           │      (yt2mp3-API.cpp)             │
└──────────────────────────┴──────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────┐
│                   Input Validation Layer                    │
├──────────────────────────┬──────────────────────────────────┤
│   URL Validator          │    Format Validator              │
│  - Regex matching        │  - MP3/MP4/WAV check             │
│  - Format verification   │  - Case insensitivity            │
└──────────────────────────┴──────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────┐
│                  Processing Core Layer                      │
├──────────────────────────┬──────────────────────────────────┤
│   Video ID Extractor     │   Download Manager               │
│  - YouTube URL parsing   │  - yt-dlp integration            │
│  - Video ID isolation    │  - Quality selection             │
└──────────────────────────┴──────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────┐
│                  Conversion Layer                           │
├──────────────────────────┬──────────────────────────────────┤
│   Format Converter       │   FFmpeg Integration             │
│  - Format routing        │  - Audio/video encoding          │
│  - Codec selection       │  - Quality profiles              │
└──────────────────────────┴──────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────┐
│                    Output Storage                           │
├────────────┬────────────────────┬─────────────────────────┤
│   MP3/     │   MP4/             │   WAV/                  │
│  Directory │   Directory        │   Directory             │
└────────────┴────────────────────┴─────────────────────────┘
```

## Component Design

### 1. CLI Interface (`YT2MP3.cpp`)

**Responsibility**: Command-line argument parsing and user interaction

**Key Functions**:
- Parse command-line arguments
- Display help and usage information
- Call core processing functions
- Return appropriate exit codes

**Execution Flow**:
```
User Command
    │
    ├─ Parse Arguments
    │   └─ Extract URL and format
    │
    ├─ Validate Input
    │   └─ Check URL and format validity
    │
    ├─ Process Video
    │   ├─ Download
    │   ├─ Convert
    │   └─ Save Output
    │
    └─ Display Result
        └─ Success/Error message
```

### 2. REST API Server (`yt2mp3-API.cpp`)

**Responsibility**: HTTP server and request handling

**Key Features**:
- Listens on `http://localhost:8080`
- Query parameter parsing
- Asynchronous request handling
- JSON response formatting
- Error response codes (400, 500, 503)

**Supported Endpoints**:
- `GET /` - Main conversion endpoint
  - Parameters: `url`, `format`
  - Returns: JSON response with status and message

### 3. Validation Layer (`YT2MP3.h`)

**Responsibility**: Input validation and sanitization

**Core Functions**:

#### `isValidURL(const std::string& url)`
```cpp
// Purpose: Validate YouTube URL format
// Input: URL string
// Output: true if valid YouTube URL, false otherwise
// Implementation: Regex pattern matching for YouTube URL formats
//   - https://www.youtube.com/watch?v=VIDEO_ID
//   - https://youtu.be/VIDEO_ID
//   - https://www.youtube.com/watch?v=VIDEO_ID&...
```

#### `isValidFormat(const std::string& format)`
```cpp
// Purpose: Validate output format
// Input: Format string (mp3, mp4, wav)
// Output: true if valid, false otherwise
// Implementation: Case-insensitive comparison
```

#### `extractVideoID(const std::string& url)`
```cpp
// Purpose: Extract video ID from YouTube URL
// Input: Valid YouTube URL
// Output: Video ID string
// Implementation: Regex extraction and parsing
// Example: https://www.youtube.com/watch?v=abc123 → "abc123"
```

### 4. Processing Core

**Responsibility**: Orchestration of download and conversion

**Key Functions**:

#### `downloadVideo(const std::string& url, const std::string& videoID)`
```cpp
// Purpose: Download video using yt-dlp
// Process:
//   1. Execute yt-dlp command with best quality flag
//   2. Handle download errors
//   3. Store video temporarily
// Quality: Best available (usually 720p or 1080p video + 192kbps audio)
```

#### `convertVideo(const std::string& format, const std::string& videoID)`
```cpp
// Purpose: Convert video to requested format using FFmpeg
// Formats:
//   - MP3: AAC codec, 192kbps, 48kHz, stereo
//   - MP4: Original video codec and settings
//   - WAV: PCM codec, lossless, 48kHz, stereo
// Output: File saved to format-specific directory
```

#### `processVideo(const std::string& url, const std::string& format)`
```cpp
// Purpose: Complete processing pipeline
// Steps:
//   1. Validate URL and format
//   2. Extract video ID
//   3. Download video
//   4. Convert to specified format
//   5. Return status to caller
```

## Data Flow

### Typical Conversion Flow

```
┌─────────────────────────────────────────────────────────────────┐
│ 1. INPUT: User provides YouTube URL and desired format          │
│    Example: "https://www.youtube.com/watch?v=dQw4w9WgXcQ"  mp3  │
└─────────────────────────────────────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────────┐
│ 2. VALIDATION:                                                  │
│    - isValidURL() checks URL format                             │
│    - isValidFormat() checks output format                       │
└─────────────────────────────────────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────────┐
│ 3. EXTRACTION:                                                  │
│    - extractVideoID() isolates video ID                         │
│    - Result: "dQw4w9WgXcQ"                                      │
└─────────────────────────────────────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────────┐
│ 4. DOWNLOAD:                                                    │
│    - Execute: yt-dlp -f best -o "temp_dQw4w9WgXcQ" [URL]       │
│    - Downloads best quality video to temp directory            │
│    - File saved as temporary file                              │
└─────────────────────────────────────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────────┐
│ 5. CONVERSION:                                                  │
│    Based on format:                                            │
│    MP3: ffmpeg -i temp_dQw4w9WgXcQ -q:a 5 -acodec libmp3lame   │
│         output_dQw4w9WgXcQ.mp3                                 │
│    MP4: ffmpeg -i temp_dQw4w9WgXcQ -c copy                     │
│         output_dQw4w9WgXcQ.mp4                                 │
│    WAV: ffmpeg -i temp_dQw4w9WgXcQ -c:a pcm_s16le              │
│         output_dQw4w9WgXcQ.wav                                 │
└─────────────────────────────────────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────────┐
│ 6. OUTPUT:                                                      │
│    - Move converted file to appropriate directory:             │
│      MP3/ , MP4/ , or WAV/                                     │
│    - Delete temporary files                                   │
│    - Return success status to user                             │
└─────────────────────────────────────────────────────────────────┘
```

### Error Handling Flow

```
Process Error
    │
    ├─ Invalid URL
    │   └─ Return error: "Invalid YouTube URL format"
    │
    ├─ Invalid Format
    │   └─ Return error: "Format must be mp3, mp4, or wav"
    │
    ├─ Download Failed
    │   ├─ Check network connection
    │   ├─ Check video accessibility (private, age-restricted)
    │   └─ Return error: "Failed to download video"
    │
    ├─ Conversion Failed
    │   ├─ Check FFmpeg installation
    │   ├─ Check disk space
    │   └─ Return error: "Failed to convert video"
    │
    └─ Storage Error
        ├─ Check directory permissions
        ├─ Check available disk space
        └─ Return error: "Failed to save output file"
```

## Key Design Decisions

### 1. External Tool Integration

**Decision**: Use `yt-dlp` for downloading and `ffmpeg` for conversion

**Rationale**:
- Robust, battle-tested solutions for their respective domains
- Reduces maintenance burden and security vulnerabilities
- Focuses YT-Converter on orchestration and API exposure
- Easier to update underlying tools independently

**Trade-offs**:
- External dependencies required for installation
- Relies on third-party tool stability
- Limited control over specific implementation details

### 2. Separation of CLI and API

**Decision**: Maintain separate entry points for CLI and API

**Rationale**:
- Allows independent evolution of interfaces
- CLI users get optimized command-line experience
- API users get proper HTTP semantics and JSON responses
- Shared core processing logic reduces duplication

**Architecture**:
```
Core Processing Functions (processVideo, etc.)
    ├─ CLI Interface (YT2MP3.cpp)
    └─ API Interface (yt2mp3-API.cpp)
```

### 3. Format-Specific Output Directories

**Decision**: Organize output files into format-specific directories (MP3/, MP4/, WAV/)

**Rationale**:
- Improves file organization and discovery
- Prevents filename collisions
- Allows users to manage formats independently
- Clear separation of concerns

### 4. Synchronous Processing

**Decision**: Sequential download → convert → save workflow

**Rationale**:
- Simpler implementation and error handling
- Predictable resource usage
- Clear cause-and-effect for debugging
- Meets current performance requirements

**Future Consideration**: Could be parallelized for multiple concurrent requests in API

### 5. Regex-Based URL Validation

**Decision**: Use regex patterns for URL validation

**Rationale**:
- Fast and efficient validation
- Handles multiple YouTube URL formats
- Prevents invalid URLs from reaching external tools
- Clear validation rules

**Supported Formats**:
```
https://www.youtube.com/watch?v=VIDEO_ID
https://youtu.be/VIDEO_ID
https://www.youtube.com/watch?v=VIDEO_ID&list=PLAYLIST_ID
```

## Technology Stack

### Core Technologies

| Component | Technology | Version | Purpose |
|-----------|-----------|---------|---------|
| Language | C++ | 17+ | High-performance core implementation |
| Build System | CMake | 3.10+ | Cross-platform build configuration |
| Download | yt-dlp | Latest | YouTube video downloading |
| Conversion | FFmpeg | 4.0+ | Audio/video processing |
| REST API | cpprestsdk | 2.10+ | HTTP server and JSON handling |
| Security | OpenSSL | 1.1+ | HTTPS and encryption |
| Libraries | Boost | 1.70+ | Additional C++ utilities |

### External Dependencies

**Runtime Dependencies**:
- yt-dlp: Video downloading
- FFmpeg: Audio/video encoding
- libssl: HTTPS support

**Build Dependencies**:
- C++17 compatible compiler (GCC 7+, Clang 5+, MSVC 2017+)
- CMake 3.10+
- Boost development libraries
- cpprestsdk development libraries

## Performance Considerations

### Resource Usage

**Memory**:
- CLI: ~20-50 MB (minimal buffer)
- API Server: ~100 MB base + per-request overhead
- Conversion Process: ~200-500 MB (depends on video quality)

**CPU**:
- Download: I/O bound (network limited)
- Conversion: CPU intensive
  - MP3: Medium CPU usage
  - MP4 (copy codec): Minimal CPU usage
  - WAV: Medium CPU usage

**Disk**:
- Temporary storage required for downloaded video
- Space requirements scale with video length and quality
- Output file size varies by format:
  - MP3: ~1 MB per minute (192kbps)
  - MP4: Varies (typically 1-2 MB per minute)
  - WAV: ~10 MB per minute (lossless)

### Optimization Opportunities

1. **Streaming Conversion**: Convert while downloading instead of waiting
2. **Format-Specific Profiles**: Allow users to customize quality/bitrate
3. **Caching**: Cache video metadata to avoid re-downloading
4. **Parallel Processing**: Handle multiple concurrent requests in API
5. **Progress Reporting**: Report download/conversion progress to clients

## Security Considerations

### Input Validation

- **URL Validation**: Prevents injection attacks and malformed requests
- **Format Validation**: Prevents arbitrary command execution
- **Video ID Extraction**: Isolates and validates YouTube identifiers

### Execution Safety

- **Process Isolation**: External tools run as separate processes
- **Error Handling**: Graceful handling of external tool failures
- **Resource Limits**: Prevent runaway processes (future improvement)

### Data Protection

- **Temporary Files**: Cleaned up after processing (future improvement)
- **User Privacy**: No logging of URLs or video metadata (by default)
- **HTTPS Support**: API server supports encrypted communication

### Known Limitations

- **Age-Restricted Videos**: Cannot download (yt-dlp limitation)
- **Private Videos**: Cannot download (yt-dlp limitation)
- **DRM Content**: Cannot download (yt-dlp limitation)
- **Copyright**: Users responsible for respecting copyright laws

## Future Improvements

### Short Term

1. **Progress Reporting**: Stream download/conversion progress to clients
2. **Request Timeouts**: Implement configurable timeout for long operations
3. **Retry Logic**: Automatic retry for failed downloads
4. **Quality Settings**: Allow users to specify download quality and audio bitrate

### Medium Term

1. **Database Integration**: Track conversion history and statistics
2. **Authentication**: Optional API authentication and rate limiting
3. **Batch Processing**: Support bulk conversions
4. **Status Endpoint**: Query conversion status for asynchronous processing
5. **Webhook Support**: Notify clients when conversion completes

### Long Term

1. **Distributed Architecture**: Scale to multiple worker nodes
2. **Container Support**: Docker/Kubernetes deployment
3. **Queue System**: Redis or RabbitMQ for request queuing
4. **Web UI**: Browser-based interface for conversions
5. **Advanced Formats**: Support additional audio formats (FLAC, OGG, etc.)
6. **Playlist Support**: Bulk download and convert entire playlists

---

**For questions or clarifications about the architecture, please refer to [CONTRIBUTING.md](../CONTRIBUTING.md) for contact information.**
