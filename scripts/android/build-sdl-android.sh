#!/usr/bin/env bash
# ============================================================================
#  afternoodle engine  --  SDL2 stack for Android
# ============================================================================
#  Builds the SDL2 libraries the engine needs as static archives for one ABI,
#  into a self-contained prefix:
#
#      <out>/include/...        headers
#      <out>/lib/lib*.a         static archives
#
#  Why this exists: Termux's own SDL2 links libX11, libwayland, xkbcommon and
#  libdecor, none of which exist in an ordinary Android app, so its libSDL2.so
#  cannot be packaged. An APK needs SDL2 built for Android. Linking it
#  statically means libafndle.so depends on nothing but libc and libdl, so the
#  APK ships a single native library and there is no ABI matching to do for
#  SDL's own .so files.
#
#  Only what the engine actually references is built. It uses no SDL2_mixer
#  symbols at all, so mixer is not here. Of SDL2_image's formats only PNG is
#  enabled: the engine loads and saves PNGs, and PNG save is the reason libpng
#  and zlib have to be here. Image loading is left to SDL2_image's bundled stb
#  where that suffices, but IMG_SavePNG is only defined when libpng is linked,
#  so libpng is not optional.
#
#  Needs: the Android NDK, cmake, ninja and curl. Intended to run in CI, which
#  has all four. Results are cacheable -- nothing here depends on the engine,
#  so one prefix serves every build until the pinned versions move.
#
#  Usage:
#      ANDROID_NDK=/path/to/ndk scripts/android/build-sdl-android.sh <out-dir>
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

# Absolute: the sub-project builds run cmake from inside a build directory,
# where a relative source path would not resolve.
OUT="${1:-$ROOT/build/android-sdl}"
ABI="${ANDROID_ABI:-arm64-v8a}"
API="${ANDROID_API:-24}"

# Pinned so a rebuild is reproducible and a cached prefix stays valid.
SDL_VERSION="2.32.10"
SDL_TTF_VERSION="2.24.0"
SDL_IMAGE_VERSION="2.8.12"
Freetype_VERSION="2.13.3"
ZLIB_VERSION="1.3.1"
LIBPNG_VERSION="1.6.48"

SDL_URL="https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VERSION}"
SDL_TTF_URL="https://github.com/libsdl-org/SDL_ttf/releases/download/release-${SDL_TTF_VERSION}"
SDL_IMAGE_URL="https://github.com/libsdl-org/SDL_image/releases/download/release-${SDL_IMAGE_VERSION}"
GITHUB="https://github.com"

say() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

for t in cmake ninja curl find; do
  command -v "$t" >/dev/null 2>&1 || die "$t is required (not found)"
done

# The NDK is not always in the same place, so accept the usual spellings.
NDK="${ANDROID_NDK:-${ANDROID_NDK_ROOT:-}}"
if [ -z "$NDK" ] || [ ! -d "$NDK" ]; then
  for c in "$ANDROID_HOME/ndk"/* "$ANDROID_SDK_ROOT/ndk"/* "$HOME/Android/Sdk/ndk"/*; do
    if [ -d "$c" ]; then NDK="$c"; break; fi
  done
fi
[ -n "$NDK" ] && [ -d "$NDK" ] || die "no NDK found; set ANDROID_NDK"
say "NDK: $NDK"

case "$ABI" in
  arm64-v8a)   TRIPLE="aarch64-linux-android" ;;
  armeabi-v7a) TRIPLE="armv7a-linux-androideabi" ;;
  x86_64)      TRIPLE="x86_64-linux-android" ;;
  *) die "unsupported ABI '$ABI'" ;;
esac

# A prebuilt NDK ships clang wrappers named after the target triple.
HOST_BIN="$(ls -d "$NDK"/toolchains/llvm/prebuilt/*/bin 2>/dev/null | head -1)"
[ -n "$HOST_BIN" ] || die "no prebuilt toolchain under $NDK/toolchains/llvm"
CC="$HOST_BIN/${TRIPLE}${API}-clang"
[ -x "$CC" ] || die "no compiler $CC"
say "target: $TRIPLE$API"

WORK="$ROOT/build/android-sdl-work"
DL="$WORK/src"
BUILD="$WORK/build"
mkdir -p "$DL" "$BUILD" "$OUT/lib" "$OUT/include"

