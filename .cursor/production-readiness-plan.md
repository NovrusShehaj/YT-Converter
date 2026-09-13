# YT-Converter Production-Readiness Plan

## 1. Executive Summary and Scope Confirmation

### Repository identity

| Field | Verified value |
|---|---|
| Workspace | `/home/ghost/Github/YT-Converter` |
| Git top-level | `/home/ghost/Github/YT-Converter` |
| Remote | `git@github.com:NovrusShehaj/YT-Converter.git` |
| Current branch | `dev/production-readiness` (`.git/HEAD` → `refs/heads/dev/production-readiness`) |
| HEAD commit | `7a52cb6b1dde1c909861a2a70373f4672c776cc3` |
| `main` / `origin/main` | Same commit `7a52cb6b` |
| Unique commits on this branch | None. Branch was created by checkout from `main` after clone. |
| Tracked application source | C++17 CLI + REST API that shells out to `yt-dlp` and `ffmpeg` |
| Uncommitted working-tree changes | Only untracked `.cursor/` (audit prompt). No application, test, CI, or config diffs. |
| Secrets inspected | No `.env`, key, token, or credential files were opened. `.gitignore` ignores `.env*`. Source/CI/docs contain no committed secrets. |

This plan analyzes the current tree on `dev/production-readiness` at `7a52cb6`. Documentation was treated as claims and checked against executed code paths. Docs and CMake comments frequently describe a different, older layout (`YT2MP3.cpp`, `handlers.cpp`, `tests/`, `MP3/` directories) that is not present.

### What this software is

YT-Converter is a small local C++ tool with two binaries:

- `yt2mp3-cli` — convert one YouTube URL to `mp3`, `mp4`, or `wav`
- `yt2mp3-api` — HTTP listener on `http://localhost:8080` that runs the same conversion synchronously and returns JSON

There is no web frontend, no database, no auth, no queue, no container, no config file, and no automated test suite. Production-readiness here means: safe to run as a maintained local tool, and safe enough that the REST API could later be operated without remote code execution, disk fill, or silent data loss.

### Verdict in one sentence

**Do not deploy this to production.** The conversion pipeline concatenates attacker-controlled URL and video-ID strings into `system()` shell commands, URL validation is an unanchored substring search, temporary files are never cleaned up, the API is a blocking GET with unbounded concurrency, and CMake advertises tests that do not exist.

### Evidence vs inference

- **Inspected evidence:** source, headers, CMake, workflows, docs, `.gitignore`, git refs/logs, leftover `build/CMakeCache.txt`.
- **Not executed (Ask mode / no process spawn):** binaries, `yt-dlp`, `ffmpeg`, CMake configure, CI. Behavioral claims about those tools use documented defaults plus how this repo invokes them.
- **Leftover build cache:** `build/CMakeCache.txt` was generated for `/home/ghost/Programming/YT-Converter`, not this path. It is stale and must not be treated as a successful build of this workspace.

---

## 2. System Understanding and Current-State Architecture

### User-facing functionality

**CLI (`src/cli/main.cpp`)**

- Requires exactly two arguments: URL and format.
- Calls `yt::converter::processVideo`.
- On success prints a banner and `output_<id>.<format>`.
- On `invalid_argument` / `runtime_error` / other exceptions, prints an error and exits `1`.
- There is no `--help`, `--version`, output-directory flag, quality flag, or progress reporter. `argc != 3` prints usage (so `yt2mp3-cli --help` is treated as “wrong number of arguments”).

**API (`src/api/server.cpp`)**

- Binds `http://localhost:8080`.
- Registers one GET handler for **every path**.
- Reads query params `url` and `format`.
- Runs `processVideo` on the request thread.
- Returns JSON `{status, output_file, message}` or `{status, error}`.
- SIGINT sets `shouldShutdown`; SIGTERM is ignored.
- No health, ready, status, download, cancel, or auth endpoints.

**Documented but not implemented**

- Playlist/channel conversion
- Format directories `MP3/`, `MP4/`, `WAV/`
- HTTP 503 when overloaded
- Root-path contract in README vs `/convert` in the server banner
- `youtu.be` URLs (error text mentions them; `isValidURL` does not accept them)
- `--help` / `--version`
- HTTPS, authentication, rate limits, retries, timeouts, progress
- Tests, `docs/API.md`, `handlers.cpp`, legacy `YT2MP3.cpp`

### Architecture (as the code actually is)

```
yt2mp3-cli (src/cli/main.cpp)
yt2mp3-api (src/api/server.cpp)
        \         /
         \       /
    yt-converter-core (STATIC)
      converter.cpp  →  validation.cpp  →  logger.cpp
              |
              +-- system("yt-dlp ...")
              +-- system("ffmpeg ...")
              |
         CWD files:
           temp_<videoID>.mp4
           output_<videoID>.<format>
```

Boundaries are shallow but real: CLI and API share the core library. There is no process sandbox, job layer, storage layer, or config layer.

### Libraries and frameworks

| Piece | Role in this repo | Actual usage |
|---|---|---|
| C++17 | Language | All sources |
| CMake 3.16+ (docs say 3.10+) | Build | `CMakeLists.txt`; policy `CMP0167 NEW` is a CMake 3.30 policy |
| C++ REST SDK (`cpprestsdk`) | HTTP + JSON | API only |
| Boost | Declared dependency | Linked everywhere; no Boost headers in project `.cpp`/`.h` |
| OpenSSL | Declared “HTTPS” | Linked; no TLS listener; leftover of cpprestsdk |
| GoogleTest | Optional tests | `BUILD_TESTS=OFF`; `tests/` does not exist |
| `yt-dlp` | Download | Invoked via `/bin/sh -c` through `system()` |
| `ffmpeg` | Transcode | Same |

### External services

- YouTube (and whatever extractors `yt-dlp` follows) over the network.
- No first-party YouTube Data API key, OAuth, cookies, or proxy configuration.

### Data / media flow (traced)

1. **Input.** CLI: `argv[1]`, `argv[2]`. API: query `url`, `format` (cpprest `uri::split_query`).
2. **Validate.** `yt::validation::validateConverterInput` → `isValidURL` + `isValidFormat`.
   - URL regex: `https?://(www\.)?youtube\.com/watch\?v=[a-zA-Z0-9_-]+` via `std::regex_search` (substring, not full-string).
   - Format: lowercased; must be `mp3` / `mp4` / `wav`.
3. **Extract ID.** `extractVideoID` finds the first `v=` and takes everything until `&` or EOS. It does **not** reuse the regex capture.
4. **Download.** `downloadVideo` builds:

   `yt-dlp -f 'bestvideo[ext=mp4]+bestaudio[ext=m4a]/mp4' -o 'temp_<videoID>.mp4' '<url>'`

   then `system()`. On non-zero wait status, throws `std::runtime_error`.
5. **Convert.** `convertVideo` runs unquoted `ffmpeg -i temp_<id>.mp4 ... output_<id>.<fmt>`:
   - mp3: `-vn -ar 44100 -ac 2 -b:a 192k -codec:a libmp3lame`
   - mp4: `-c copy`
   - wav: no codec/sample-rate flags
6. **Return filename.** `output_<videoID>.<format>` in the process CWD. Temp file is left on disk. No move into `MP3/`/`MP4/`/`WAV/`.

### State, persistence, auth

- **State:** in-process only. API has `std::atomic<bool> shouldShutdown`. Logger is a process-wide singleton.
- **Persistence:** files in CWD. No database, cache, or job history.
- **Auth/authz:** none. Any local client that can open `localhost:8080` can convert.

### Build, test, deploy, CI

- **Build:** `mkdir build && cd build && cmake .. && make` produces `yt2mp3-cli` and `yt2mp3-api` in the build dir (no `bin/` prefix). Default `Release` + `-O3` (non-MSVC). MSVC uses `/W4 /WX`.
- **Tests:** CMake option `BUILD_TESTS` expects `tests/test_validator.cpp` and `tests/test_converter.cpp`. Directory is absent. CI passes `-DBUILD_TESTS=OFF`.
- **Manual QA:** `quick-test.sh` configures, builds, and greps CLI validation errors. `docs/TESTING.md` is a machine-specific checklist (`/home/ghost/Programming/YT-Converter`) and tests the wrong API path (`/?url=` vs documented `/convert`).
- **CI:** `.github/workflows/build.yml` and `code-quality.yml` run on `main`/`develop` only — **not** on `dev/production-readiness`. Matrix: Ubuntu + macOS, Debug + Release. No Windows. No conversion smoke test. `actions/upload-artifact@v3` and `actions/cache@v3` are used.
- **Deploy:** no Dockerfile, compose, systemd unit, package, or env template. `install()` copies binaries to `bin` and headers to `include/yt-converter`; only a version file is installed for CMake packages (no `Config.cmake`).
- **Config/secrets:** no environment variables read in source. Port, bind, log level, and output root are hardcoded. API log level is `DEBUG`.

