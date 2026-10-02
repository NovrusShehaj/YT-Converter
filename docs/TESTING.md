# Testing

Automated tests do not contact YouTube. They use GoogleTest and scripts under `tests/fakes/`.

## Run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DBUILD_API=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

`BUILD_API=ON` (the default) requires cpprestsdk and builds the separate `yt-converter-api-tests`
binary; configuration fails when cpprestsdk is missing. Use `-DBUILD_API=OFF` for a CLI-only build.
`./quick-test.sh` is a wrapper around the same commands (`BUILD_API=OFF ./quick-test.sh` for CLI only).

## Static checks

The formatter and static-analysis gates are pinned so local runs match CI:

```sh
uvx --from clang-format==18.1.8 clang-format --dry-run -Werror $(git ls-files '*.cpp' '*.h')
clang-tidy -p build --quiet $(git ls-files 'src/*.cpp')   # .clang-tidy makes findings errors
cmake -S . -B build-werror -DYTCONV_WERROR=ON -DBUILD_TESTS=ON && cmake --build build-werror
```

Sanitizer builds use `-DYTCONV_SANITIZE=address,undefined` or `-DYTCONV_SANITIZE=thread`.

## What is covered

| Test | Purpose |
|---|---|
| `test_validation` | URL/ID table: watch, youtu.be, shorts, injection, foreign host, playlist, channel |
| `test_process` | argv spawn, missing binary, non-zero exit, timeout kill; `ProcessLines.*` bound callback lines with the compiled `ytconv-output-writer` fixture (multi-megabyte newline-free output, giant line then progress, unterminated final line, throwing callback reaps the child, timeout, cancellation) |
| `test_converter` | fake download/convert, temp cleanup, reuse, error mapping, audio vs video argv |
| `test_media` | Real ffmpeg/ffprobe on locally generated media: MP3, WAV, MKV→MP4 remux, MP4 bypass, failed replacement. Skips only when the tools are absent; `YTCONV_REQUIRE_MEDIA_TESTS=1` turns that skip into a failure (CI sets it) |
| `test_source_cache` | Leased generations survive eviction and refresh during an encode, TTL, cap order, all-leased pressure, oversized sources, failed refresh, extension changes, crashed downloads, two separate CLI processes |
| `test_job_queue` | Per-job cancellation of shared operations (follower, original, MP3 vs shared WAV download, waiting job, all subscribers, queued, during encode, repeated, races, shutdown) and bounded subscriptions/history (sequential load, duplicate storm, TTL with an injected clock, count limit, config validation) |
| `test_reuse_policy` | 48-case reuse/force/refresh matrix with content and generation checks, publication ordering, refresh download sharing, queue coalescing, CLI `--force`/`--refresh` |
| `test_metrics` | Exact counter deltas for cold/warm/reused/shared/failed/canceled/refresh scenarios; gauges return to zero |
| `test_dependencies` | Readiness invalidation after a launch failure, recovery, per-configuration keys, coalesced probes, zero TTL, invalidation during a blocked probe |
| `test_security_argv` | reconstructed URL, `--no-playlist`, safe filenames |
| `test_api` (`yt-converter-api-tests`) | HTTP contract, API-key matrix on every service route, synchronous mode through the queue, request-gate limits over raw sockets (exact/over-limit, chunked, slow, malformed, disconnect, admission, shutdown mid-read, bypass), readiness recovery, force/refresh over HTTP |
| `test_error` / `test_config` / `test_logger` | mappings, env guards, concurrent log lines |
| `no_system_in_src` | `ctest` + CI fail if `system(` or `popen(` returns in `src/` |
| `cli_help` / `cli_version` | `--help` and `--version` exit 0 |

Set `YTCONV_YT_DLP` and `YTCONV_FFMPEG` to the fake scripts only in tests. Production binaries look up real tools on `PATH`.

Races are driven by marker-file barriers, not sleeps: `tests/fakes/ffmpeg-copy` (with
`YTCONV_GATE_DIR`/`YTCONV_GATE_FORMAT`) and `tests/fakes/yt-dlp` (with `YTCONV_YTDLP_GATE_DIR`) write
`started.<pid>` and wait for a `release` file, so a test can hold an encode or download open at
an exact point. `ffmpeg-copy` writes `OUT:` plus its input, so tests can prove which source
generation an output came from.

## Sanitizers

```sh
cmake -S . -B build-asan -DBUILD_TESTS=ON -DYTCONV_SANITIZE=address,undefined
UBSAN_OPTIONS=suppressions=$PWD/tests/sanitizers/ubsan.supp ctest --test-dir build-asan
cmake -S . -B build-tsan -DBUILD_TESTS=ON -DBUILD_API=OFF -DYTCONV_SANITIZE=thread
ctest --test-dir build-tsan
```

The UBSan suppression covers one `vptr` report raised inside cpprestsdk's own stream headers.
ThreadSanitizer runs without the API because cpprestsdk and Boost.Asio are not instrumented.

## Container

`tests/container/smoke.sh IMAGE` checks the built image as UID 10001 (CLI and API startup,
health/readiness, a fake conversion, API-key enforcement, authorized cancellation, SIGTERM, the
image `HEALTHCHECK`). `tests/container/compose-smoke.sh` runs the documented Compose setup and
the root-owned bind-mount case. Both need Docker and run in the CI `container` job.

## Manual conversion

A live download is optional and ToS-sensitive. After unit tests pass:

```sh
./build/yt2mp3-cli --output-dir ./output "https://www.youtube.com/watch?v=VIDEO_ID" mp3
```

Confirm `./output/<id>.mp3` exists and `./output/jobs/` has no leftover `source.*` files.

The API:

```sh
./build/yt2mp3-api --allow-unauthenticated-localhost
curl -X POST http://127.0.0.1:8080/v1/conversions \
  -H 'Content-Type: application/json' \
  -d '{"url":"https://youtu.be/VIDEO_ID","format":"mp3"}'
```

`.github/workflows/integration.yml` is `workflow_dispatch` only and does not hit YouTube on every PR.
