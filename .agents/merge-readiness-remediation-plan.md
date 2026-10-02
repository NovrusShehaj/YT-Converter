# YT-Converter merge-readiness remediation plan

Date: 2026-10-01  
Reviewed branch: `dev/api-improvements`  
Review baseline: `d703487ed97f1c4f3e9866f59cae65b5e5a30541`  
Main baseline: `03363e0e771a8aec09f605e9ed6fd3daf1db4d86`

## Objective and completion rule

Resolve review findings 1–12, restore reproducible builds, and collect the evidence needed for a new merge-readiness review. The target is 100/100 readiness: no unresolved merge blockers, verified behavior for the supported deployment, and passing required checks against the final commit.

A plan, passing fake-tool tests, or twelve checked boxes cannot guarantee a future score. Mark this work complete only after the acceptance criteria below pass and a fresh review finds no remaining material defects. Reassess the current branch if it changes after this document was written.

This plan supplements `.cursor/production-performance-plan.md`. Keep its personal/localhost and trusted-LAN boundary, argv-based child execution, canonical YouTube URLs, audio-only selection, atomic publication, and manual-only live conversions. Streaming pipelines, new bitrate options, a database, and a public multi-tenant service remain outside this remediation.

## Evidence from the review

- A fresh Debug core/CLI build passed; 39 core tests and four CTest targets passed.
- A valid local audio fixture with real ffmpeg failed because the temporary output ended in `.mp3.partial` without an explicit muxer.
- Targeted checks reproduced source eviction during encoding, stale completed output after refresh, reuse despite `reuse_completed=false`, cancellation of a coalesced leader through its follower, and cancellation propagating from MP3 to an uncanceled WAV job sharing its download.
- API compilation and HTTP tests were not verified because cpprestsdk was unavailable. CMake silently disabled the API despite `BUILD_API=ON`.
- The Docker dependency finding was established from the build/runtime stages; container startup was not executed.
- A clean export of the tracked branch failed configuration in `build/` because its committed cache contained local absolute paths. The formatter configuration also failed parsing locally. These two defects already existed on main.

Do not turn an unexecuted check into a claim of verification. Record command, platform, commit, result, and relevant log for each release gate.

## Design decisions to settle before implementation

### Separate client jobs from shared operations

A job is a client's independently cancelable request and retained status. An operation owns a download or encode, its immutable result, execution lifetime, subscribers, and operation cancellation token. Neither is owned by the first client's job ID.

- Canceling a job detaches its subscription. It must not cancel surviving subscribers.
- Cancel an operation when its final interested subscriber leaves, or when service shutdown requires it.
- Bound actual operations and child processes independently of the number of subscriptions.
- Retain immutable result/status data separately from executing requests. Completed status must not depend on a live leader record.
- Ordinary same-format requests may share an operation. Requests with incompatible output policies must not be accidentally attached.

Keep execution on bounded workers. Do not solve leader cancellation by creating an unbounded thread or future for each flight. A worker continuing a shared download after its original requester cancels remains accounted for until that work actually ends.

### Define cache and output ownership

Use immutable source generations and a source lease lasting through encoding or MP4 publication. Refresh publishes a new generation instead of deleting or overwriting the generation an existing reader uses. Eviction operates on entries eligible for retirement, not arbitrary source paths.

Atomic output replacement must use a unique temporary path on the output filesystem. Serialize competing writers to the same final output and define their order. Keep locks away from child execution except a necessary per-output writer guard; never hold a global queue/cache mutex during a download, encode, HTTP reply, or worker join.

### Define compatibility and resource contracts