### Entry points and operational dependencies

| Entry | Symbol | Runtime needs |
|---|---|---|
| CLI | `src/cli/main.cpp` `main` | `yt-dlp`, `ffmpeg` on `PATH`; write access to CWD |
| API | `src/api/server.cpp` `main` | Same + free TCP 8080 + cpprestsdk/OpenSSL/Boost at link time |
| Core | `yt::converter::processVideo` | Same binaries; no preflight `which` |

### UI surface (there is no GUI)

UX findings below apply to CLI stdout/stderr and the JSON API. There are no HTML/CSS/JS assets, no browser app, and no accessibility tree to audit.

---

## 3. Detailed Findings

### F01 — Shell command injection through `system()`  
**Priority:** P0 (Release Blocker)  
**Type:** security  

**Problem.** Download and convert commands are built by string concatenation and executed with `system()`, which invokes a shell. URL validation is an unanchored substring match, and `extractVideoID` copies raw text after `v=`. A single quote in the URL or video ID breaks out of the quoted `yt-dlp` arguments; ffmpeg arguments are unquoted.

**Evidence.**

```80:96:src/core/converter.cpp
void downloadVideo(const std::string& url, const std::string& videoID) {
    // ...
    std::string downloadCommand = "yt-dlp -f 'bestvideo[ext=mp4]+bestaudio[ext=m4a]/mp4' "
                                  "-o 'temp_" + videoID + ".mp4' '" + url + "'";
    int exitCode = system(downloadCommand.c_str());
```

```109:129:src/core/converter.cpp
        convertCommand = "ffmpeg -i " + tempFile + " -vn -ar 44100 -ac 2 -b:a 192k "
                        "-codec:a libmp3lame " + outputFile;
    // ...
    int exitCode = system(convertCommand.c_str());
```

```15:19:src/utils/validation.cpp
    std::regex youtube_url_pattern(
        R"(https?://(www\.)?youtube\.com/watch\?v=[a-zA-Z0-9_-]+)"
    );
    return std::regex_search(url, youtube_url_pattern);
```

```59:73:src/core/converter.cpp
    size_t found = url.find("v=");
    if (found != std::string::npos) {
        std::string videoID = url.substr(found + 2);
        size_t ampPos = videoID.find('&');
        if (ampPos != std::string::npos) {
            videoID = videoID.substr(0, ampPos);
        }
```

Example that `isValidURL` accepts:  
`https://www.youtube.com/watch?v=dQw4w9WgXcQ'; touch /tmp/pwned; echo '`  
The regex matches the legal prefix; the remainder is copied into the shell.

**Impact.** Remote or local callers can run arbitrary shell commands as the converter user. The API uses GET, so a webpage can trigger this against `localhost:8080` (CSRF against a local service). This is a release blocker for any networked use and for any CLI that accepts untrusted URLs.

**Recommendation.** Never use a shell. Spawn `yt-dlp` and `ffmpeg` with an argv array (`posix_spawn` / `CreateProcess` / a small RAII wrapper). Validate the video ID as `^[A-Za-z0-9_-]{11}$` (or the exact capture from a fully anchored URL regex). Reject any URL that does not match in full.

---

### F02 — Path traversal via unsanitized video ID in filenames  
**Priority:** P0 (Release Blocker)  
**Type:** security  

**Problem.** After a substring URL match, `extractVideoID` keeps slashes and `..`. Filenames become `temp_<id>.mp4` and `output_<id>.<fmt>` with no sanitization.

**Evidence.** `extractVideoID` (`converter.cpp` ~55–78) + `getOutputFilename` (~140–143) + ffmpeg path concatenation (~104–111).  
URL `https://www.youtube.com/watch?v=abc/../../../tmp/x` matches `v=abc`; extracted ID is `abc/../../../tmp/x`; output path is `temp_abc/../../../tmp/x.mp4`.

**Impact.** Write/read outside the working directory. Combined with F01 this is write-anywhere plus command execution.

**Recommendation.** Use only a validated 11-character ID in filenames. Resolve output under a configured root and reject paths that escape it (`std::filesystem::weakly_canonical` vs root).

---

### F03 — Unanchored URL validation is a bypass, not a validator  
**Priority:** P0 (Release Blocker)  
**Type:** bug, security  

**Problem.** `std::regex_search` succeeds if a YouTube watch URL appears *anywhere* in the string. `downloadVideo` then passes the **original** URL to `yt-dlp`.

**Evidence.** `yt::validation::isValidURL` (`validation.cpp` 8–19). `downloadVideo` uses `url`, not a reconstructed `https://www.youtube.com/watch?v=<id>` (`converter.cpp` 80–86).

Accepted examples:

- `https://evil.example/?q=https://www.youtube.com/watch?v=dQw4w9WgXcQ` — `yt-dlp` fetches the attacker host (SSRF / arbitrary extractor).
- `https://www.youtube.com/watch?v=dQw4w9WgXcQ&list=PLxxxx` — passes; `yt-dlp` default is to follow playlists unless `--no-playlist`.

`getURLValidationError` mentions `youtu.be`, but `isValidURL` does not accept it (`validation.cpp` 39–54 vs 15–17). Docs claim `youtu.be` support (`README.md` architecture table; `docs/ARCHITECTURE.md` ~341–345).

**Impact.** Validation does not constrain the download target. Playlists can fill the disk. Common share URLs fail. Error messages lie about why.

**Recommendation.** Full-match regex (or a URL parser) for a tight allowlist. Reconstruct the yt-dlp URL from the captured ID. Pass `--no-playlist`. Add `youtu.be`, `/shorts/`, `/embed/`, `/live/`, `m.youtube.com`, `music.youtube.com` only as explicit, tested variants.

---

### F04 — State-changing GET with no auth is CSRF- and abuse-friendly  
**Priority:** P0 if the API is reachable beyond a trusted tty; otherwise P1  
**Type:** security  

**Problem.** Conversion is a GET with side effects. The handler is registered for all paths. There is no origin check, CSRF token, API key, or method restriction.

**Evidence.** `listener.support(methods::GET, handleRequest)` (`server.cpp` 188–189). `handleRequest` never inspects the path (54–116). README documents GET (`README.md` 218–223).

**Impact.** Any origin that can cause the browser to request `http://localhost:8080/...` can start downloads, fill disks, and (with F01) execute commands. Binding localhost reduces internet exposure; it does not stop browser CSRF or other local processes.

**Recommendation.** `POST /v1/conversions` only. Reject GET for conversion. Optional shared-secret header for local API use. Path allowlist. Do not bind `0.0.0.0` until auth, rate limits, and job isolation exist.

---

### F05 — Temporary files never deleted; failures leave partial media  
**Priority:** P1 (High Priority)  
**Type:** reliability, bug  

**Problem.** `processVideo` downloads, converts, and returns. No `unlink` on success or in the `catch` that rethrows. Docs claim cleanup is a “future improvement” (`docs/ARCHITECTURE.md` 421–422) while also describing delete-on-success in the data-flow diagram (242).

**Evidence.** `processVideo` (`converter.cpp` 10–53): try/catch logs and `throw;` with no filesystem cleanup. No `std::filesystem` usage in the repo.

**Impact.** Every success leaves `temp_<id>.mp4` (often larger than the output). Failures leave truncated files. CWD/`build/` grows without bound. ffmpeg without `-y` then fails on reruns (F07).

**Recommendation.** RAII temp file (delete in destructor unless released). Delete temp after successful convert. On failure, delete temp and incomplete output. Put media under a dedicated directory, not CWD.

---

### F06 — No concurrency control; same video ID races  
**Priority:** P1 (High Priority)  
**Type:** reliability, bug  

**Problem.** The API runs `processVideo` inline. cpprestsdk handles requests on a thread pool. Two requests for the same ID share `temp_<id>.mp4` and `output_<id>.mp3`. Logger is documented as thread-safe (`include/logger.h` 29–30) but has no mutex (`src/utils/logger.cpp` 68–86). `std::localtime` is not thread-safe.

**Evidence.** `handleRequest` → `processVideo` (`server.cpp` 100–102). No semaphore, queue, or per-ID lock. Logger members are unsynchronized.

**Impact.** Corrupt media, interleaved logs, data races (undefined behavior), and N parallel 4K downloads with no cap.

**Recommendation.** Global concurrency limit (start at 1 or 2). Per-video-ID lock or unique job IDs in filenames. Mutex (or channel) in `Logger`. Later: a worker queue and async job API.

