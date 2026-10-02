#!/bin/sh
# Runtime-image gate: every binary must resolve all shared libraries, and no compiler toolchain
# may be present. Prints the ldd results as build evidence.
set -eu
status=0
for binary in "$@"; do
  echo "== ldd $binary"
  ldd "$binary"
  if ldd "$binary" | grep -q 'not found'; then
    echo "Unresolved shared library in $binary" >&2
    status=1
  fi
done
for tool in cc c++ gcc g++ clang clang++ ld make; do
  if command -v "$tool" >/dev/null 2>&1; then
    echo "Compiler toolchain present in runtime image: $tool" >&2
    status=1
  fi
done
exit "$status"