- Preserve 202 asynchronous submission, 200 immediate reuse/synchronous completion, 401 authentication failure, 503 admission rejection, and 409 cancellation of a terminal job.
- Expired retained job IDs return 404. Document that job history is bounded and process-local.
- For minimum compatibility risk, preserve the current executable admission limit: `YTCONV_QUEUE_DEPTH` bounds queued plus running conversion operations. Update documentation that currently describes it as waiting jobs only. Give subscribers and retained history separate explicit limits.
- Preserve the existing 8,192-byte decoded request-body limit and 400 oversized-body response.
- `force` replaces completed output using a permitted cached source. `refresh` requests a new source generation and replacement output. Disabled completed-output reuse also requires fresh encoding/publication, but may reuse a permitted source.
- Do not rely on random job IDs as authorization. Enforce the configured API-key policy on job routes.

## Traceability and implementation order

The issue numbers below match the review. Implementation order follows dependencies rather than numeric order.

| Work package | Review finding | Primary change | Depends on |
|---|---|---|---|
| G0 | Additional build gates | Clean tracked checkout, valid formatter, explicit API build | None |
| R1 | 1 | Explicit ffmpeg muxer and local media integration | G0 |
| R2 | 2 | Authorization for status/cancel | G0 |
| R10 | 10 | Bounded callback line buffering | G0 |
| R4 | 4 | Immutable leased sources and coordinated eviction | R1 |
| R5 | 5 | Subscriber-aware cancellation and operation ownership | R4 |
| R6 | 6 | Bounded subscriptions and terminal retention | R5 |
| R8 | 8 | Consistent reuse/force/refresh behavior | R4, R5 |
| R7 | 7 | Sync responses using bounded execution | R5, R6 |
| R9 | 9 | Bounded asynchronous body extraction | R2 |
| R11 | 11 | Once-per-operation measurements | R5, R6, R8 |
| R12 | 12 | Production readiness invalidation | R5 |
| R3 | 3 | Complete runtime dependencies and container checks | G0; final verification after API fixes |
| G1 | All | Documentation, release gates, independent re-review | All packages |

## G0 — Restore trustworthy build and test gates

**Files:** `CMakeLists.txt`, `.clang-format`, `.gitignore`, tracked `build/*`, `.github/workflows/build.yml`, `.github/workflows/code-quality.yml`, `.github/workflows/integration.yml`.

1. Remove generated build files from the Git index while preserving developers' local build directories. Keep `build/` ignored. Confirm there is no tracked cache, generated Makefile, configure log, object, or binary.
2. Correct the boolean formatter value `AllowShortLoopsOnASingleLine` to `false`. Check the whole configuration with the formatter used in CI, then format affected source files.
3. Make `BUILD_API=ON` require cpprestsdk instead of silently disabling the API. An explicitly requested API build must fail with an actionable dependency message. CLI-only environments use `BUILD_API=OFF` explicitly.
4. Install dependencies needed by the existing supported Linux/macOS CI matrix. Verify installation steps themselves succeed on those runners. Ensure API tests are registered and actually run; uploading a missing API binary with a warning is insufficient.
5. Keep fake/network-free tests on PRs. Keep actual YouTube conversion exclusive to the manual workflow.
6. Make required checks fail on relevant compiler, formatter, and test failures. Do not count ignored static-analysis failures as passing evidence.

**Acceptance:** configure and build both Debug and Release from clean checkouts; `BUILD_API=ON` produces the API binary and HTTP tests. A missing cpprest dependency makes that requested configuration fail. Formatter parses successfully. The source archive configures in `build/` on a different absolute path. Record any supported-platform limitation explicitly.

**Commit:** `build: restore reproducible API and formatting gates`.

## R1 — Fix real ffmpeg output selection (finding 1)

**Files:** `src/core/converter.cpp`, `tests/test_converter.cpp`, `tests/fakes/ffmpeg`, new local-media integration tests, `CMakeLists.txt`.