---

### F07 — ffmpeg invoked without `-y`, `-nostdin`, or timeouts  
**Priority:** P1 (High Priority)  
**Type:** reliability, bug  

**Problem.** Reconversion of the same ID can block on overwrite prompt. stdin can stall ffmpeg. `system()` has no timeout. yt-dlp has no `--socket-timeout` / `--max-filesize`.

**Evidence.** ffmpeg strings in `convertVideo` (`converter.cpp` 109–120). No `-y`, `-nostdin`, `-hide_banner`, `-loglevel`. `system()` at 90 and 129.

**Impact.** Hung API workers, hung CLI, and no recovery short of kill. Long live streams or huge videos run until disk or memory dies.

**Recommendation.** Always `-y -nostdin`. Kill the child after a configurable timeout. Pass yt-dlp `--no-playlist --socket-timeout --max-filesize --retries`. Prefer audio-only extraction for mp3/wav.

---

### F08 — Playlist, channel, and “best video” downloads can exhaust disk  
**Priority:** P1 (High Priority)  
**Type:** reliability, functionality  

**Problem.** The original URL is given to yt-dlp. `watch?v=&list=` is valid. Format is best MP4 video + best M4A, which can be 4K. No duration/size cap, no free-space check.

**Evidence.** `downloadVideo` format string (`converter.cpp` 85–86). Validation allows extra query text because of `regex_search`.

**Impact.** A single API GET can write tens of GB. WAV is uncompressed (~10 MB/min per the project’s own docs).

**Recommendation.** `--no-playlist`. Reconstruct watch URL from ID. For audio formats use `bestaudio` / `-x`. Enforce max duration/size. Check free space before download.

---

### F09 — `system()` wait status is treated as an exit code  
**Priority:** P2 (Medium Priority)  
**Type:** bug  

**Problem.** On POSIX, `system()` returns a wait status. Exit `1` often appears as `256`. Users and logs get a wrong code.

**Evidence.** `converter.cpp` 90–94 and 129–134.

**Recommendation.** Decode with `WIFEXITED` / `WEXITSTATUS` (and spawn APIs that return the real code). Map missing-binary vs yt-dlp vs ffmpeg failures to distinct errors.

---

### F10 — Common YouTube URL variants are rejected or mishandled  
**Priority:** P1 (High Priority)  
**Type:** bug, functionality, UX/design  

**Problem.** Only `http(s)://(www.)youtube.com/watch?v=...` substring-matches. Share links that real users send do not work, despite docs.

| Variant | Docs | Code |
|---|---|---|
| `youtube.com/watch?v=` | Yes | Partial (substring) |
| `youtu.be/ID` | Yes (`ARCHITECTURE.md`, `getURLValidationError`) | Rejected |
| `/shorts/`, `/embed/`, `/live/` | No | Rejected |
| `m.youtube.com`, `music.youtube.com` | No | Rejected |
| `watch?v=&list=` | “supported” as URL form | May download a playlist |
| Channel / playlist-only URLs | Future work | Rejected (good) until designed |

`extractVideoID` cannot parse `youtu.be/ID` even if validation were relaxed.

**Impact.** First-run failures for the most common mobile share URL.

**Recommendation.** One parser that returns a normalized `VideoRef {id, kind}`. Reject playlist-only and channel URLs with a clear message until batch work exists.

---

### F11 — Output contract does not match docs (paths, codecs, API)  
**Priority:** P1 (High Priority)  
**Type:** bug, documentation, UX/design  

**Problem.**

- Files are `./output_<id>.<fmt>`, not `MP3/` / `MP4/` / `WAV/` (`getOutputFilename`, `converter.cpp` 140–143). README 195–202 and TESTING.md 182–185 say otherwise.
- MP3 is 44100 Hz (`converter.cpp` 110–111), not 48000 (`README.md` 344–348). Architecture says MP3 uses AAC (`ARCHITECTURE.md` 176) — wrong codec.
- WAV has no explicit PCM flags (`converter.cpp` 118–120) vs documented `pcm_s16le` 48 kHz.
- API success JSON is `{status:"success", output_file, message}` (`server.cpp` 107–114), not `{status:200, message:"The audio file has been saved as..."}` (`README.md` 254–259).
- README examples hit `/`; the server banner documents `/convert`. Both currently work only because every GET is handled.
- HTTP 503 is documented and never returned.

**Impact.** Scripts and operators follow README and fail. Quality claims are false.

**Recommendation.** Pick one contract, implement it, and make README/ARCHITECTURE/TESTING/CLI banner identical. Prefer: `POST /v1/conversions`, typed JSON, files under `<output-root>/<id>.<fmt>`.

---

### F12 — API returns a local filename, not the media  
**Priority:** P1 (High Priority)  
**Type:** functionality, UX/design  

**Problem.** Success is the string `output_VIDEO_ID.mp3`. There is no download route and no absolute path. The file exists only on the server CWD.

**Evidence.** `server.cpp` 107–116.

**Impact.** `curl` users get JSON, not audio. A remote client can never retrieve the file. Even locally, CWD is usually `build/`.

**Recommendation.** For a local API: return absolute `file://`-style path plus bytes/sha256. For any networked API: async job + authenticated GET of the artifact, with TTL deletion. Do not add an unauthenticated static file server.

---

### F13 — No preflight for `yt-dlp` / `ffmpeg`; generic failures  
**Priority:** P1 (High Priority)  
**Type:** reliability, UX/design, operations/deployment  

**Problem.** Missing binaries, 403, private/age-gated videos, and disk-full all become `yt-dlp command failed with exit code: N`. Architecture’s error tree (network vs private vs ffmpeg vs disk) is not implemented (`docs/ARCHITECTURE.md` 247–272).

**Evidence.** `downloadVideo` / `convertVideo` throw only the wait status. CLI troubleshooting is a static list (`main.cpp` 73–82). CMake does not `find_program(yt-dlp)` / `find_program(ffmpeg)`.

**Impact.** First-run experience is “conversion failed” with no actionable cause. Operators cannot alert on “binary missing” vs “YouTube blocked us”.

**Recommendation.** Startup and per-job checks. Surface yt-dlp stderr (truncated, sanitized). Classify exit codes / known stderr snippets.

---

### F14 — Shutdown does not stop in-flight work; SIGTERM ignored  
**Priority:** P1 (High Priority)  
**Type:** reliability, operations/deployment  

**Problem.** Only SIGINT is handled (`server.cpp` 42–47, 182). The loop sleeps 100 ms (`205–207`) then `listener.close()`. In-flight `system()` children are not killed. Docker/systemd send SIGTERM.

**Impact.** Orphan yt-dlp/ffmpeg, leaked temps, failed health on restart.

**Recommendation.** Handle SIGTERM. Track child PIDs; kill process groups on shutdown. Drain or fail in-flight HTTP requests. Do not accept new work after shutdown starts.

---

### F15 — Hardcoded bind, port, log level; DEBUG logs full URLs  
**Priority:** P1 (High Priority)  
**Type:** security, operations/deployment  

**Problem.** Listen URL is `http://localhost:8080` (`server.cpp` 186). API sets `LogLevel::DEBUG` (`177`). Every request logs URL and, at debug, client IP (`82–83`). No env/config. README “change the port in source and rebuild” (518).

**Impact.** Cannot deploy without a rebuild. Debug logs become a URL history. If bind is later changed to `0.0.0.0` without auth, the service is an open converter.

**Recommendation.** Config from env: `YTCONV_BIND`, `YTCONV_PORT`, `YTCONV_LOG_LEVEL`, `YTCONV_OUTPUT_DIR`, `YTCONV_MAX_CONCURRENT`, timeouts. Default API log INFO. Log video ID + request ID, not raw URLs, unless debug is explicit.

---

### F16 — Logger is unsafe for the API’s concurrency model  
**Priority:** P1 (High Priority)  
**Type:** bug, reliability  

**Problem.** Singleton with unsynchronized `std::cout` / optional `ofstream`. Header claims thread safety it does not provide.

**Evidence.** `include/logger.h` 29–30 vs `src/utils/logger.cpp` 68–86, 100–110 (`std::localtime`).

**Impact.** Torn lines, crashed file stream, UB under load.

**Recommendation.** Mutex around `log()`. `localtime_r` / `localtime_s`. Or replace with a tiny thread-safe logger. Enable file logging for the API by default under the output/log dir.

---

### F17 — Dead / contradictory public API in headers and CMake  
**Priority:** P2 (Medium Priority)  
**Type:** technical debt  

**Problem.**

