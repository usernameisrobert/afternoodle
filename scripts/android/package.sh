#!/bin/sh
# Bundles the Android build into a tarball for sideloading. Not released.
set -eu
ABI="${ANDROID_ABI:-arm64-v8a}"
VER="$(git -C "$(dirname "$0")/../.." rev-parse --short HEAD 2>/dev/null || echo dev)"
OUT="dist/afternoodle-android-${ABI}-${VER}.tar.gz"
mkdir -p dist
tar czf "$OUT" -C dist/android "$ABI" manifest.txt
echo "$OUT"