1. Pass an explicit output muxer before the output path: `-f mp3`, `-f wav`, or `-f mp4`, based only on the validated format. Keep muxer selection separate from codec flags.
2. Generate a unique partial path per writing operation on the output filesystem. Keep atomic rename and cleanup on errors/cancellation. Preserve a previously valid final output when a replacement fails.
3. Tighten fake argv assertions so the fake's permissiveness no longer conceals a missing muxer.
4. Add integration fixtures generated entirely locally: a short tone for MP3/WAV, a compatible non-MP4 source for the copy/remux path, and a valid MP4 for the bypass path. Use real ffmpeg and ffprobe; no YouTube requests.
5. Validate codec/container, nonzero duration, expected audio properties, nonempty output, and absence of partial files. Verify already-valid MP4 does not spawn the converter ffmpeg stage.

**Acceptance:** MP3, WAV, and fallback MP4 complete with real ffmpeg. A failed replacement leaves the old final intact and removes its partial. Tests fail if the muxer is removed.

**Commit:** `fix: select ffmpeg muxers for atomic temporary outputs`.

## R2 — Authorize job status and cancellation (finding 2)

**Files:** `src/api/api_app.cpp`, `tests/test_api.cpp`, `docs/API.md`.

1. Apply the same authorization helper used by conversion submission to GET and DELETE job routes before lookup, snapshot disclosure, or cancellation.
2. Prefer a small shared route-policy helper to three divergent checks. Preserve existing deliberate unauthenticated-localhost behavior and public health/readiness policy.
3. For a configured key, missing/wrong keys return 401 regardless of whether the job exists. A correct key proceeds to normal 200/404/409 behavior.
4. Document that the single service key authorizes service jobs; this change does not introduce user accounts or per-user ownership.

**Tests:** create a valid job with a correct key; test both routes with missing, incorrect, and correct keys. Unauthorized DELETE must leave the job and child unchanged. Unauthorized GET must reveal no output path or error detail. Cover nonexistent IDs and unauthenticated-localhost mode.

**Acceptance:** protected routes consistently enforce the key, while health checks and the existing local opt-in remain compatible.

**Commit:** `fix: require configured API key for job routes`.

## R3 — Complete the Docker runtime (finding 3)

**Files:** `Dockerfile`, `docker-compose.yml`, container smoke tests, CI workflow, `README.md`.

1. Install the matching Debian runtime library for cpprestsdk and its required transitive libraries in the runtime stage. Determine the complete dependency closure from the built binary rather than guessing that one package is sufficient.
2. Require the builder to produce the API binary; remove the conditional API installation that hides an incomplete build.
3. Inspect `ldd` results for both executables in the runtime image and fail on any unresolved library. Retain a compiler-free runtime, the intentional yt-dlp pin, UID 10001, loopback default, and healthcheck.
4. Smoke-test the actual image as its configured user: CLI help/version, API startup, health/readiness, a queued fake conversion, status polling, and authorized cancellation where a key is configured.
5. Provide executable fixtures and a writable output mount owned by UID 10001 in the test. Exercise the documented Compose setup too; a root-created bind mount must not be mistaken for a code failure. Document necessary host directory ownership rather than making the service run as root.

**Acceptance:** both executables load; the service starts as UID 10001; fake conversion returns 202 then succeeds; no missing shared libraries or compiler toolchain appear in runtime. Container tests use no live YouTube URL and run as a required Linux gate.

**Commit:** `fix: install API runtime dependencies and verify container startup`.

## R4 — Protect sources through encode/publication (finding 4)

**Files:** `src/core/converter.cpp`, new cache component/header if needed, `include/converter.h`, cache/converter tests, `CMakeLists.txt`.