- `include/converter.h` 18–25 declares `yt::converter::isValidURL` / `isValidFormat`. Implementations live in `yt::validation`. Linking those symbols fails.
- `isValidString` and `getSupportedFormats` are unused.
- CMake wraps targets in `if(EXISTS ...)` (`CMakeLists.txt` 68–74, 98, 126, 156) — leftover scaffolding.
- CMake links Boost and OpenSSL into the core library though core sources do not include them (`CMakeLists.txt` 77–83).
- Style guide in `CONTRIBUTING.md` (Allman, `m_` members) does not match the code (K&R, no `m_`).

**Impact.** Contributors implement the wrong functions. `BUILD_TESTS=ON` fails immediately. Extra link dependencies complicate Windows/vcpkg.

**Recommendation.** One validation API. Delete dead declarations. Remove `EXISTS` guards. Link Boost/OpenSSL only on the API target if still required by cpprestsdk.

---

### F18 — CMake policy `CMP0167` vs advertised CMake 3.10/3.16  
**Priority:** P1 (High Priority)  
**Type:** operations/deployment, bug  

**Problem.** `cmake_minimum_required(VERSION 3.16)` then `cmake_policy(SET CMP0167 NEW)` (`CMakeLists.txt` 1–4). CMP0167 exists from CMake 3.30. Older 3.16–3.29 CMake errors on unknown policy. README/CONTRIBUTING still say CMake 3.10+.

**Evidence.** `CMakeLists.txt` 1–4; README 82; leftover cache was CMake 4.3.1.

**Impact.** “Follow the README” builds break on common distro CMake.

**Recommendation.**

```cmake
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()
```

Or raise `cmake_minimum_required` to what CI actually has and document it. Add `find_package` fallbacks.

---

### F19 — Test suite does not exist; CI never runs conversions  
**Priority:** P1 (High Priority)  
**Type:** testing  

**Problem.** No `tests/` directory. CMake still lists `tests/test_validator.cpp` and `tests/test_converter.cpp` (`CMakeLists.txt` 151–154). CI: `-DBUILD_TESTS=OFF` (`build.yml` 128). Coverage job expects `.gcda` and exits 0 if missing (`code-quality.yml` 388–391). `quick-test.sh` only greps “error|invalid”. CLI has no `--help` for the documented help test.

**Impact.** The injection, traversal, URL, and cleanup bugs have no regression tests. CI green does not mean the product works.

**Recommendation.** See Phase 4. Highest value: unit tests for URL/ID parsing (no network); spawn-wrapper tests that record argv; API tests with a fake converter; one optional integration job with a short Creative Commons fixture.

---

### F20 — CI does not run on this branch; quality gates are soft; artifact action is old  
**Priority:** P2 (Medium Priority)  
**Type:** operations/deployment  

**Problem.** Workflows trigger on `main` and `develop` only (`build.yml` 5–11, `code-quality.yml` 5–7). clang-tidy, cppcheck, flawfinder, and secrets grep `exit 0` on findings (`code-quality.yml` 153, 243, 282, 298). Secrets grep would match this plan’s word “token” if it were in `.cpp`. `actions/upload-artifact@v3` / `cache@v3` are deprecated relative to v4. Entire `build/` is uploaded. No Windows job despite README platform badge. Coverage installs package name `gcov` (often not a real apt package).

**Impact.** Work on `dev/production-readiness` gets no CI. Formatting is the only hard quality gate. Artifact v3 may fail as GitHub removes it.

**Recommendation.** Trigger on all PRs and this branch. Pin actions to v4. Fail on tests. Do not upload object files. Add Windows only after the spawn/path layer is portable.

---

### F21 — No production packaging, health, or rollback story  
**Priority:** P1 (High Priority) for any hosted API; P2 for CLI-only releases  
**Type:** operations/deployment  

**Problem.** No container, health/readiness, systemd unit, version flag, SBOM, or release workflow. Project `VERSION 1.0.0` is not compiled into binaries. Install rule does not install runtime tools (`yt-dlp`, `ffmpeg`).

**Impact.** There is nothing to “deploy” except raw binaries that assume a developer CWD.

**Recommendation.** If the product is a **CLI**: ship versioned binaries + a brew/apt note that `yt-dlp` and `ffmpeg` are required; `yt2mp3-cli --version`. If the product is an **API**: container with non-root user, read-only rootfs except output volume, health endpoint, resource limits, and a job queue. Do not publish an open converter on the public internet without legal review (YouTube ToS).

---

### F22 — Documentation is stale, machine-specific, and internally inconsistent  
**Priority:** P1 (High Priority)  
**Type:** documentation  

**Problem.** README project tree lists files that do not exist (`handlers.cpp`, `validator.cpp`, `YT2MP3.cpp`, `docs/API.md`, `tests/`). Clone URL is `yourusername/YT-Converter`. CONTRIBUTING runs `./yt2mp3` and `./yt2mp3-API`. TESTING.md and embedded scripts use `/home/ghost/Programming/YT-Converter`. Architecture still centers on `YT2MP3.cpp`. CODE_OF_CONDUCT report address is `[maintainers contact information]`.

**Impact.** Contributors and operators cannot follow the docs to a working, safe build.

**Recommendation.** Rewrite README/ARCHITECTURE/TESTING/CONTRIBUTING against `src/` and the new API/CLI contract after F01–F11. Remove personal absolute paths.

---

### F23 — CLI/API UX: no progress, no cancel, mixed streams, Unicode status  
**Priority:** P2 (Medium Priority)  
**Type:** UX/design  

**Problem.** Conversion can take minutes with only logger lines. No progress from yt-dlp (`--newline` / `--progress`). No cancel. Success uses `✓` / `✗` (`main.cpp` 61, 68). Logger writes INFO to stdout and ERROR to stderr while CLI also writes banners to stdout — piping is messy. API holds the HTTP socket for the whole job (no 202 + poll).

**Impact.** Users think the tool hung. Accessibility on limited terminals is poor. API clients time out.

**Recommendation.** CLI: `--help`, `--version`, `--output-dir`, `--quality`, `--verbose`, ASCII-safe `--no-unicode`, SIGINT cancel. API: 202 + `GET /v1/jobs/{id}` after the security work. Progress as structured logs or SSE only after auth.

There is no visual design system to polish. Do not add a web UI in the first production push.

---

### F24 — Observability is unstructured and has no health/metrics  
**Priority:** P2 (Medium Priority)  
**Type:** operations/deployment  

**Problem.** Logs are `[timestamp] [LEVEL] message`. No request ID, no JSON option, no metrics, no `/health`. File logging exists but is off. No error reporting product.

**Evidence.** `formatMessage` (`logger.cpp` 113–118). Server loop has no probe (`server.cpp` 204–207).

**Impact.** Cannot debug a failed conversion in production or load-balance an API.

**Recommendation.** Appropriate for this size: request/job ID in every line; `LOG_LEVEL`; `/healthz` (process up) and `/readyz` (yt-dlp + ffmpeg + disk). Counters: jobs started/succeeded/failed, active jobs, bytes written. No need for a full APM first.

---

### F25 — Header/docs over-claim security and async behavior  
**Priority:** P2 (Medium Priority)  
**Type:** documentation, technical debt  

**Problem.** README: “Asynchronous Processing”, “Robust error handling”, “Comprehensive YouTube URL validation”. Architecture: format validation “Prevents arbitrary command execution”, “HTTPS Support”, “No logging of URLs by default”. Code: sync handler, logs URLs at INFO, HTTP only, validation does not prevent command execution.

**Impact.** False confidence during review and release.

**Recommendation.** Delete claims that are not true. Document residual risk: this tool downloads YouTube media and may violate YouTube ToS if operated as a public service.

---

### F26 — Legal / ToS / copyright operational risk  
**Priority:** P1 (High Priority) for a public API; P2 for private CLI  
**Type:** operations/deployment  

**Problem.** A production API that fetches arbitrary YouTube URLs for any client is a copyright and ToS hazard. README already says users are responsible (`ARCHITECTURE.md` 430). There is no rate limit, no allowlist of videos, no logging retention policy.

**Impact.** Hosting this as a public converter is not just a tech problem.

**Recommendation.** Ship as a **local CLI** first. If an API remains, keep it localhost-only, documented as personal use, with auth. Get legal review before any public bind.

---

### F27 — Performance: blocking convert-after-download, best-video for audio  
**Priority:** P2 (Medium Priority)  
**Type:** technical debt  

**Problem.** Full muxed MP4 is downloaded before any transcode (`processVideo` 34–41). mp3/wav still pay for video. No cache. Architecture lists streaming conversion as future work (400).

**Impact.** Extra CPU, disk, and time on the common “URL → MP3” path.

**Recommendation.** After security: `yt-dlp -x --audio-format mp3` (or wav) for audio; keep video path for mp4. Unique job directories. Optional reuse of a completed `output_<id>.<fmt>` if checksum/policy allows.

