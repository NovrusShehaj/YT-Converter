#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

echo "YT-Converter quick test"
echo

missing=()
for tool in cmake; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    missing+=("$tool")
  fi
done
if ! command -v g++ >/dev/null 2>&1 && ! command -v clang++ >/dev/null 2>&1; then
  missing+=("c++ compiler")
fi
if [ "${#missing[@]}" -gt 0 ]; then
  echo "Missing build tools: ${missing[*]}"
  exit 1
fi

echo "Configuring (BUILD_TESTS=ON)"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON
echo "Building"
cmake --build build --parallel
echo "Testing"
ctest --test-dir build --output-on-failure

echo
echo "CLI help:"
./build/yt2mp3-cli --help | head -20

echo
echo "Optional live conversion (not run by this script):"
echo "  ./build/yt2mp3-cli --output-dir ./output \"https://www.youtube.com/watch?v=VIDEO_ID\" mp3"
echo