1. Extract cache lifecycle responsibilities from ad hoc directory scanning into a small cache manager. Give each source generation an immutable path, known size, creation time, and reader ownership.
2. Acquire a lease atomically with lookup. Keep it until encode or MP4 publication finishes, including exceptions and cancellation. Use RAII to release leases.
3. Download into a unique incoming generation. Verify it, then publish its cache metadata. Refresh must not delete the old generation first; a failed refresh must leave previous valid output/cache usable under normal policy.
4. Evict only eligible generations. Defer deletion of leased entries and account for deferred retirement. Expired entries should actually become eligible for cleanup, not merely stop satisfying reuse.
5. Establish a lock order for cache metadata, filesystem publication, and shared-operation membership. Never call child execution or queue callbacks while holding the cache's global mutex.
6. Handle two CLI processes sharing an output root. A process-local reader count alone is insufficient. Use a cross-process advisory locking/lease strategy, or create private hard-link/copy input pins under coordinated filesystem locking before releasing lookup protection. Unique immutable generations prevent refresh from modifying pinned content. Document platform-specific implementations and recovery after process death.
7. Define cache-cap behavior when all candidates are leased or the new source exceeds the cap. An oversized source may remain a transient leased input but must not be retained indefinitely. Account separately for retained cache bytes and bounded active working bytes; expose pressure instead of deleting active input.
8. Clean unique incoming/partial paths after failure. Do not remove another operation's directory. Retire older extensions/generations deterministically.

**Tests:** gate an encoder immediately before opening its source; force another download to evict; verify completion. Refresh during another encode; verify each reader uses its intended generation. Cover TTL, oversized entries, all-leased pressure, failed refresh, different extensions, exceptions, and concurrent CLI processes. Use barriers/marker handshakes rather than sleep-based race assumptions.

**Acceptance:** eviction/refresh cannot invalidate active inputs; cache capacity and transient exceptions are documented and tested; failed work releases leases and artifacts; idle eviction restores the retained-byte bound.

**Commit:** `fix: lease immutable source generations during conversion`.

## R5 — Isolate cancellation of shared work (finding 5)

**Files:** `include/job_queue.h`, `src/utils/job_queue.cpp`, `include/singleflight.h`, `src/utils/singleflight.cpp`, `src/core/converter.cpp`, `src/utils/process.cpp`, queue/converter/API tests.

1. Replace leader-ID aliasing with independent job records subscribed to a shared operation. Operation identity remains stable if the first client cancels or its history expires.
2. Give jobs individual cancellation/state and operations their own cancellation token. Pass only the operation token to a shared child process.
3. Canceling one subscriber marks only that job canceled and detaches it. Preserve the operation for remaining subscribers. Never copy a leader's canceled state over an independent follower.
4. When the final interested subscriber leaves, request operation cancellation and terminate its child group using the existing TERM → two-second grace → KILL sequence. Shutdown cancels all operations and joins executing workers.
5. Make singleflight waits cancellation-aware with notification or a bounded timed wait. A canceled waiting request must not remain blocked for the download timeout. Do not hold the membership mutex while blocking.
6. A canceled original requester may still host the bounded worker carrying out a shared operation. After publishing the shared download result, stop its own remaining pipeline if nobody still requires it; otherwise continue shared encode work for surviving subscribers. Keep the resource reservation until actual work stops.
7. Resolve completion-versus-cancellation under one state transition rule: once a job is terminal, it never becomes running/succeeded again. Cancellation acknowledged before publication must prevent that canceled job alone from creating new output; valid publication for surviving jobs remains allowed.
8. Remove finished operations safely from the flight table without erasing a newer operation for the same key. Define what a new caller does while the previous operation is canceling.

**Tests:** cancel same-format follower, cancel original client with a follower, cancel MP3 while WAV shares its download, cancel a waiting WAV, cancel every subscriber, queued cancel, cancel during encode, repeated DELETE, completion race, and shutdown. Verify surviving outputs, process counts, child termination, worker accounting, and no stuck waits.

**Acceptance:** one job's cancellation cannot cancel another interested job. Last-subscriber cancellation reaps the child. All waits and shutdown complete within defined bounds.

**Commit:** `fix: separate job cancellation from shared operation lifetime`.

## R6 — Bound subscribers and retained history (finding 6)

**Files:** queue implementation/header, config implementation/header, `.env.example`, `docs/API.md`, queue/API/config tests.