---

### F28 — No TODO/FIXME in source; debt lives in docs and CMake  
**Priority:** P3 (Polish / Future Improvement)  
**Type:** technical debt  

**Problem.** Application `.cpp`/`.h` contain no `TODO`/`FIXME`/`HACK`. The real debt is implicit: `system()`, missing tests, `EXISTS` guards, stale docs.

**Recommendation.** Track the items in this plan rather than scattering TODOs. Do not add placeholder comments instead of fixes.

---

## 4. Implementation Roadmap

Implement in the phase order below. Each task is written so another agent can execute it without re-auditing.

---

### Phase 0 — Release blockers (security + process execution)

Do this before any feature, bind-address change, or public docs that encourage running the API.

#### Task T01 — Replace `system()` with argv spawn

- **Priority:** P0  
- **Problem:** F01. Shell interpolation of URL and video ID.  
- **Evidence:** `downloadVideo` / `convertVideo` in `src/core/converter.cpp` ~80–137.  
- **Recommended Change:** Add `yt::process::run(argv, opts)` that does not invoke `sh`. Pass each argument as its own argv element. Capture stdout/stderr to a bounded buffer. Return real exit code. Time out and kill the process group.  
- **Implementation Notes:** Linux/macOS: `posix_spawnp` + pipe, or a well-reviewed single-file wrapper. Windows: `CreateProcessW` with a properly escaped command line **only if** argv cannot be used; prefer no shell. Do not implement by wrapping `system()` with `escape()`. Put binaries in a dedicated module so converter.cpp has no string-built commands. Default PATH lookup; optional absolute paths from config later.  
- **Files Likely Affected:** new `include/process.h`, `src/utils/process.cpp`; `src/core/converter.cpp`; `CMakeLists.txt`; later tests.  
- **Dependencies:** None. First code change.  
- **Validation:** Unit tests: argv equals `{"yt-dlp","-f",...,"-o",out,url}` with a URL containing `'`, `;`, `$()`, spaces. Integration: fake `yt-dlp` script that writes argv to a file. Confirm `system(` is gone from `src/`.

#### Task T02 — Strict URL parse + video-ID allowlist + reconstructed download URL

- **Priority:** P0  
- **Problem:** F02, F03, F10.  
- **Evidence:** `isValidURL` `regex_search`; `extractVideoID` raw `v=` slice; download uses original URL.  
- **Recommended Change:** Replace search-with-regex with a parser:
  - Accept only allowlisted hosts/paths.
  - Capture ID with `^[A-Za-z0-9_-]{11}$` (YouTube’s current public ID shape; reject longer).
  - Always invoke yt-dlp with `https://www.youtube.com/watch?v=<id>` plus `--no-playlist`.
  - Filenames: `temp_<id>.mp4`, `output_<id>.<fmt>` only from that ID.
  - Return typed errors for empty, unsupported host, playlist-only, channel.  
- **Implementation Notes:** Put parsing in `yt::validation` only. Remove duplicate declarations from `converter.h`. Support `youtu.be/<id>` and `watch?v=` in the same change so F10 is not a second rewrite. Reject IDs containing `/`, `.`, space, quotes. Use `std::regex_match` or a manual parser; add `^` `$`.  
- **Files Likely Affected:** `include/validation.h`, `src/utils/validation.cpp`, `include/converter.h`, `src/core/converter.cpp`.  
- **Dependencies:** T01 can land first; T02 must land before any API exposure. Prefer same PR if small.  
- **Validation:** Table-driven tests (see Phase 4 T14). Cases: injection suffix, embedded YouTube URL on another host, `youtu.be`, `watch?v=&list=`, `../` ID, 11-char happy path, empty, non-YouTube.

#### Task T03 — Stop using GET for conversion; path allowlist

- **Priority:** P0 (API)  
- **Problem:** F04.  
- **Evidence:** `server.cpp` 54–116, 188–189.  
- **Recommended Change:** `POST /v1/conversions` with JSON `{"url","format"}`. `GET /v1/healthz` only besides that. `405` on GET conversion. `404` on unknown paths. Keep localhost bind.  
- **Implementation Notes:** Do not add `0.0.0.0` here. Optional `X-Api-Key` compared with constant-time equality to `YTCONV_API_KEY` if set; if unset, refuse to start unless `--allow-unauthenticated-localhost` is passed (forces a conscious choice).  
- **Files Likely Affected:** `src/api/server.cpp`; README API section after T22.  
- **Dependencies:** T01, T02 (otherwise POST is still RCE).  
- **Validation:** curl POST happy path (mocked converter). GET `/v1/conversions` → 405. GET `/` → 404. Missing key when required → 401.

---

### Phase 1 — Correctness and reliability

#### Task T04 — RAII temp files and output root

- **Priority:** P1  
- **Problem:** F05, F02 residual.  
- **Evidence:** no delete; CWD outputs.  
- **Recommended Change:** Configured `output_root` (default `./output`). Job directory `output_root/jobs/<id>/` or `<id>-<unique>/`. Temp + output live there. Destructor deletes temp. Failed output deleted. Success keeps only the final file.  
- **Implementation Notes:** `std::filesystem`. Create dirs with 0700. `weakly_canonical` must stay under root. Unique suffix if you have not yet added per-ID locks (T05).  
- **Files Likely Affected:** `converter.cpp`, `converter.h`, CLI/API for `--output-dir` / env.  
- **Dependencies:** T02 (safe IDs).  
- **Validation:** After success, no `temp_*` remains. After forced ffmpeg failure, no temp and no zero-byte output. Path-escape unit test.

#### Task T05 — Concurrency cap and per-job isolation

- **Priority:** P1  
- **Problem:** F06.  
- **Evidence:** inline `processVideo`; shared filenames.  
- **Recommended Change:** Process-wide semaphore (`YTCONV_MAX_CONCURRENT`, default 1 for v1). Unique job IDs so two requests never share files. If semaphore full: HTTP 503 JSON (finally matching the README status code) or CLI message. Mutex in Logger (can be T06).  
- **Implementation Notes:** A `std::counting_semaphore` or atomic + condition_variable. Do not queue unbounded jobs in memory without a max wait.  
- **Files Likely Affected:** `server.cpp`, new `include/job_limiter.h`, `logger.cpp`.  
- **Dependencies:** T04 unique dirs make this simpler.  
- **Validation:** Two parallel POSTs with the same URL; both succeed with distinct files or one 503. ThreadSanitizer on logger tests.

#### Task T06 — Thread-safe logger

- **Priority:** P1  
- **Problem:** F16.  
- **Evidence:** `logger.h` vs `logger.cpp`.  
- **Recommended Change:** Lock in `Logger::log`. `localtime_r`. Document “thread-safe for concurrent log()”.  
- **Files Likely Affected:** `include/logger.h`, `src/utils/logger.cpp`.  
- **Dependencies:** None; parallel with T01.  
- **Validation:** N threads logging; no torn lines; TSan clean.

#### Task T07 — ffmpeg/yt-dlp flags: `-y`, `-nostdin`, `--no-playlist`, size/time limits

- **Priority:** P1  
- **Problem:** F07, F08.  
- **Evidence:** command strings in `converter.cpp`.  
- **Recommended Change:**  
  - yt-dlp: `--no-playlist --newline --socket-timeout 30 --max-filesize 500M` (configurable) `--retries 2`.  
  - Audio: do not download video (T18 can refine).  
  - ffmpeg: `-y -nostdin -hide_banner -loglevel error`.  
  - WAV: `-vn -acodec pcm_s16le -ar 44100 -ac 2` (pick one spec and document it).  
  - MP3: keep lame 192k; document 44100.  
  - Child timeout (e.g. 15 min) in the spawn wrapper.  
- **Implementation Notes:** Centralize flags in one struct `DownloadOptions` / `ConvertOptions`.  
- **Files Likely Affected:** `converter.cpp`, `process` wrapper, headers.  
- **Dependencies:** T01.  
- **Validation:** Existing output is overwritten without prompt. Playlist URL downloads one video. A fake hung binary is killed at timeout.

#### Task T08 — Decode exit codes and classify errors

- **Priority:** P1  
- **Problem:** F09, F13.  
- **Evidence:** raw `system()` status; generic `runtime_error`.  
- **Recommended Change:** Types: `BinaryNotFound`, `DownloadFailed`, `ConversionFailed`, `Timeout`, `DiskFull`. CLI and API map to messages + HTTP 400/404/507/504/500. Capture last 4–8 KB of stderr.  
- **Implementation Notes:** Check `ENOENT` from spawn. Do not log full stderr if it contains cookies (none today).  
- **Files Likely Affected:** converter, CLI, API.  
- **Dependencies:** T01.  
- **Validation:** PATH without ffmpeg → clear CLI error, API 503/500 with stable `error_code`. Do not require a live YouTube hit for unit tests (fake binaries).

