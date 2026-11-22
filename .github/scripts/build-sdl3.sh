#!/usr/bin/env bash
# Build and install SDL3 into a prefix, for CI runners without an SDL3 package (Ubuntu 24.04).
# The version is the GIT_TAG of the FetchContent fallback in the top-level CMakeLists.txt, so
# CI and the fallback stay on the same SDL.
#
#   .github/scripts/build-sdl3.sh <install-prefix> [extra SDL cmake args...]
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=<install-prefix>
set -euo pipefail

prefix=${1:?usage: build-sdl3.sh <install-prefix> [cmake args...]}
shift
root=$(cd "$(dirname "$0")/../.." && pwd)
tag=$(sed -n 's/.*GIT_TAG \(release-[0-9.]*\).*/\1/p' "$root/CMakeLists.txt" | head -n 1)
if [ -z "$tag" ]; then
  echo "cannot find the SDL3 GIT_TAG in CMakeLists.txt" >&2
  exit 1
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
git clone --quiet --depth 1 --branch "$tag" https://github.com/libsdl-org/SDL.git "$work/SDL"
cmake -S "$work/SDL" -B "$work/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DSDL_SHARED=ON -DSDL_STATIC=OFF \
  -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF "$@"
cmake --build "$work/build" --parallel
cmake --install "$work/build"
echo "SDL3 $tag installed in $prefix"
