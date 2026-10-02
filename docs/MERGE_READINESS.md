# Merge-readiness evidence

This records the evidence for the remediation in `.agents/merge-readiness-remediation-plan.md`
(review findings 1–12 plus the build gates). It follows the plan's rule: an unexecuted check is
listed as pending, never as verified.

- Branch: `dev/api-improvements`
- Code commit verified: `678dddbc0bb883f12c7a2ea3b559b5c8726fca48` (later commits change documentation only)
- Review baseline: `d703487`; main baseline: `03363e0`
- Date: 2026-10-01

## Local platform

| Item | Version |
|---|---|
| OS | Arch-based Linux (Omarchy), kernel 7.2.5, x86_64 |
| Compilers | GCC 16.2.1, Clang 22.1.8 |
| CMake | 3.31.4 |
| cpprestsdk / Boost | 2.10.18 / 1.74 from Debian bookworm packages (`libcpprest-dev`, `libboost1.74-dev`), extracted locally |
| GoogleTest | 1.18.0 |
| ffmpeg / ffprobe | n9.0.1 (host); 5.1.9 inside the reproduced image |
| clang-format / clang-tidy | 18.1.8 (pinned, via `uvx`) / 22.1.8 |

Test inventory at the verified commit: 99 core tests (`yt-converter-tests`), 5 real-media tests
(`yt-converter-media-tests`), 25 HTTP tests (`yt-converter-api-tests`), plus the `no_system_in_src`,
`cli_help`, and `cli_version` CTest targets.

## Evidence matrix

| Gate | Status | Evidence |
|---|---|---|
| Clean checkout | **Verified locally** | `git archive HEAD` into a different absolute directory, then configure in `build/` and build Debug and Release with `BUILD_API=ON`. Both build, and `ctest` passes 6/6 in each. `git ls-files` contains no `build/`, `CMakeCache.txt`, or `CMakeFiles/` entries. |
| Missing cpprestsdk | **Verified locally** | `cmake -DBUILD_API=ON -DCMAKE_DISABLE_FIND_PACKAGE_cpprestsdk=ON` fails with "BUILD_API=ON requires cpprestsdk". `-DBUILD_API=OFF` builds the CLI. |
| Core and API, Linux | **Verified locally** | Debug and Release with `BUILD_API=ON`; HTTP tests built as `yt-converter-api-tests` and executed (25/25). The full suite also passed 3 consecutive repeats (`ctest --repeat until-fail:3`, Release `-Werror`). |
| Core and API, macOS | **Pending CI** | Not runnable here. Homebrew's `cpprestsdk` 2.10.19 is bottled for current macOS but deprecated (upstream archived) and depends on Boost 1.92. The CI `build` job on `macos-latest` is the evidence. |
| Formatting | **Verified locally** | `.clang-format` parses (`--dump-config`); `clang-format 18.1.8 --dry-run -Werror` passes on every tracked C++ file. |
| Compiler warnings | **Verified locally** | GCC and Clang Debug builds with `-DYTCONV_WERROR=ON` (API and tests) succeed. |
| Static analysis | **Verified locally** | `clang-tidy -p build` over `src/*.cpp` with the checked-in `.clang-tidy` (all findings are errors): 0 findings. CI uses Ubuntu's clang-tidy. cppcheck and flawfinder are advisory only. |
| Real media (finding 1) | **Verified locally** | `MediaTest.*` run real ffmpeg/ffprobe on generated media: MP3 (mp3, 44.1 kHz, stereo), WAV (pcm_s16le, 44.1 kHz, stereo), MKV→MP4 stream-copy remux, MP4 bypass (ffmpeg not spawned), and a failed real encode that keeps the old output and leaves no partial. Removing `-f` fails these tests and the fake-ffmpeg tests. |
| Authorization (finding 2) | **Verified locally** | `ApiTest.JobRoutesEnforceConfiguredApiKey`: missing/wrong/correct key on POST, GET job, and DELETE job; 401 for unknown IDs without a key; an unauthorized DELETE leaves the job running; 401 bodies carry no job fields. Unauthenticated-localhost mode is covered too. Without the fix, 19 assertions fail. |
| Shared work (findings 4, 5, 8) | **Verified locally** | Barrier-driven tests: `SourceCacheTest.*` (eviction and refresh during an encode, two separate CLI processes), `JobQueueTest.Cancel*` (follower, original, MP3 vs. a shared WAV download, a waiting job, all subscribers, queued, during encode, repeated, completion races, shutdown), `ReusePolicyTest.*` (48-case policy matrix and publication ordering). Each core rule was mutated and the matching tests failed. |
| Resource bounds (6, 9, 10) | **Verified locally** | Operations, live jobs, and history (`JobQueueTest.SequentialWorkload*`, `DuplicateStorm*`, TTL with an injected clock); request bytes (`GateTest.*`, raw sockets, `peak_request_bytes`); callback lines (`ProcessLines.*`, `peak_line_bytes`); source leases and cache bytes (`SourceCacheTest.AllLeased*`, `Oversized*`). |
| Synchronous mode (finding 7) | **Verified locally** | `SyncApiTest.*`: operation and child limits with `max_concurrent=1`, immediate 503 at admission, health under 1 s while sync responses are open, sharing, typed failures, shutdown resolves open responses with 503. |
| Shutdown | **Verified locally** | `JobQueueTest.ShutdownCancelsEverythingAndJoinsPromptly` (callbacks once, children reaped, late submits rejected), `SyncApiTest.ShutdownResolvesOutstandingSyncResponses`, `GateTest.ShutdownClosesHalfReadRequests`, container SIGTERM check. |
| Metrics (finding 11) | **Verified locally** | `MetricsTest.*`: exact counter deltas against fixture sizes for cold, warm, reused, shared, failed-encode, canceled, and refresh scenarios; gauges return to zero. |
| Readiness (finding 12) | **Verified locally** | `ReadinessTest.*` and `ApiTest.ReadinessRecovers*`: a launch failure forces a re-probe inside the TTL, a restored tool recovers, per-configuration keys, coalesced probes, zero TTL, invalidation during a blocked probe. Each rule was mutated and its test failed. |
| Docker image (finding 3) | **Partially verified locally; pending CI** | No Docker daemon access here. Both Dockerfile stages were reproduced in an official `debian:bookworm-slim` rootfs under an unprivileged user namespace (`bwrap`). The builder (GCC 12.2, CMake 3.25.1, `make` explicitly installed) builds without warnings. `collect-runtime-packages.sh` yields `libbrotli1 libc6 libcpprest2.10 libgcc-s1 libssl3 libstdc++6 zlib1g`, and `verify-runtime.sh` passes (no unresolved libraries, no compiler). `in-container-checks.sh` passes as UID 10001 with networking unshared: CLI and API startup, health/readiness, a fake conversion (202, then succeeded), 401 without the key, authorized cancellation, clean SIGTERM. The only deviation from a real build: dpkg's `chown` to group `staff` cannot work in a single-UID namespace, so `chown` was masked during package configuration. The image `HEALTHCHECK`, `docker run` with the default entrypoint, and Compose (`tests/container/smoke.sh`, `compose-smoke.sh`) need Docker and run in the required CI `container` job. |
| Sanitizers | **Verified locally** | ASan+UBSan (leak detection on), full suite including API: 6/6. One UBSan `vptr` report inside cpprestsdk's own `bytestream::open_istream` is suppressed by `tests/sanitizers/ubsan.supp` (cpprest's `Concurrency` namespace only). TSan on the core (queue, single-flight, cache, process, metrics, reuse policy): 5/5, no reports. |
| Windows | **Not supported; unverified** | The API's request gate is POSIX-only, and the Windows paths in `process.cpp` and `file_lock.cpp` are not compiled or tested anywhere. README and ARCHITECTURE state this. |
| Timing | **Pending manual run** | `.github/workflows/integration.yml` (manual dispatch only) logs `download_ms`, `convert_ms`, `source_bytes`, `output_bytes`, and the matching metrics, and checks for a non-empty output. It was not run; PR tests make no YouTube requests. |
| Re-review | **Pending** | Needs an independent review of the final diff against current `origin/main`. The implementer's self-review fixed two issues (`678dddb`) but is not independent. |

