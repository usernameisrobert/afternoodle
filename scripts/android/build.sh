#!/bin/sh
# ============================================================================
#  afternoodle engine  --  Android arm64 build (UNRELEASED)
# ============================================================================
#  Builds the engine for arm64-v8a using the NDK toolchain. This is not part
#  of the release matrix yet: there is no Activity or JNI glue, so the output
#  is a native static library and shared library for an app to link against.
#
#  With an NDK installed:
#     ANDROID_NDK=/path/to/ndk sh scripts/android/build.sh
#
#  On a Termux device the NDK is not packaged, so the script falls back to the
#  device's own aarch64-linux-android toolchain, which targets the same ABI:
#     sh scripts/android/build.sh
#
#  Output lands in build/android/ and dist/android/.
# ============================================================================
set -eu

API="${ANDROID_API:-24}"
ABI="${ANDROID_ABI:-arm64-v8a}"
OUT="build/android"
DIST="dist/android"

case "$ABI" in
  arm64-v8a)   TRIPLE="aarch64-linux-android" ;;
  armeabi-v7a) TRIPLE="armv7a-linux-androideabi" ;;
  x86_64)      TRIPLE="x86_64-linux-android" ;;
  *) echo "error: unsupported ABI '$ABI'" >&2; exit 1 ;;
esac
TARGET="$TRIPLE$API"

# Prefer the NDK, since it is the only way to target an ABI other than the
# host. Fall back to the Termux toolchain, which is already the Android ABI.
# command -v prints a full path, which matters because the compiler may not be
# on the search path the script itself is running under.
NDK="${ANDROID_NDK:-${ANDROID_NDK_ROOT:-}}"
if [ -n "$NDK" ] && [ -d "$NDK/toolchains/llvm/prebuilt" ]; then
  HOST="$(ls "$NDK/toolchains/llvm/prebuilt" 2>/dev/null | head -1)"
  BIN="$NDK/toolchains/llvm/prebuilt/$HOST/bin"
  CC="$BIN/${TRIPLE}${API}-clang"
  AR="$BIN/llvm-ar"
  STRIP="$BIN/llvm-strip"
  TOOLCHAIN_NAME="ndk ($HOST)"
else
  CC="$(command -v "${TRIPLE}${API}-clang" || command -v "${TRIPLE}-gcc" || true)"
  if [ -z "$CC" ]; then
    echo "error: no Android toolchain. Install the NDK and set ANDROID_NDK," >&2
    echo "       or run this on a device with the Termux toolchain." >&2
    exit 1
  fi
  BINDIR="$(dirname "$CC")"
  AR="$(command -v llvm-ar || echo "$BINDIR/llvm-ar")"
  STRIP="$(command -v llvm-strip || echo "$BINDIR/llvm-strip")"
  TOOLCHAIN_NAME="termux"
fi

if [ ! -x "$CC" ]; then
  echo "error: compiler not found: $CC" >&2
  exit 1
fi

SRC_DIRS="src/core src/platform src/render src/ecs src/physics src/audio src/script src/vsl src/app"
CORE_SRC=""
for d in $SRC_DIRS; do
  [ -d "$d" ] || continue
  for f in "$d"/*.c; do
    [ -e "$f" ] && CORE_SRC="$CORE_SRC $f"
  done
done

# SDL2 headers. Point ANDROID_SDL_PREFIX at an SDL2 built for Android when
# cross-compiling; on a Termux device the system SDL2 is already correct.
if [ -n "${ANDROID_SDL_PREFIX:-}" ]; then
  SDL_INC="-I${ANDROID_SDL_PREFIX}/include -I${ANDROID_SDL_PREFIX}/include/SDL2"
else
  SDL_INC="$(pkg-config --cflags sdl2 SDL2_image SDL2_ttf 2>/dev/null || true)"
  if [ -z "$SDL_INC" ]; then
    echo "error: no SDL2 headers found. Install SDL2, or set ANDROID_SDL_PREFIX" >&2
    echo "       to an SDL2 built for $ABI." >&2
    exit 1
  fi
fi

CFLAGS="-O2 -std=c11 -fno-strict-aliasing -fPIC -fvisibility=hidden
  -Iinclude -Isrc -DAFNDLE_BUILDING $SDL_INC
  -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers"

echo "==> android: ABI=$ABI API=$API toolchain=$TOOLCHAIN_NAME target=$TARGET"

OBJS=""
for f in $CORE_SRC; do
  o="$OUT/$ABI/$(echo "$f" | tr '/' '_' | sed 's/\.c$/.o/')"
  mkdir -p "$(dirname "$o")"
  "$CC" $CFLAGS -target "$TARGET" -c "$f" -o "$o"
  OBJS="$OBJS $o"
done

LIB="$OUT/libafndle.a"
mkdir -p "$OUT"
rm -f "$LIB"
"$AR" rcs "$LIB" $OBJS
echo "    static  $LIB"

# A shared library needs every af_ symbol to stay visible, so override the
# -fvisibility=hidden used for the static build. Link the objects directly:
# adding the archive too would define every symbol twice.
SHARED="$OUT/libafndle.so"
"$CC" $CFLAGS -target "$TARGET" -shared -fvisibility=default -o "$SHARED" $OBJS
"$STRIP" --strip-unneeded "$SHARED"
echo "    shared  $SHARED"

mkdir -p "$DIST/$ABI"
cp "$LIB" "$DIST/$ABI/"
cp "$SHARED" "$DIST/$ABI/"
cp -r include "$DIST/$ABI/include"

# A test binary, built against the Termux SDL2 when it is available. It links
# the device's own SDL2 rather than shipping one, so it only runs under
# Termux; it exists to prove the engine works on arm64 Android.
# A test binary, linked against the device's SDL2. It needs the same SDL2 at
# run time, so it is for checking the engine on arm64 Android, not for
# redistribution.
SDL_CFLAGS="$SDL_INC"
SDL_LIBS="$(pkg-config --libs sdl2 SDL2_image SDL2_ttf 2>/dev/null || true)"
if [ -n "$SDL_LIBS" ]; then
  "$CC" -O2 -std=c11 -Iinclude $CFLAGS $SDL_CFLAGS \
    tests/test_engine.c "$LIB" -o "$DIST/$ABI/afndle-tests" $SDL_LIBS -lm
  echo "    tests   $DIST/$ABI/afndle-tests"
else
  echo "    (no SDL2 found, skipping the test binary)"
fi

printf 'afternoodle engine\nandroid-%s\nunreleased\n' "$ABI" > "$DIST/manifest.txt"
echo "==> done -> $DIST/"
