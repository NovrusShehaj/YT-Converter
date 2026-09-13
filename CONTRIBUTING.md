# Contributing to YT-Converter

This repository is a C++17 CLI and localhost API. Please keep that scope: no web UI, database, Redis, Kubernetes, or public unauthenticated service.

## Code of Conduct

See [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md). Report issues at https://github.com/NovrusShehaj/YT-Converter/issues.

## Development

```sh
git clone https://github.com/NovrusShehaj/YT-Converter.git
cd YT-Converter
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/yt2mp3-cli --help
```

CMake 3.16+ is required. cpprestsdk is required only for `yt2mp3-api`.

## Style

The code is C++17, K&R braces, 4-space indent, as enforced by `.clang-format`. Run `clang-format -i` on files you touch. Do not add `system()` or `popen()`. Do not concatenate untrusted strings into a shell command.

## Tests

New URL shapes, process flags, or API status codes need a regression test. Prefer fake binaries under `tests/fakes/` over live YouTube.

## Pull requests

- Use a feature branch.
- Update README / docs/API.md if the CLI or HTTP contract changes.
- Do not commit `.env`, keys, cookies, or media files.
- CI runs on every branch and pull request.