#### Task T09 — Graceful shutdown and child kill

- **Priority:** P1  
- **Problem:** F14.  
- **Evidence:** SIGINT only; no child tracking.  
- **Recommended Change:** SIGINT + SIGTERM. Registry of child PIDs / process groups. On signal: stop listener, kill children, join handlers with timeout, then exit.  
- **Files Likely Affected:** `server.cpp`, `process.cpp`.  
- **Dependencies:** T01 (need PIDs).  
- **Validation:** Start API, start a long fake job, send SIGTERM, child gone, exit 0, no orphan.

#### Task T10 — Startup preflight

- **Priority:** P1  
- **Problem:** F13.  
- **Evidence:** CMake/runtime never check tools.  
- **Recommended Change:** CLI and API: `yt-dlp --version` and `ffmpeg -version` at start (short timeout). Fail fast with install hints per OS. CMake `find_program` WARN (not REQUIRED) so CI can still compile without yt-dlp if you mock later.  
- **Files Likely Affected:** new `src/utils/dependencies.cpp`, both `main`s, `CMakeLists.txt`.  
- **Dependencies:** T01 helpful but can call `--version` via spawn or a one-off.  
- **Validation:** Hide ffmpeg from PATH in a test env; CLI exits 1 before network.

---

### Phase 2 — Security hardening

#### Task T11 — Config via environment; safe defaults

- **Priority:** P1  
- **Problem:** F15.  
- **Evidence:** hardcoded port and DEBUG.  
- **Recommended Change:** Read `YTCONV_*` env vars. Default bind `127.0.0.1`, port `8080`, log `INFO`, output `./output`. Refuse `0.0.0.0` / `::` unless `YTCONV_ALLOW_REMOTE=1` **and** API key set. Never commit `.env`; add `.env.example` with empty values only.  
- **Implementation Notes:** Do not log secrets. Parse port as integer with range check.  
- **Files Likely Affected:** new `include/config.h`, `src/utils/config.cpp`, `server.cpp`, `main.cpp`.  
- **Dependencies:** T03 if API key is required for remote.  
- **Validation:** Bind/port change without rebuild. Remote bind without key → refuse to start.

#### Task T12 — Redact URLs; request IDs

- **Priority:** P1  
- **Problem:** F15, F24.  
- **Evidence:** `server.cpp` 82; `processVideo` debug logs URL (`converter.cpp` 14).  
- **Recommended Change:** Log `request_id` + `video_id` + format. Raw URL only at DEBUG when `YTCONV_LOG_URLS=1`.  
- **Files Likely Affected:** logger, converter, server, CLI.  
- **Dependencies:** T06.  
- **Validation:** INFO logs after a conversion contain ID not full URL.

#### Task T13 — Headers and local CSRF posture

- **Priority:** P2  
- **Problem:** F04 residual.  
- **Evidence:** no header middleware.  
- **Recommended Change:** `Cache-Control: no-store`. Do not add CORS `*` . If a browser UI is added later, use same-site tokens; out of scope now.  
- **Files Likely Affected:** `server.cpp`.  
- **Dependencies:** T03.  
- **Validation:** Response header test.

---

### Phase 3 — Architecture cleanup (only after 0–2)

#### Task T14 — Single validation module; delete dead API

- **Priority:** P2  
- **Problem:** F17.  
- **Evidence:** `converter.h` 18–25; unused helpers.  
- **Recommended Change:** Validation only in `yt::validation`. Converter consumes `VideoRef`. Remove `EXISTS` CMake guards. Link Boost/OpenSSL only where required.  
- **Files Likely Affected:** headers, `CMakeLists.txt`.  
- **Dependencies:** T02.  
- **Validation:** `nm` / link test; `BUILD_TESTS=ON` compiles.

#### Task T15 — Error type, not string soup

- **Priority:** P2  
- **Problem:** F08/F13 maintainability.  
- **Evidence:** `throw std::invalid_argument` / `runtime_error` with strings.  
- **Recommended Change:** `yt::Error` with `code` + `message` + `http_status`. One map in API and CLI.  
- **Files Likely Affected:** new `include/error.h`; converter, CLI, API.  
- **Dependencies:** T08.  
- **Validation:** Unit test mapping; no raw `e.what()` as public API for unexpected exceptions (keep generic 500).

Do **not** introduce a database, Redis, Kubernetes, or web UI in this phase.

---

### Phase 4 — Tests (highest-value pre-release)

#### Task T16 — Unit tests for validation and filenames

- **Priority:** P1  
- **Problem:** F19.  
- **Evidence:** missing `tests/`; CMake references ghosts.  
- **Recommended Change:** GoogleTest (already wired) or Catch2 if GTest is painful on a platform. Tests for T02 table.  
- **Implementation Notes:** Enable `BUILD_TESTS` in CI Debug.  
- **Files Likely Affected:** `tests/test_validation.cpp`, `CMakeLists.txt`, workflow.  
- **Dependencies:** T02.  
- **Validation:**  
  - Valid: `https://www.youtube.com/watch?v=dQw4w9WgXcQ`, `https://youtu.be/dQw4w9WgXcQ`, extra benign query stripped.  
  - Invalid: empty; `example.com`; injection suffix; host + embedded watch URL; `watch?v=abc/../x`; shorts/playlist-only until supported.  
  - `getOutputFilename` never contains `/` or `..`.

#### Task T17 — Process-wrapper and converter unit tests with fake binaries

- **Priority:** P1  
- **Problem:** F01, F07, F09 cannot be regression-locked without fakes.  
- **Evidence:** live yt-dlp is flaky and ToS-sensitive.  
- **Recommended Change:** `YTCONV_YT_DLP` / `YTCONV_FFMPEG` overrides pointing at scripts that record argv and write a tiny fixture file. Tests: expected argv; timeout kill; non-zero mapped to `DownloadFailed`; temp deleted.  
- **Files Likely Affected:** `tests/test_process.cpp`, `tests/test_converter.cpp`, `tests/fakes/`.  
- **Dependencies:** T01, T04, T08.  
- **Validation:** `ctest` with no network.

#### Task T18 — API contract tests

- **Priority:** P1  
- **Problem:** F04, F11, F12.  
- **Evidence:** no API tests; docs disagree with JSON.  
- **Recommended Change:** Start listener on port `0` or a high port in-process, or run `yt2mp3-api` in CI with fakes. Assert POST body, 400/401/404/405/503, JSON schema, no conversion on GET.  
- **Files Likely Affected:** `tests/test_api.cpp` or a `curl` script in CI.  
- **Dependencies:** T03, T11.  
- **Validation:** Contract file checked in; CI fails on drift.

#### Task T19 — Optional live integration (manual or nightly)

- **Priority:** P2  
- **Problem:** yt-dlp/YouTube break often.  
- **Evidence:** README troubleshooting 403.  
- **Recommended Change:** One short, known-safe video, job `workflow_dispatch` only, not on every PR. Never put cookies or accounts in CI.  
- **Files Likely Affected:** `.github/workflows/integration.yml`.  
- **Dependencies:** T01–T08.  
- **Validation:** Artifact is non-empty mp3; temp gone.

#### Task T20 — Security regression tests

- **Priority:** P1  
- **Problem:** F01–F03.  
- **Evidence:** flawfinder in CI is non-blocking and would not catch argv bugs.  
- **Recommended Change:** Tests that fail if `system(` returns to `src/`. Tests that a URL with `';` never appears as a single shell string. Optional clang-tidy `bugprone-suspicious-string-compare` is not enough — assert spawn argv.  
- **Files Likely Affected:** `tests/test_security_argv.cpp`; CI grep `system(` in `src/`.  
- **Dependencies:** T01, T02.  
- **Validation:** CI step `! grep -R 'system(' src`.

Do not invest in browser/a11y/E2E UI tests — there is no UI. Add CLI `--help` snapshot tests after T21.

---

### Phase 5 — CLI / API UX (targeted, no redesign)

#### Task T21 — CLI flags and honest help

- **Priority:** P2  
- **Problem:** F23, F11.  
- **Evidence:** `main.cpp` 42–47; README `--help`.  
- **Recommended Change:** `--help`, `--version` (from CMake `PROJECT_VERSION`), `--output-dir`, `--format` as flag or keep positional. `--no-unicode`. Print absolute output path. Exit codes: 2 validation, 3 missing deps, 4 download, 5 convert, 1 unknown.  
- **Files Likely Affected:** `src/cli/main.cpp`, `CMakeLists.txt` (`configure_file` version header).  
- **Dependencies:** T10, T04, T08.  
- **Validation:** `yt2mp3-cli --help` exits 0; `--version` prints `1.0.0` or new version; wrong argc still shows help.