# unpack <name> <url>
#
# Extracts the archive and renames its one top-level entry to $DL/<name>.
# Release tarballs unpack to SDL-2.32.10, but GitHub's own tarballs unpack to
# something like freetype-freetype-534ad34, a shortened commit hash. Normalising
# here means no caller has to know which kind of archive it got.
unpack() {
  local name="$1" url="$2" dir="$DL/$1"
  [ -d "$dir" ] && return 0

  local arc="$DL/.archive-$(basename "$url")"
  say "fetch $name"
  curl -fsSL --retry 3 -o "$arc" "$url" || die "could not download $url"

  rm -rf "$DL/.stage"
  mkdir -p "$DL/.stage"
  case "$arc" in
    *.tar.gz|*.tgz) tar xzf "$arc" -C "$DL/.stage" ;;
    *.tar.xz)       tar xJf "$arc" -C "$DL/.stage" ;;
    *.zip)          unzip -q "$arc" -d "$DL/.stage" ;;
    *) die "unhandled archive kind: $url" ;;
  esac
  rm -f "$arc"

  local n root
  n="$(find "$DL/.stage" -mindepth 1 -maxdepth 1 -type d | wc -l)"
  [ "$n" -eq 1 ] || die "$name: expected one root directory in the archive, found $n"
  root="$(find "$DL/.stage" -mindepth 1 -maxdepth 1 -type d)"
  mv "$root" "$dir"
  rm -rf "$DL/.stage"
}

unpack SDL        "$SDL_URL/SDL2-$SDL_VERSION.tar.gz"
unpack SDL_ttf    "$SDL_TTF_URL/SDL2_ttf-$SDL_TTF_VERSION.tar.gz"
unpack SDL_image  "$SDL_IMAGE_URL/SDL2_image-$SDL_IMAGE_VERSION.tar.gz"
# freetype tags its releases with dashes, not dots: VER-2-13-3, not VER-2.13.3.
unpack freetype   "$GITHUB/freetype/freetype/archive/refs/tags/VER-${Freetype_VERSION//./-}.tar.gz"
unpack zlib       "$GITHUB/madler/zlib/releases/download/v$ZLIB_VERSION/zlib-$ZLIB_VERSION.tar.gz"
unpack libpng     "$GITHUB/pnggroup/libpng/archive/refs/tags/v$LIBPNG_VERSION.tar.gz"

# Every sub-project is an Android static build.
run_cmake() {
  local name="$1"; shift
  cmake -S "$DL/$name" -B "$BUILD/$name" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="android-$API" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    "$@"
}

# Flattens the archive into the prefix, which is what the engine's link line
# expects: -L<prefix>/lib -l<name>, with no per-subdirectory structure.
install_lib() {
  local built="$1" outname="$2"
  [ -f "$built" ] || die "expected $built to have been built"
  cp "$built" "$OUT/lib/$outname"
  say "  $outname  $(du -h "$OUT/lib/$outname" | cut -f1)"
}

# ---------------------------------------------------------------- zlib
say "zlib $ZLIB_VERSION"
if [ ! -f "$OUT/lib/libz.a" ]; then
  cmake -S "$DL/zlib" -B "$BUILD/zlib" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DZLIB_BUILD_EXAMPLES=OFF >/dev/null
  cmake --build "$BUILD/zlib" >/dev/null
  install_lib "$BUILD/zlib/libz.a" libz.a
fi

# ---------------------------------------------------------------- freetype
# SDL2_ttf needs freetype. HarfBuzz, brotli, bzip2 and the image decoders are
# off: the engine only renders text and gains nothing from them.
say "freetype $Freetype_VERSION"
if [ ! -f "$OUT/lib/libfreetype.a" ]; then
  run_cmake freetype \
    -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON \
    -DFT_REQUIRE_ZLIB=ON \
    -DFT_DISABLE_PNG=ON -DFT_DISABLE_JPEG=ON -DFT_DISABLE_TIFF=ON \
    -DBUILD_SHARED_LIBS=OFF
  cmake --build "$BUILD/freetype" --target freetype >/dev/null
  install_lib "$BUILD/freetype/libfreetype.a" libfreetype.a
  # freetype installs under include/freetype2; match that layout so that
  # ft2build.h is found by both SDL2_ttf and the engine.
  rm -rf "$OUT/include/freetype2"
  cp -r "$DL/freetype/include" "$OUT/include/freetype2"
fi

# ---------------------------------------------------------------- libpng
# zlib comes from the prefix, so PNG and SDL2_image share one copy of it.
say "libpng $LIBPNG_VERSION"
if [ ! -f "$OUT/lib/libpng16.a" ]; then
  run_cmake libpng \
    -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
    -DPNG_FRAMEWORK=OFF \
    -DZLIB_ROOT="$OUT" -DZLIB_LIBRARY="$OUT/lib/libz.a" \
    -DZLIB_INCLUDE_DIR="$OUT/include" \
    -DCMAKE_PREFIX_PATH="$OUT"
  cmake --build "$BUILD/libpng" --target png_static >/dev/null
  install_lib "$BUILD/libpng/libpng16.a" libpng16.a
  cp "$DL/libpng/png.h" "$DL/libpng/pngconf.h" "$OUT/include/"
fi