1. Define three independent limits: outstanding executable operations, live job/subscription records, and retained terminal snapshots. Check the subscription bound before allocating an attached follower.
2. Proposed configurable defaults: `YTCONV_MAX_ACTIVE_JOBS=256`, `YTCONV_JOB_HISTORY_MAX=1024`, and `YTCONV_JOB_HISTORY_TTL_SEC=3600`. Validate ranges; document that values are an initial policy subject to measured memory cost, not a benchmark result.
3. Start terminal TTL at completion/cancellation, use a monotonic clock, and prune oldest terminal snapshots deterministically at insertion and lookup/maintenance points. Enforce the maximum even when TTL has not expired.
4. On completion, discard heavyweight executable request data, callbacks, cancellation ownership, and unnecessary configuration copies. Keep compact status snapshots. Subscribers depend on an operation while active, not on a retained leader record.
5. Never prune running operations to meet a history bound. Do not hold stopped/completed children or cache leases through retained history. Snapshot sizes must also be bounded, including error strings.
6. Reject admission with 503 `busy` at the appropriate limit. Expired IDs return 404. Bound collisions/replacement behavior for generated IDs so a new submission cannot silently replace an existing job record.

**Tests:** large sequential completion workload, duplicate storm while the execution queue is full, TTL expiration using a controllable clock, count eviction, leader history removal with active followers, canceled records, and invalid config. Assert exact map/record bounds through test instrumentation; use RSS only as supplemental evidence.

**Acceptance:** retained memory scales with configured limits, not historical request count. Attached requests cannot bypass record admission. Operation state survives history pruning correctly.

**Commit:** `fix: bound live job subscriptions and retained history`.

## R7 — Preserve bounds in synchronous mode (finding 7)

**Files:** `src/api/api_app.cpp`, queue implementation/header, `tests/test_api.cpp`, `docs/API.md`.

1. Route non-reused synchronous conversions through the same queue/admission path as asynchronous requests. Remove direct unbounded converter calls from HTTP handlers.
2. Add an asynchronous completion notification for a submitted job. Keep the response outstanding without blocking a cpprest scheduler thread on `get()`, a condition variable, or worker execution.
3. A sync subscriber participates in the same cancellation and sharing lifecycle. Full admission returns 503 immediately. Shutdown resolves outstanding responses and drains callbacks before destroying their captured server state.
4. Preserve 200 success and typed error status mapping for synchronous callers. Use the same data/metrics builder as asynchronous completion. Do not implement a private second executor.
5. Preserve the normal service behavior when a client disconnects unless cancellation-on-disconnect is explicitly designed and documented; never cancel work needed by other subscribers implicitly.

**Tests:** more distinct slow sync requests than the operation cap, child-count instrumentation, duplicate sync/async subscribers, sync failure mapping, health responsiveness, body-read responsiveness, and shutdown with outstanding sync responses. Set `max_concurrent=1` to make bounds observable.

**Acceptance:** sync and async calls obey identical operation/child limits. Health remains responsive during slow sync conversions. No outstanding completion callback outlives its server state.

**Commit:** `fix: execute synchronous conversions through the bounded queue`.

## R8 — Make reuse, force, and refresh consistent (finding 8)

**Files:** converter and queue implementation, API/CLI request parsing, converter/API tests, `README.md`, `docs/API.md`.

1. Centralize completed-output reuse eligibility and use it at every early or late reuse check, including HTTP immediate reuse. Eligibility is `reuse_completed && !force && !refresh`, plus a validated usable final file.
2. Centralize source reuse and operation-coalescing policy separately. `force` may use cached source; `refresh` must require a new generation. A refresh cannot attach to an older ordinary in-flight download and pretend it refreshed.
3. Serialize writers targeting the same final path. Define ordering at admission/publication so an older operation cannot overwrite a later successful refresh. Concurrent refresh requests may share a newly started compatible refresh generation; document the exact rule.
4. Keep `reused=true` meaningful: it represents use of a completed output, not merely shared download bytes. Expose source-cache use/shared-operation information separately when needed.
5. Preserve existing output on refresh/download/encode failure. Check cancellation before final publication.