## CI gates that produce the pending evidence

Workflow `Build`: `build` (Ubuntu and macOS × Debug and Release, requires the API binary and API
and media tests in `ctest -N`, `YTCONV_REQUIRE_MEDIA_TESTS=1`), `clean-checkout`, `sanitizers`
(ASan+UBSan, TSan), and `container`, all required by `build-status`. Workflow `Code Quality`:
`clang-format`, `no-shell-spawn`, `clang-tidy`, and `compiler-warnings` (GCC and Clang
`-Werror`), all required by `quality-summary`. These were not run for this commit because the
branch has not been pushed.

## Commits

| Commit | Package |
|---|---|
| `2c14bfd` | G0: reproducible checkout, formatter, strict API build, CI gates |
| `5af9fcd` | R1: explicit ffmpeg muxer, unique partials, real-media tests |
| `86acf1f` | R2: API key on job routes |
| `a52e1be` | R10: bounded callback lines, nonblocking drains, child guard |
| `1da88d8` | R4: leased immutable source generations, cross-process locks |
| `e6a258e` | R5 and R6: jobs separate from operations; bounded subscriptions and history |
| `1a7cae3` | R8: one reuse policy, refresh semantics, publication ordering |
| `d0faf54` | R7: synchronous mode through the queue |
| `fa30147` | R9: request gate and asynchronous bounded body reads |
| `1564c0d` | R11: physical vs. logical metrics |
| `470b874` | R12: readiness invalidation and recovery |
| `371db1e` | R3: runtime dependency closure, container smoke tests |
| `678dddb` | Review hardening (publication record, accept backoff, sanitizer suppression) |