#### Task T22 — Align JSON and CLI copy with implementation

- **Priority:** P1  
- **Problem:** F11, F12, F22, F25.  
- **Evidence:** README 254–294 vs `server.cpp` 107–114.  
- **Recommended Change:** One OpenAPI-ish snippet in README. Fields: `status`, `error_code`, `message`, `output_path`, `job_id`. Remove 503-from-nowhere until T05 implements it.  
- **Files Likely Affected:** README, ARCHITECTURE, TESTING, CONTRIBUTING, `printServerInfo`.  
- **Dependencies:** T03, T08.  
- **Validation:** Doc examples copy-paste against a running test server.

#### Task T23 — Progress and cancel (CLI first)

- **Priority:** P2  
- **Problem:** F23.  
- **Evidence:** no progress flags.  
- **Recommended Change:** Forward yt-dlp `--newline` progress to stderr. SIGINT → kill child (T09). API progress later via job resource; not required for CLI-only production.  
- **Files Likely Affected:** process wrapper, CLI.  
- **Dependencies:** T01, T09.  
- **Validation:** During a fake long download, SIGINT yields exit 130/canceled and cleanup (T04).

---

### Phase 6 — Performance

#### Task T24 — Audio-only download path

- **Priority:** P2  
- **Problem:** F27, F08.  
- **Evidence:** always bestvideo+bestaudio.  
- **Recommended Change:** mp3/wav: `yt-dlp -f bestaudio -x --audio-format ...` or download audio and ffmpeg. mp4: merge mp4+m4a as now, with a max height (e.g. 1080) to cap size.  
- **Files Likely Affected:** `converter.cpp`.  
- **Dependencies:** T01, T07.  
- **Validation:** Fake yt-dlp asserts audio-only argv for mp3; video argv for mp4. Manual: mp3 smaller and faster than current.

#### Task T25 — Optional completed-file reuse

- **Priority:** P3  
- **Problem:** duplicate downloads.  
- **Evidence:** no cache.  
- **Recommended Change:** If `output/<id>.mp3` exists and `--force` not set, skip. Document stale risk if YouTube replaces the video.  
- **Files Likely Affected:** converter, CLI flag `--force`.  
- **Dependencies:** T04.  
- **Validation:** Second CLI run does not call yt-dlp (fake counter).

Keep concurrency at 1–2 until metrics exist. Do not add Redis.

---

### Phase 7 — Observability

#### Task T26 — Health and readiness

- **Priority:** P2  
- **Problem:** F21, F24.  
- **Evidence:** no probe endpoints.  
- **Recommended Change:** `GET /v1/healthz` → 200 `{"status":"ok"}`. `GET /v1/readyz` → 200 if binaries found and output dir writable and not shutting down; else 503.  
- **Files Likely Affected:** `server.cpp`.  
- **Dependencies:** T03 routing, T10.  
- **Validation:** curl tests; fail readyz if output dir chmod 000.

#### Task T27 — Structured optional logs + counters

- **Priority:** P2  
- **Problem:** F24.  
- **Evidence:** text-only logger.  
- **Recommended Change:** `YTCONV_LOG_FORMAT=text|json`. In-process counters logged on SIGUSR1 or `GET /v1/metrics` **only on localhost**. No SaaS APM required.  
- **Files Likely Affected:** logger, server.  
- **Dependencies:** T06, T12.  
- **Validation:** JSON line parses; metrics increment after a fake job.

---

### Phase 8 — Build, CI/CD, deployment

#### Task T28 — CMake hygiene

- **Priority:** P1  
- **Problem:** F18, F17, F19.  
- **Evidence:** `CMakeLists.txt` 1–4, 68–74, 149–172.  
- **Recommended Change:** Guard CMP0167. Drop `EXISTS`. Real test targets. `find_program` warnings. Generate `version.h`. Do not `include_directories` globally.  
- **Files Likely Affected:** `CMakeLists.txt`.  
- **Dependencies:** T14, T16.  
- **Validation:** Fresh configure on CMake 3.16 (container) and 4.x. `BUILD_TESTS=ON` in CI.

#### Task T29 — CI that matches the branch and actually tests

- **Priority:** P1  
- **Problem:** F20, F19.  
- **Evidence:** `on.push.branches: main, develop`.  
- **Recommended Change:** `on: [push, pull_request]` all branches. `BUILD_TESTS=ON`. Run `ctest --output-on-failure`. Upgrade `actions/cache` and `upload-artifact` to v4; upload only binaries + test logs. Keep clang-format. Make `! grep system(` a required check. Fix or drop the `gcov` apt package. Do not fail the pipeline only on clang-tidy noise until a `.clang-tidy` is tuned.  
- **Files Likely Affected:** `.github/workflows/*.yml`.  
- **Dependencies:** T16–T18, T01.  
- **Validation:** PR from this branch is green with tests. Artifact v4 works.

#### Task T30 — Release artifacts (CLI-first)

- **Priority:** P2  
- **Problem:** F21.  
- **Evidence:** no release workflow.  
- **Recommended Change:** Tag `vX.Y.Z`. Build Ubuntu + macOS binaries. Checksum file. Release notes: requires `yt-dlp` and `ffmpeg` on PATH. Windows after spawn portability.  
- **Files Likely Affected:** `.github/workflows/release.yml`.  
- **Dependencies:** T21 version, T28.  
- **Validation:** Dry-run tag on a fork.

#### Task T31 — Container only if you still want a hosted API

- **Priority:** P2 (skip entirely for CLI-only production)  
- **Problem:** F21, F26.  
- **Evidence:** no Dockerfile.  
- **Recommended Change:** Distroless or debian-slim, non-root, `yt-dlp` + `ffmpeg` pinned, volume `/data/output`, `HEALTHCHECK` curl `/v1/healthz`, memory/cpu limits, read-only root. Default `127.0.0.1` — publishing `80/443` requires T11 remote guards + legal sign-off.  
- **Files Likely Affected:** `Dockerfile`, `.dockerignore`, compose example.  
- **Dependencies:** T01–T12, T26.  
- **Validation:** Container starts, health 200, POST with API key, file on volume, user is non-root.

---

### Phase 9 — Documentation and release validation

#### Task T32 — Rewrite operator docs to match the code