| Completed-output reuse enabled | Force | Refresh | Intended behavior |
|---|---|---|---|
| Yes | No | No | Reuse valid completed output; otherwise encode from an eligible source |
| Yes | Yes | No | Replace output; eligible source cache allowed |
| Yes | Either | Yes | Obtain a new source generation and replace output |
| No | Either | No | Encode/publish again; eligible source cache allowed |
| No | Either | Yes | Obtain a new source generation and replace output |

**Tests:** full policy matrix with/without completed output and with fresh/expired/missing source cache. Change fixture bytes or generation IDs to prove refresh replaced content. Cover same-format refresh, overlapping ordinary/force/refresh requests, and failed refresh preserving old output. Exercise CLI `--refresh` and API `refresh`/`force` independently.

**Acceptance:** no late reuse check contradicts the requested policy; content and generation assertions verify behavior, not only child invocation counts.

**Commit:** `fix: enforce consistent output and source reuse policies`.

## R9 — Bound request bodies during asynchronous extraction (finding 9)

**Files:** `src/api/api_app.cpp`, API implementation/header if lifetime helpers are needed, `tests/test_api.cpp`.

1. Retain fast rejection for a valid oversized Content-Length, but strictly validate the header and do not trust it as the memory boundary.
2. Inspect cpprestsdk's listener/streaming behavior on the supported platforms before selecting the implementation. Verify that the chosen read path does not cause the framework to buffer the entire entity before application enforcement.
3. Read at most 8,193 decoded body bytes through bounded asynchronous stream reads. The extra byte detects overflow. Only parse JSON after a complete body of at most 8,192 bytes is available.
4. Remove `extract_string().get()` from the listener callback. Keep request/server state alive until continuations finish, resolve each response once, and integrate callback draining with R7 shutdown.
5. Stop/close oversized request input using supported listener behavior. Do not attempt to drain an unlimited body merely to reuse a connection. Add an explicit bounded body-read timeout and bounded outstanding-read admission if the listener does not already enforce suitable limits; proposed body-read timeout is 10 seconds, configurable and separate from conversion timeouts.
6. If cpprestsdk itself buffers unbounded input before this layer, document that result and fix/enforce the bound at the listener boundary. Do not claim the issue solved by a post-buffer size check. A framework replacement is not the default solution.

**Tests:** exact 8,192-byte and 8,193-byte JSON entities, oversized known-length requests, chunked oversized input, slow/incomplete input, malformed lengths, valid JSON, disconnect, and shutdown mid-read. Use a raw/local HTTP client for chunked/partial traffic. Verify prompt response/connection termination, bounded retained bytes, no enqueued invalid work, and health responsiveness.

**Acceptance:** body memory and read lifetime are bounded even without Content-Length; conversion/health callbacks remain responsive and no abandoned read survives shutdown.

**Commit:** `fix: enforce request body limits while reading asynchronously`.

## R10 — Bound progress-line buffers (finding 10)

**Files:** `include/process.h`, `src/utils/process.cpp`, `tests/test_process.cpp`.

1. Add an explicit callback-line limit, default 8,192 bytes, independent of the captured-output limit. Apply it to stdout and stderr on POSIX and Windows.
2. Choose and document one deterministic overflow policy: retain the first bounded prefix, discard the remainder until the newline, then deliver at most one truncated-line callback and reset. A final unterminated line follows the same bound.
3. Keep draining child pipes during overflow; do not block the child or grow another temporary buffer. Preserve CR/LF handling and normal progress parsing.
4. Make callback cleanup exception-safe so a failing callback cannot strand a registered child or its pipe descriptors. Keep callbacks quick and avoid executing them under global process locks.
5. Use a compiled local output-writer fixture for cross-platform process tests rather than assuming shell scripts can execute on Windows.

**Tests:** multi-megabyte newline-free stdout/stderr with callbacks enabled and 8 KB capture, giant line followed by normal progress, final unterminated line, both streams, callback failure, timeout, and cancellation. Assert maximum callback payload/pending storage as well as successful child completion.

