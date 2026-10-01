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
| `test_process` | argv spawn, missing binary, non-zero exit, timeout kill |
| `test_converter` | fake download/convert, temp cleanup, reuse, error mapping, audio vs video argv |
| `test_security_argv` | reconstructed URL, `--no-playlist`, safe filenames |
| `test_api` | POST contract, 404/405/401, health/ready/metrics (needs cpprestsdk) |
| `test_error` / `test_config` / `test_logger` | mappings, env guards, concurrent log lines |
| `no_system_in_src` | `ctest` + CI fail if `system(` or `popen(` returns in `src/` |
| `cli_help` / `cli_version` | `--help` and `--version` exit 0 |

Set `YTCONV_YT_DLP` and `YTCONV_FFMPEG` to the fake scripts only in tests. Production binaries look up real tools on `PATH`.

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