# ---------------------------------------------------------------- SDL2
# The engine needs the dummy video driver, which needs no windowing system at
# all, so X11 and Wayland are compiled out. That is also what keeps the archive
# free of the libraries that made Termux's SDL2 unpackagable.
say "SDL2 $SDL_VERSION"
if [ ! -f "$OUT/lib/libSDL2.a" ]; then
  run_cmake SDL \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON \
    -DSDL_TEST=OFF -DSDL_TESTS=OFF -DSDL_INSTALL_TESTS=OFF \
    -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_OFFSCREEN=OFF \
    -DSDL_DUMMYVIDEO=ON \
    -DSDL_VIDEO=ON -DSDL_AUDIO=OFF \
    -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VULKAN=OFF \
    -DSDL_PIPEWIRE=OFF -DSDL_PULSEAUDIO=OFF -DSDL_ALSA=OFF \
    -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_LIBSYSTEM=OFF
  cmake --build "$BUILD/SDL" --target SDL2-static >/dev/null
  install_lib "$BUILD/SDL/libSDL2.a" libSDL2.a
  rm -rf "$OUT/include/SDL2"
  mkdir -p "$OUT/include/SDL2"
  cp -r "$DL/SDL/include/SDL2/." "$OUT/include/SDL2/"
fi

# ---------------------------------------------------------------- SDL2_ttf
say "SDL2_ttf $SDL_TTF_VERSION"
if [ ! -f "$OUT/lib/libSDL2_ttf.a" ]; then
  # 2.24 uses the standard BUILD_SHARED_LIBS and does not vendor its
  # dependencies by default once they are found, so name them explicitly.
  run_cmake SDL_ttf \
    -DBUILD_SHARED_LIBS=OFF \
    -DSDL2TTF_SAMPLES=OFF -DSDL2TTF_TESTS=OFF -DSDL2TTF_INSTALL=OFF \
    -DSDL2TTF_VENDORED=OFF -DSDL2TTF_HARFBUZZ=OFF \
    -DFREETYPE_LIBRARY="$OUT/lib/libfreetype.a" \
    -DFREETYPE_INCLUDE_DIRS="$OUT/include/freetype2" \
    -DCMAKE_PREFIX_PATH="$OUT"
  cmake --build "$BUILD/SDL_ttf" --target SDL2_ttf >/dev/null
  install_lib "$BUILD/SDL_ttf/libSDL2_ttf.a" libSDL2_ttf.a
  cp "$DL/SDL_ttf/include/SDL_ttf.h" "$OUT/include/"
fi

# ---------------------------------------------------------------- SDL2_image
# PNG only, with saving enabled. SDL2IMAGE_PNG_SAVE is what defines
# IMG_SavePNG, which af_texture_save_png() calls, so it is the reason libpng
# is in this list at all. DEPS_SHARED=OFF links libpng and zlib in rather than
# dlopen-ing them, since the APK ships no other .so files.
say "SDL2_image $SDL_IMAGE_VERSION"
if [ ! -f "$OUT/lib/libSDL2_image.a" ]; then
  run_cmake SDL_image \
    -DBUILD_SHARED_LIBS=OFF \
    -DSDL2IMAGE_TESTS=OFF -DSDL2IMAGE_SAMPLES=OFF -DSDL2IMAGE_INSTALL=OFF \
    -DSDL2IMAGE_VENDORED=OFF -DSDL2IMAGE_DEPS_SHARED=OFF \
    -DSDL2IMAGE_PNG=ON -DSDL2IMAGE_PNG_SAVE=ON -DSDL2IMAGE_PNG_SHARED=OFF \
    -DSDL2IMAGE_JPG=OFF -DSDL2IMAGE_JPG_SAVE=OFF \
    -DSDL2IMAGE_TIF=OFF -DSDL2IMAGE_WEBP=OFF -DSDL2IMAGE_JXL=OFF \
    -DSDL2IMAGE_AVIF=OFF -DSDL2IMAGE_SVG=OFF -DSDL2IMAGE_XPM=OFF \
    -DSDL2IMAGE_XCF=OFF -DSDL2IMAGE_GIF=OFF -DSDL2IMAGE_LBM=OFF \
    -DSDL2IMAGE_PCX=OFF -DSDL2IMAGE_PNM=OFF -DSDL2IMAGE_QOI=OFF \
    -DSDL2IMAGE_TGA=OFF -DSDL2IMAGE_BMP=OFF \
    -DZLIB_ROOT="$OUT" -DZLIB_LIBRARY="$OUT/lib/libz.a" \
    -DZLIB_INCLUDE_DIR="$OUT/include" \
    -DPNG_LIBRARY="$OUT/lib/libpng16.a" \
    -DPNG_INCLUDE_DIR="$OUT/include" \
    -DCMAKE_PREFIX_PATH="$OUT"
  cmake --build "$BUILD/SDL_image" --target SDL2_image >/dev/null
  install_lib "$BUILD/SDL_image/libSDL2_image.a" libSDL2_image.a
  cp "$DL/SDL_image/include/SDL_image.h" "$OUT/include/"
fi

say "done -> $OUT"
ls -la "$OUT/lib"