**Acceptance:** every retained output buffer has an explicit bound; verbose children complete or cancel normally on verified platforms.

**Commit:** `fix: cap pending process lines without blocking pipe drains`.

## R11 — Measure physical work once (finding 11)

**Files:** metrics implementation/header, converter/queue/singleflight data models, API JSON/logging, new metrics tests, documentation.

1. Separate physical operation counters from logical client-job counters. Emit download/encode measurements at their operation completion, not inside every subscriber's success path.
2. Define `bytes_downloaded` honestly. Final source size measures downloaded media bytes, not exact wire traffic including retries, metadata, headers, or merge overhead. Keep it as a documented approximation counted once per actual download; introduce a separately named source-size field. Do not label source filesize as exact network transfer.
3. Warm-source and completed-output reuse contribute zero new downloaded bytes. A failed encode still preserves measurements for its already-completed download. Decide and document how failed downloads account for available partial-byte evidence; use zero/unknown rather than inventing a value.
4. Count download and encode durations once per operation, including failed/canceled attempts where measured. Per-job snapshots may expose the shared operation's elapsed duration for diagnosis, but must not increment global totals again.
5. Define `bytes_written` as newly published output bytes, not bytes of a reused output; expose output size independently. Record each logical job's terminal outcome exactly once, including attached jobs and immediate reuse, with a separate canceled count.
6. Keep queued/running/terminal logical states and executing-operation gauges separate. Add cache-hit/coalesced counts if needed to explain saved work. Ensure counters/gauges cannot underflow during completion/cancel races.
7. Update the manual timing workflow, docs, and completion logs to use these definitions and field names consistently.

**Tests:** take before/after counter snapshots for cold MP3, warm WAV, completed-output reuse, concurrent same-format sharing, concurrent MP3/WAV sharing, failed encode after successful download, cancellation of one/all subscribers, and refresh. Assert exact deltas against fixture size and invocation count. Preserve backward compatibility where possible; document changes in meaning.

**Acceptance:** one physical download produces one download measurement regardless of subscribers/formats. Reuse adds zero download work. Logical outcome counts and active gauges reconcile after every scenario.

**Commit:** `fix: count shared media operations once in metrics`.

## R12 — Invalidate readiness in production (finding 12)

**Files:** `include/dependencies.h`, `src/utils/dependencies.cpp`, converter/queue error handling, dependency/API tests.

1. Introduce a production invalidation API distinct from the test-reset helper. Invoke it when conversion child execution reports `BinaryNotFound` or the relevant executable-launch failure.
2. Key readiness cache entries by the effective tool configuration (both paths and any settings affecting the probe). Avoid a process-global success for one configuration satisfying another.
3. Use a monotonically increasing generation to invalidate probes already in progress. Publish a probe result only if its generation is still current; an old success must not undo a newer missing-tool failure.
4. Coalesce simultaneous probes for a configuration and keep probe timeouts bounded. Do not hold the global cache mutex across both external version commands. Continue caching only successful results for the configured TTL.
5. Define readiness as a current advisory probe: invalidation forces the next check to re-probe immediately, not a permanent unhealthy state. A restored tool may make readiness succeed again. Keep health independent of tool spawning.

**Tests:** successful readiness, remove a fixture executable, trigger a conversion launch failure, then readiness inside the old TTL must re-probe and fail. Restore the tool and verify recovery. Cover alternate configurations, simultaneous probes, failed probes, zero TTL, and invalidation during a blocked successful probe.

**Acceptance:** missing tools cannot leave stale cached success after a conversion failure; stale in-flight results cannot republish invalidated state; health remains responsive.

**Commit:** `fix: invalidate readiness cache after executable failures`.

## G1 — Final documentation and merge gates

