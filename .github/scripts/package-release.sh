#!/usr/bin/env bash
# Stage and archive a release build: the two binaries, any SDL3 runtime library the build
# produced (none when SDL3 is linked statically, as release.yml does), the README, and the
# vendored original simulator beside the binaries, where both frontends look for it.
# No games are included.
#
#   .github/scripts/package-release.sh <build-dir> <out-dir> <package-name>
#
# Writes <out-dir>/<package-name>.zip on Windows and <out-dir>/<package-name>.tar.gz elsewhere.
set -euo pipefail

build=${1:?usage: package-release.sh <build-dir> <out-dir> <package-name>}
out=${2:?usage: package-release.sh <build-dir> <out-dir> <package-name>}
name=${3:?usage: package-release.sh <build-dir> <out-dir> <package-name>}
root=$(cd "$(dirname "$0")/../.." && pwd)

exe=""
if [ -f "$build/exen-run.exe" ]; then
  exe=".exe"
fi
for bin in exen-reloaded exen-run; do
  if [ ! -f "$build/$bin$exe" ]; then
    echo "missing $build/$bin$exe (was the SDL frontend built?)" >&2
    exit 1
  fi
done

mkdir -p "$out"
out=$(cd "$out" && pwd)
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
dir="$stage/$name"
mkdir -p "$dir"

cp "$build/exen-reloaded$exe" "$build/exen-run$exe" "$dir/"
# SDL3 runtime, if the build linked it dynamically (FetchContent builds it static by default)
find "$build" -maxdepth 3 \( -name 'SDL3.dll' -o -name 'libSDL3*.so*' -o -name 'libSDL3*.dylib' \) \
  -exec cp -P {} "$dir/" \;
cp "$root/README.md" "$dir/"
# libretro core and its .info (install into the frontend's cores/ and info/ directories)
for core in "$build"/exen_reloaded_libretro.dll "$build"/exen_reloaded_libretro.so "$build"/exen_reloaded_libretro.dylib; do
  [ -f "$core" ] && mkdir -p "$dir/libretro" && cp "$core" "$build/exen_reloaded_libretro.info" "$dir/libretro/"
done
[ -f "$root/docs/LIBRETRO.md" ] && cp "$root/docs/LIBRETRO.md" "$dir/libretro/" 2>/dev/null || true
cp "$root/vendor/gensimu/gensimu.exe" "$dir/"
# default phone settings (nickname, currency, SMS prices) for new saves; read beside gensimu.exe
mkdir -p "$dir/flash"
cp "$root/vendor/gensimu/flash/eeprom.dat" "$dir/flash/"
cp "$root/vendor/gensimu/PROVENANCE.md" "$dir/gensimu-PROVENANCE.md"
if [ -f "$root/LICENSE" ]; then
  cp "$root/LICENSE" "$dir/"
fi

echo "== $name"
ls -l "$dir"
echo "== runtime dependencies"
case "$(uname -s)" in
  Linux*) ldd "$dir/exen-reloaded" || true ;;
  Darwin*) otool -L "$dir/exen-reloaded" || true ;;
  MINGW*|MSYS*|CYGWIN*) objdump -p "$dir/exen-reloaded$exe" | grep 'DLL Name' || true ;;
esac

# Smoke test the package: exen-run must find gensimu.exe beside itself (no --sim).
"$dir/exen-run$exe" --frames 30 --quiet "$root/vendor/gensimu/flash/download1.exn" > "$stage/smoke.txt"
grep -q "status: running" "$stage/smoke.txt"
echo "== package smoke test passed"

cd "$stage"
if [ -n "$exe" ]; then
  rm -f "$out/$name.zip"
  zip -qr "$out/$name.zip" "$name"
  echo "wrote $out/$name.zip"
else
  tar -czf "$out/$name.tar.gz" "$name"
  echo "wrote $out/$name.tar.gz"
fi
