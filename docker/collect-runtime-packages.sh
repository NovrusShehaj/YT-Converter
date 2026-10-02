#!/bin/sh
# Prints the Debian packages that provide every shared library the given binaries load
# (the full ldd closure, including transitive libraries), one per line. Fails if any library is
# unresolved. Run in the builder stage; the runtime stage installs exactly this list.
set -eu
if [ "$#" -eq 0 ]; then
  echo "usage: $0 BINARY..." >&2
  exit 2
fi
libs=$(mktemp)
trap 'rm -f "$libs"' EXIT
for binary in "$@"; do
  if ldd "$binary" | grep -q 'not found'; then
    echo "Unresolved shared library in $binary:" >&2
    ldd "$binary" >&2
    exit 1
  fi
  # "libfoo.so.1 => /lib/x86_64-linux-gnu/libfoo.so.1 (0x...)" and "/lib64/ld-linux-x86-64.so.2 (0x...)"
  ldd "$binary" | awk '/=> \// { print $3 } /^[[:space:]]*\// { print $1 }' >> "$libs"
done
packages=$(mktemp)
trap 'rm -f "$libs" "$packages"' EXIT
# No pipeline around the loop: a failure must stop this script, not just a subshell.
for lib in $(sort -u "$libs"); do
  real=$(readlink -f "$lib")
  # dpkg records the path the package installed, which may be the /usr-merged or the
  # unmerged spelling of the same file.
  owner=$(dpkg -S "$lib" 2>/dev/null || dpkg -S "$real" 2>/dev/null ||
          dpkg -S "$(echo "$real" | sed 's#^/usr##')" 2>/dev/null || true)
  if [ -z "$owner" ]; then
    echo "No Debian package owns $lib ($real)" >&2
    exit 1
  fi
  echo "$owner" | head -n 1 | cut -d: -f1 >> "$packages"
done
sort -u "$packages"