Update `README.md`, `docs/API.md`, `docs/ARCHITECTURE.md`, and `.env.example` in the same packages that change their contracts. Explain auth on job routes, job-history expiration, admission limits, sync behavior, force/refresh policy, cache pressure/leases, and measurement semantics. Correct the old plan's completion claims with links to the corresponding tests/evidence; preserve its historical audit rather than silently rewriting it as verified.

### Required evidence matrix

| Gate | Required evidence |
|---|---|
| Clean checkout | Build from a tracked-file export/new checkout in a different absolute directory; no generated files in Git |
| Core and API | Debug/Release on supported Linux/macOS matrix; API explicitly built; HTTP tests actually executed |
| Formatting/warnings | Valid formatter configuration, affected files formatted, required compiler/static checks evaluated |
| Real media | Network-free real-ffmpeg MP3/WAV/remux/MP4-bypass tests with ffprobe validation |
| Authorization | Correct/missing/wrong key matrix on POST, GET job, DELETE job |
| Shared work | Deterministic cancellation, eviction, refresh, retention, and publication race tests |
| Resource bounds | Measured/test-instrumented limits for operations, children, job records, source leases, request bytes, and process buffers |
| Shutdown | Reject submissions once stopping; cancel/join operations; resolve HTTP continuations; reap children and release leases |
| Metrics/readiness | Exact fixture-counter deltas and missing-tool invalidation/recovery tests |
| Docker | Actual runtime image as UID 10001, no unresolved libraries/compiler toolchain, successful fake conversion and healthcheck |
| Sanitizers | Targeted ASan/UBSan and, where supported, TSan over shared-operation/cache/queue races; investigate relevant reports |
| Windows claims | Native process fixture verifies bounded pipes, callback lines, and cancellation if Windows support remains claimed; otherwise explicitly state unverified scope |
| Timing | Manual operator-supplied conversion logs stage timings and validates nonempty output; no live YouTube traffic in PR tests |
| Re-review | Review final diff against current origin/main and confirm no unresolved P1/P2 defects or required skipped gates |

Run deterministic unit tests while implementing each package, then the appropriate integration checks. Avoid retrying flaky races until green: use explicit synchronization, bounded timeouts, and preserved failure logs. Optional comparative performance claims require repeatable before/after measurements of the same input; do not infer a speedup from a faster HTTP acknowledgment.

### Final implementation checklist

- [x] G0: reproducible checkout, valid formatter, mandatory requested API build. (`2c14bfd`; macOS leg pending CI)
- [x] R1: real ffmpeg muxer/output regression fixed. (`5af9fcd`)
- [x] R2: job routes enforce authorization. (`86acf1f`)
- [ ] R3: runtime image dependency closure/startup verified. (`371db1e`; both stages reproduced in a bookworm rootfs and the in-container smoke passes as UID 10001; real `docker build`, HEALTHCHECK, and Compose pending the CI `container` job)
- [x] R4: active sources survive eviction and refresh. (`1da88d8`)
- [x] R5: independently cancelable subscribers and bounded operation shutdown. (`e6a258e`)
- [x] R6: active subscribers and terminal history bounded. (`e6a258e`)
- [x] R7: synchronous mode obeys the same execution limits. (`d0faf54`)
- [x] R8: all reuse/force/refresh cases verified. (`1a7cae3`)
- [x] R9: bounded async request reading, including chunked/slow bodies. (`fa30147`)
- [x] R10: bounded callback lines on verified process implementations. (`a52e1be`; POSIX verified, Windows unsupported)
- [x] R11: physical work counted once and metric meanings documented. (`1564c0d`)
- [x] R12: production readiness invalidation/recovery verified. (`470b874`)
- [ ] G1: docs aligned, all required evidence attached to the final commit, independent re-review complete. (docs and local evidence in `docs/MERGE_READINESS.md`; pending CI run, manual timing run, and independent re-review)

Suggested rollout is small reviewable commits in the dependency order above. Keep concurrency at a conservative value during validation and increase it only after sharing, cancellation, cache lifetime, and admission checks pass together. The final readiness score should be assigned from that verified implementation, not from this document's ambition.
