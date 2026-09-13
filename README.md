# YT-Converter

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![CMake](https://img.shields.io/badge/CMake-3.16%2B-064F8C.svg)](https://cmake.org/)
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20macOS-lightgrey.svg)](#)

Local C++17 CLI and optional localhost REST API that convert a single YouTube video to `mp3`, `mp4`, or `wav` by running `yt-dlp` and `ffmpeg` as separate processes (no shell). There is no web UI, database, or public service.

**This is a personal, loopback tool.** You are responsible for YouTube Terms of Service and copyright. Public Internet deployment is out of scope until you have legal review, authentication, quotas, and isolation that this repository does not provide.

## Features

- CLI (`yt2mp3-cli`) and localhost API (`yt2mp3-api`)
- Strict YouTube URL parsing (`watch`, `youtu.be`, `shorts`, `embed`, `live`)
- Video IDs used in paths must match `^[A-Za-z0-9_-]{11}$`
- yt-dlp is always invoked with a reconstructed `watch?v=` URL and `--no-playlist`
- Temporary files live under a configured output root and are deleted after success or failure
- Environment-based configuration; remote bind requires an API key
- Unit tests with fake `yt-dlp` / `ffmpeg` binaries (no network)

## Prerequisites

| Requirement | Version | Notes |
|---|---|---|
| C++ compiler | C++17 | GCC, Clang |
| CMake | 3.16+ | 3.30+ policy CMP0167 is guarded |
| yt-dlp | latest | Required at runtime |
| ffmpeg | 4+ | Required at runtime |
| cpprestsdk | 2.10+ | Required only to build the API |
| Boost, OpenSSL | as needed by cpprestsdk | API only |
| GoogleTest | 1.15+ | Fetched automatically when `-DBUILD_TESTS=ON` |

### Linux (Debian/Ubuntu)

```sh
sudo apt-get install build-essential cmake libboost-all-dev libcpprest-dev libssl-dev ffmpeg
pip install yt-dlp
```

### macOS

```sh
brew install cmake boost cpprestsdk openssl yt-dlp ffmpeg
```

Windows is not tested. The process layer has a CreateProcess path, but CI only runs on Linux and macOS.

## Build

```sh
git clone https://github.com/NovrusShehaj/YT-Converter.git
cd YT-Converter
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Binaries land in `build/yt2mp3-cli` and, if cpprestsdk is present, `build/yt2mp3-api`.

```sh
./build/yt2mp3-cli --help
./build/yt2mp3-cli --version
```

## CLI

```sh
./build/yt2mp3-cli [options] <URL> [format]
```

Examples:

```sh
./build/yt2mp3-cli "https://www.youtube.com/watch?v=dQw4w9WgXcQ" mp3
./build/yt2mp3-cli --output-dir ./output --format wav "https://youtu.be/dQw4w9WgXcQ"
./build/yt2mp3-cli --force --no-unicode "https://www.youtube.com/shorts/dQw4w9WgXcQ" mp4
```

| Flag | Purpose |
|---|---|
| `--help`, `--version` | Usage and `1.0.0` |
| `--output-dir DIR` | Finished files go here (default `./output`) |
| `--format FMT` | `mp3`, `mp4`, or `wav` |
| `--quality N` | Max mp4 height (default 1080) |
| `--force` | Replace an existing `<id>.<fmt>` |
| `--no-unicode` | ASCII status markers |
| `-v` / `-q` | Debug or errors-only logging |

Exit codes: `0` success, `2` validation, `3` missing tools, `4` download, `5` convert, `130` canceled.

Finished files are `<output-dir>/<id>.<fmt>`. The absolute path is printed on success.

### Conversion settings

| Format | Tooling | Notes |
|---|---|---|
| MP3 | `bestaudio` then ffmpeg `libmp3lame` | 44100 Hz, stereo, 192 kbps |
| WAV | `bestaudio` then ffmpeg `pcm_s16le` | 44100 Hz, stereo |
| MP4 | video+audio merge, max height 1080 | ffmpeg `-c copy` |

ffmpeg always gets `-y -nostdin -hide_banner -loglevel error`.

## Localhost API

The API binds `127.0.0.1:8080` by default. It will not start without `YTCONV_API_KEY` or `--allow-unauthenticated-localhost`. It refuses `0.0.0.0` / `::` unless `YTCONV_ALLOW_REMOTE=1` **and** an API key is set.

```sh
YTCONV_ALLOW_UNAUTHENTICATED_LOCALHOST=1 ./build/yt2mp3-api
```

```sh
curl -sS -X POST http://127.0.0.1:8080/v1/conversions \
  -H 'Content-Type: application/json' \
  -d '{"url":"https://www.youtube.com/watch?v=dQw4w9WgXcQ","format":"mp3"}'
```

Success:

```json
{
  "status": "success",
  "error_code": "ok",
  "message": "Conversion completed",
  "output_path": "/abs/path/output/dQw4w9WgXcQ.mp3",
  "job_id": "dQw4w9WgXcQ-1a2b3c4d"
}
```

The response is JSON with an absolute `output_path`. Bytes are not streamed. There is no unauthenticated static file server.

| Method | Path | Result |
|---|---|---|
| POST | `/v1/conversions` | Convert (JSON `url`, `format`) |
| GET | `/v1/conversions` | 405 |
| GET | `/v1/healthz` | Process up |
| GET | `/v1/readyz` | Tools + writable output |
| GET | `/v1/metrics` | Loopback only |
| GET | `/` | 404 |

If `YTCONV_API_KEY` is set, send `X-Api-Key`. HTTP codes actually produced: 200, 400, 401, 404, 405, 500, 503, 504, 507.

See [docs/API.md](docs/API.md).

## Configuration

Environment variables (see `.env.example`; `.env` files are not loaded automatically):

| Variable | Default | Purpose |
|---|---|---|
| `YTCONV_BIND` | `127.0.0.1` | Listen address |
| `YTCONV_PORT` | `8080` | Listen port |
| `YTCONV_LOG_LEVEL` | `INFO` | `DEBUG`, `INFO`, `WARNING`, `ERROR`, `CRITICAL` |
| `YTCONV_LOG_FORMAT` | `text` | `text` or `json` |
| `YTCONV_OUTPUT_DIR` | `./output` | Media root |
| `YTCONV_MAX_CONCURRENT` | `1` | API in-flight jobs |
| `YTCONV_CHILD_TIMEOUT_SEC` | `900` | Kill hung yt-dlp/ffmpeg |
| `YTCONV_YT_DLP` / `YTCONV_FFMPEG` | `yt-dlp` / `ffmpeg` | Binary paths (tests use fakes) |
| `YTCONV_API_KEY` | empty | Required for remote bind |
| `YTCONV_ALLOW_REMOTE` | unset | Must be `1` to bind all interfaces |
| `YTCONV_LOG_URLS` | unset | Log reconstructed URLs at DEBUG only |

INFO logs use `request_id` and `video_id`, not the user URL.

## Project structure

```
YT-Converter/
├── CMakeLists.txt
├── src/cli/main.cpp
├── src/api/server.cpp
├── src/api/api_app.cpp
├── src/core/converter.cpp
├── src/utils/          # validation, process, logger, config, errors
├── include/
├── tests/              # GoogleTest + fake binaries
├── docs/
└── .github/workflows/
```

## Tests

```sh
cmake -S . -B build -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Tests do not call YouTube. `tests/fakes/` records argv and writes tiny fixtures. See [docs/TESTING.md](docs/TESTING.md).

`./quick-test.sh` configures, builds, and runs `ctest`.

## Legal

Downloading YouTube media may violate YouTube's Terms of Service. This project ships as a local converter for content you are allowed to copy. Do not run it as an open converter on the public Internet.

## License

MIT. See [LICENSE](LICENSE).