- **Priority:** P1  
- **Problem:** F22, F25.  
- **Evidence:** README tree; `/home/ghost/Programming/...`; `yourusername`.  
- **Recommended Change:** README: real clone URL, real flags, real JSON, prerequisites table once (not twice), ToS warning, localhost-only API warning. ARCHITECTURE: `src/cli/main.cpp`, `src/api/server.cpp`, spawn, output root. TESTING: no personal paths; point at `ctest` and `quick-test.sh`. CONTRIBUTING: actual binary names, actual style (or reformat code — pick one). Add `docs/API.md` or delete the reference. Contact for CoC.  
- **Files Likely Affected:** `README.md`, `docs/*`, `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, `quick-test.sh`.  
- **Dependencies:** T21, T22, T03 so docs do not freeze the old GET API.  
- **Validation:** A clean machine can follow README to build, run `--help`, run unit tests, and refuse a bad URL without network.

#### Task T33 — `quick-test.sh` as a thin wrapper around ctest

- **Priority:** P2  
- **Problem:** script rebuilds from scratch and greps loosely; TESTING.md duplicates it.  
- **Evidence:** `quick-test.sh` 135–148.  
- **Recommended Change:** After T16, script: deps check → cmake → ctest. Keep one conversion example commented.  
- **Files Likely Affected:** `quick-test.sh`, `docs/TESTING.md`.  
- **Dependencies:** T16, T28.  
- **Validation:** Script exit 0 on a machine with deps.

#### Task T34 — Pre-release validation pass (manual)

- **Priority:** P1  
- **Problem:** Need a human gate after the above.  
- **Evidence:** This plan’s checklist (section 5).  
- **Recommended Change:** Run the checklist on Linux and macOS. Do not call production-ready until P0 items are gone and tests are in CI.  
- **Files Likely Affected:** none (process).  
- **Dependencies:** T01–T12, T16–T18, T28–T29, T32.  
- **Validation:** Section 5 all P0/P1 boxes checked.

---

## 5. Production-Readiness Checklist

### Identity and scope

- [ ] Requirement: Work is on `dev/production-readiness` in `NovrusShehaj/YT-Converter` and this plan has been updated if HEAD is not `7a52cb6` plus the implemented commits.
- [ ] Requirement: No application secrets are committed; `.env` stays gitignored.

### Security (must be true before any network exposure)

- [ ] Requirement: `src/` contains no `system(` / `popen(` / shell-concatenated commands.
- [ ] Requirement: yt-dlp and ffmpeg are started with argv (or equivalent non-shell API).
- [ ] Requirement: Video IDs used in paths match `^[A-Za-z0-9_-]{11}$` (or the documented allowlist).
- [ ] Requirement: URLs are full-match validated; download URL is reconstructed from the ID.
- [ ] Requirement: `yt-dlp` is always invoked with `--no-playlist`.
- [ ] Requirement: Conversion is not a GET; unknown paths 404.
- [ ] Requirement: Default bind is loopback; remote bind requires API key.
- [ ] Requirement: Path traversal tests pass (`..`, extra hosts, quote/semicolon IDs).
- [ ] Requirement: INFO logs do not print full user URLs by default.
- [ ] Requirement: Flawfinder/CI grep for `system(` is a failing gate.

### Correctness and reliability

- [ ] Requirement: Temp files are deleted on success and failure.
- [ ] Requirement: Output lives under a configured directory, not an arbitrary CWD leak.
- [ ] Requirement: ffmpeg uses `-y -nostdin`; reruns overwrite safely.
- [ ] Requirement: Child processes have a timeout and are killed on CLI/API shutdown.
- [ ] Requirement: SIGTERM and SIGINT both shut down the API.
- [ ] Requirement: Concurrent jobs are capped; same-ID jobs do not share files.
- [ ] Requirement: Logger is mutex-protected and TSan-clean for `log()`.
- [ ] Requirement: Missing `yt-dlp`/`ffmpeg` fails before a network call with a clear message.
- [ ] Requirement: `youtu.be` and `watch?v=` both work; playlist-only and channel URLs are explicit errors.
- [ ] Requirement: Documented MP3/MP4/WAV flags match ffmpeg argv.

### API / CLI contract

- [ ] Requirement: `--help` and `--version` work.
- [ ] Requirement: README examples match the running server (method, path, JSON fields).
- [ ] Requirement: HTTP codes 400/401/404/405/503/500 are actually produced as documented.
- [ ] Requirement: Success response includes an absolute `output_path` (and does not pretend the bytes were streamed unless they are).

### Testing

- [ ] Requirement: `tests/` exists and `cmake -DBUILD_TESTS=ON` builds `yt-converter-tests`.
- [ ] Requirement: `ctest` runs on every PR without network.
- [ ] Requirement: Fake-binary tests cover argv, timeout, cleanup, and error mapping.
- [ ] Requirement: API contract tests cover POST/GET/missing params/invalid URL.

### Observability and operations

- [ ] Requirement: `/v1/healthz` and `/v1/readyz` exist if the API is shipped.
- [ ] Requirement: `YTCONV_LOG_LEVEL`, bind, port, output dir, max concurrent are configurable without rebuild.
- [ ] Requirement: Shutdown leaves no orphan `yt-dlp`/`ffmpeg`.

### Build / CI / release

- [ ] Requirement: CMake configures on the documented minimum version (policy guarded).
- [ ] Requirement: CI runs on this branch and on pull requests.
- [ ] Requirement: CI actions are current (cache/artifact v4 or pinned working versions).
- [ ] Requirement: Windows is either tested or removed from marketing badges.
- [ ] Requirement: Version is embedded in the binary.

### Documentation / legal

- [ ] Requirement: README clone URL, tree, and API samples match the repo.
- [ ] Requirement: No `/home/ghost/Programming/...` paths in docs.
- [ ] Requirement: Architecture diagram names the real files and `system()`-free spawn.
- [ ] Requirement: README states localhost-only, personal-use intent, and YouTube ToS/copyright responsibility.
- [ ] Requirement: Public Internet deployment is explicitly out of scope until legal review.

### Explicitly out of scope for v1 production (CLI-local)

- [ ] Requirement: No public unauthenticated converter.
- [ ] Requirement: No web UI redesign.
- [ ] Requirement: No playlist/channel batch until isolation, quotas, and ToS review exist.

---

## 6. Recommended Execution Order

Minimize rework: **lock the execution and validation layer first**, then files/jobs, then API shape, then tests that freeze that shape, then docs/CI.

```
T01 spawn            ─┐
T06 logger mutex     ─┼─ parallel (no overlap)
T18 CMake policy     ─┘  (T28 partial: CMP0167 only, immediately)

T02 URL/ID parser          (after or with T01)
T04 output root + RAII     (needs T02)
T07 flags + timeouts       (needs T01)
T08 error classification   (needs T01)
T10 preflight              (parallel with T07)

T05 concurrency            (needs T04)
T09 shutdown               (needs T01)
T03 POST + routes          (needs T01+T02)
T11 env config             (with T03)
T12 log redaction          (needs T06)

T14 header/CMake cleanup   (after T02)
T15 error type             (after T08; can wait)

T16 unit validation        (after T02)     ─�08; can wait)

T16 unit validation        (after T02)     ─┐
T17 fake-binary tests      (after T01/T04) ─┼─ parallel
T20 security grep/argv     (after T01)     ─┘
T18 API tests              (after T03)

T21 CLI flags              (after T10/T08)
T24 audio-only             (after T07; isolated)
T26 health                 (after T03/T10)
T22+T32 docs               (after contract stable)
T29 CI                     (as soon as T16 exists; expand later)
T33 quick-test             (after ctest)
T34 manual checklist
```

**Do not parallelize** T01 with drive-by refactors of `converter.cpp` (merge conflicts).  
**Do not** write README API samples until T03.  
**Do not** add Docker (T31) or a web UI before T01–T12.  
**Do not** enable `0.0.0.0` in the same PR as “first API fix”.

Safe parallel tracks after T01/T02 land:

1. Reliability (T04–T10)  
2. Tests (T16/T17/T20)  
3. Logger/config (T06, T11, T12)  
4. Docs-only typo fixes that do not describe the old GET API

---

## Current Production-Readiness Assessment

### Major strengths

- Small, readable C++17 core: CLI and API both call one function, `yt::converter::processVideo`.
- Format allowlist for `mp3` / `mp4` / `wav` is real (`normalizeFormat` + `isValidFormat`).
- API default bind is loopback HTTP, which limits accidental Internet exposure (it does not make the API safe).
- MIT license, CoC, and a CMake split into `yt-converter-core`, `yt2mp3-cli`, `yt2mp3-api`.
- CI exists for Ubuntu/macOS compile on `main`/`develop`; clang-format can fail the quality workflow.
- `.gitignore` excludes media, `build/`, and `.env*`. No committed secrets were found in inspected source.
- Logger abstraction (levels, optional file) is a reasonable place to add request IDs later.

### Major blockers

1. **Command injection and path traversal** via `system()` + `regex_search` + raw `videoID` (F01–F03).  
2. **State-changing unauthenticated GET** on all paths (F04).  
3. **No cleanup, no timeouts, no concurrency limit** — disk and process leaks (F05–F08, F14).  
4. **No tests**; CMake/CI advertise a suite that is not in the tree (F19).  
5. **Docs describe a different product** (paths, JSON, directories, async, HTTPS) (F11, F22, F25).  
6. **No deployable artifact or health story** for an API (F21).

### Highest-risk areas

- `src/core/converter.cpp` command construction and filename use.  
- `src/utils/validation.cpp` `std::regex_search`.  
- `src/api/server.cpp` GET-everything handler, DEBUG URL logging, sync `processVideo`.  
- Operating `yt-dlp` against arbitrary URLs (ToS, 403s, huge files).  
- Any future change that binds `0.0.0.0` without T01–T12.

### Most important next step

**Implement T01 and T02 in one focused change:** spawn yt-dlp/ffmpeg without a shell, and accept only a full-match YouTube URL whose captured 11-character ID is the only string interpolated into paths and argv. Add the argv/parser unit tests (T16/T17/T20) in the same effort so the fix cannot regress. Do not add features, a UI, or a public bind until that lands.

### Is production deployment recommended?

**No.**

- **Public or LAN API:** unsafe. Treat as a release blocker.  
- **Local CLI for a trusted user:** usable as an experimental tool if the user quotes URLs and accepts leftover `temp_*.mp4` files, but it is still not “production-ready” (injection if the URL is untrusted, no `--help`/`--version`, docs wrong, no tests).  
- **After Phases 0–2, 4 (unit/API tests), 8 (CI on this branch), and 9 (docs):** a **localhost CLI + optional localhost API** can be called production-ready for personal use. A hosted multi-tenant converter would still need legal review, quotas, auth, and a job queue that this repo does not have.

No numeric score is useful here: P0 security in the only conversion path is sufficient to fail production readiness.
