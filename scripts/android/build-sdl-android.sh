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
#  APK ships a single native library and there is no ABI-matching to do for
#  SDL's own .so files.
#
#  Only what the engine actually references is built. It uses no SDL2_mixer
#  symbols at all, so mixer is not here. PNG support is required because the
#  engine loads and saves PNGs, which pulls in zlib and libpng.
#
#  Needs: the Android NDK, cmake, ninja, and git. Intended to run in CI, which
#  has all four. Results are cacheable -- nothing here depends on the engine,
#  so the same prefix serves every build until the pinned versions move.
#
#  Usage:
#      ANDROID_NDK=/path/to/ndk scripts/android/build-sdl-android.sh <out-dir>
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

# Absolute: the sub-project builds below run cmake from inside a build
# directory, where a relative source path would not resolve.
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

MIRROR="https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VERSION}"
GITHUB_MIRROR="https://github.com"

say() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

for t in cmake ninja git; do
  command -v "$t" >/dev/null 2>&1 || die "$t is required (not found)"
done

# The NDK is not always at a fixed place, so accept the usual spellings.
NDK="${ANDROID_NDK:-${ANDROID_NDK_ROOT:-}}"
if [ -z "$NDK" ] || [ ! -d "$NDK" ]; then
  for c in "$ANDROID_HOME/ndk"/* "$ANDROID_SDK_ROOT/ndk"/* "$HOME/Android/Sdk/ndk"/*; do
    [ -d "$c" ] && NDK="$c" && break
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
AR="$HOST_BIN/llvm-ar"
RANLIB="$HOST_BIN/llvm-ranlib"
STRIP="$HOST_BIN/llvm-strip"
[ -x "$CC" ] || die "no compiler $CC"
say "target: $TRIPLE$API"

WORK="$ROOT/build/android-sdl-work"
mkdir -p "$WORK" "$OUT/lib" "$OUT/include"
DL="$WORK/src"
mkdir -p "$DL"

# fetch <name> <url>
fetch() {
  local name="$1" url="$2" path="$DL/$1"
  if [ ! -e "$path" ]; then
    say "fetch $name"
    # Prefer a tarball over a clone: much smaller and no history to carry.
    if curl -fsSL --retry 3 -o "$path" "$url"; then
      case "$name" in
        *.tar.gz) tar xzf "$path" -C "$DL" ;;
        *.tar.xz) tar xJf "$path" -C "$DL" ;;
        *.tgz)    tar xzf "$path" -C "$DL" ;;
        *.zip)    unzip -q "$path" -d "$DL" ;;
      esac
      rm -f "$path"
    else
      die "could not download $url"
    fi
  fi
}

fetch "SDL.tar.gz"          "$MIRROR/SDL-$SDL_VERSION.tar.gz"
fetch "SDL2_ttf.tar.gz"     "$GITHUB_MIRROR/libsdl-org/SDL_ttf/releases/download/release-$SDL_TTF_VERSION/SDL2_ttf-$SDL_TTF_VERSION.tar.gz"
fetch "SDL2_image.tar.gz"   "$GITHUB_MIRROR/libsdl-org/SDL_image/releases/download/release-$SDL_IMAGE_VERSION/SDL2_image-$SDL_IMAGE_VERSION.tar.gz"
fetch "freetype.tar.xz"     "$GITHUB_MIRROR/freetype/freetype/archive/refs/tags/VER-$Freetype_VERSION.tar.gz"
fetch "zlib.tar.gz"         "$GITHUB_MIRROR/madler/zlib/releases/download/v$ZLIB_VERSION/zlib-$ZLIB_VERSION.tar.gz"
fetch "libpng.tar.xz"       "$GITHUB_MIRROR/glennrp/libpng/archive/refs/tags/v$LIBPNG_VERSION.tar.gz"

# Unpacked directories have version-bearing names; normalise them.
declare -A SRC
SRC[SDL]="$DL/SDL-$SDL_VERSION"
SRC[SDL_ttf]="$DL/SDL2_ttf-$SDL_TTF_VERSION"
SRC[SDL_image]="$DL/SDL2_image-$SDL_IMAGE_VERSION"
SRC[freetype]="$DL/freetype-VER-$Freetype_VERSION"
SRC[zlib]="$DL/zlib-$ZLIB_VERSION"
SRC[libpng]="$DL/libpng-$LIBPNG_VERSION"
for k in "${!SRC[@]}"; do
  [ -d "${SRC[$k]}" ] || die "expected source dir ${SRC[$k]} is missing"
done

# cmake wrapper: every sub-project is an Android static build.
BUILD="$WORK/build"
run_cmake() {
  local name="$1"; shift
  cmake -S "${SRC[$name]}" -B "$BUILD/$name" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="android-$API" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    "$@"
}

# Copies a built archive into the prefix, flattened, which is what the engine's
# link line expects: -L<prefix>/lib -l<name> with no per-subdir structure.
install_lib() {
  local name="$1" built="$2" outname="$3"
  [ -f "$built" ] || die "expected $built to have been built"
  cp "$built" "$OUT/lib/$outname"
  say "  $(basename "$outname")  $(du -h "$OUT/lib/$outname" | cut -f1)"
}

# ---------------------------------------------------------------- zlib
# SDL2_image needs zlib; a plain static build with no wrapper is enough.
say "zlib $ZLIB_VERSION"
if [ ! -f "$OUT/lib/libz.a" ]; then
  zsrc="$SRC[zlib]"
  mkdir -p "$BUILD/zlib"
  ( cd "$BUILD/zlib" && cmake -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DZLIB_BUILD_EXAMPLES=OFF "$zsrc" >/dev/null \
    && ninja >/dev/null )
  install_lib zlib "$BUILD/zlib/libz.a" libz.a
fi

# ---------------------------------------------------------------- freetype
# SDL2_ttf needs freetype. HarfBuzz and brotli are off: the engine only
# renders Latin text through TTF and gains nothing from them.
say "freetype $Freetype_VERSION"
if [ ! -f "$OUT/lib/libfreetype.a" ]; then
  run_cmake freetype \
    -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON \
    -DFT_REQUIRE_ZLIB=ON \
    -DFT_DISABLE_PNG=ON -DFT_DISABLE_JPEG=ON -DFT_DISABLE_TIFF=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_INSTALL_PREFIX="$OUT" \
    -DCMAKE_INSTALL_LIBDIR=lib
  cmake --build "$BUILD/freetype" --target freetype >/dev/null
  install_lib freetype "$BUILD/freetype/libfreetype.a" libfreetype.a
  cp -r "$SRC[freetype]/include" "$OUT/include/"
fi

# ---------------------------------------------------------------- libpng
# zlib comes from the prefix so PNG and SDL2_image share one copy.
say "libpng $LIBPNG_VERSION"
if [ ! -f "$OUT/lib/libpng16.a" ]; then
  run_cmake libpng \
    -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
    -DPNG_FRAMEWORK=OFF \
    -DZLIB_ROOT="$OUT" -DZLIB_LIBRARY="$OUT/lib/libz.a" \
    -DZLIB_INCLUDE_DIR="$OUT/include" \
    -DCMAKE_PREFIX_PATH="$OUT"
  cmake --build "$BUILD/libpng" --target png_static >/dev/null
  install_lib libpng "$BUILD/libpng/libpng16.a" libpng16.a
  cp "$SRC[libpng]/png.h" "$SRC[libpng]/pngconf.h" "$OUT/include/"
fi

# ---------------------------------------------------------------- SDL2
# The engine needs the dummy video driver, which needs no windowing system at
# all, so X11 and Wayland are compiled out. That is also what keeps the
# resulting archive free of the libraries that broke packaging Termux's build.
say "SDL2 $SDL_VERSION"
if [ ! -f "$OUT/lib/libSDL2.a" ]; then
  run_cmake SDL \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON \
    -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    -DSDL_INSTALL_TESTS=OFF -DSDL_INSTALL_EXAMPLES=OFF \
    -DSDL_X11=OFF -DSDL_WAYLAND=OFF \
    -DSDL_KMSDRM=OFF -DSDL_OFFSCREEN=OFF -DSDL_DUMMY=ON \
    -DSDL_VIDEO=ON -DSDL_AUDIO=OFF -DSDL_GPU=OFF \
    -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VULKAN=OFF \
    -DSDL_PIPEWIRE=OFF -DSDL_PULSEAUDIO=OFF -DSDL_ALSA=OFF \
    -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_LIBSYSTEM=OFF \
    -DSDL_TESTS=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_INSTALL_PREFIX="$OUT" \
    -DCMAKE_INSTALL_LIBDIR=lib
  cmake --build "$BUILD/SDL" --target SDL2-static >/dev/null
  install_lib SDL "$BUILD/SDL/libSDL2.a" libSDL2.a
  mkdir -p "$OUT/include/SDL2"
  cp -r "$SRC[SDL]/include/SDL2/." "$OUT/include/SDL2/"
fi

# ---------------------------------------------------------------- SDL2_ttf
say "SDL2_ttf $SDL_TTF_VERSION"
if [ ! -f "$OUT/lib/libSDL2_ttf.a" ]; then
  run_cmake SDL_ttf \
    -DSDL2TTF_BUILD_TESTS=OFF -DSDL2TTF_BUILD_EXAMPLES=OFF \
    -DSDL2TTF_SHARED=OFF -DSDL2TTF_STATIC=ON \
    -DFREETYPE_ROOT="$OUT" \
    -DCMAKE_PREFIX_PATH="$OUT" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  cmake --build "$BUILD/SDL_ttf" --target SDL2_ttf-static >/dev/null
  install_lib SDL_ttf "$BUILD/SDL_ttf/libSDL2_ttf.a" libSDL2_ttf.a
  cp -r "$SRC[SDL_ttf]/include" "$OUT/include/"
fi

# ---------------------------------------------------------------- SDL2_image
# PNG only. Without a JPEG or other codec the engine's PNG load and save still
# work, which is all it uses.
say "SDL2_image $SDL_IMAGE_VERSION"
if [ ! -f "$OUT/lib/libSDL2_image.a" ]; then
  run_cmake SDL_image \
    -DSDL2IMAGE_BUILD_TESTS=OFF -DSDL2IMAGE_BUILD_EXAMPLES=OFF \
    -DSDL2IMAGE_SHARED=OFF -DSDL2IMAGE_STATIC=ON \
    -DSDL2IMAGE_PNG=ON -DSDL2IMAGE_JPG=OFF -DSDL2IMAGE_TIF=OFF \
    -DSDL2IMAGE_WEBP=OFF -DSDL2IMAGE_JXL=OFF -DSDL2IMAGE_AVIF=OFF \
    -DSDL2IMAGE_QOI=ON -DSDL2IMAGE_MNG=OFF \
    -DZLIB_ROOT="$OUT" -DZLIB_LIBRARY="$OUT/lib/libz.a" \
    -DZLIB_INCLUDE_DIR="$OUT/include" \
    -DLIBPNG_ROOT="$OUT" \
    -DCMAKE_PREFIX_PATH="$OUT" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  cmake --build "$BUILD/SDL_image" --target SDL2_image-static >/dev/null
  install_lib SDL_image "$BUILD/SDL_image/libSDL2_image.a" libSDL2_image.a
  cp -r "$SRC[SDL_image]/include" "$OUT/include/"
fi

say "done -> $OUT"
ls -la "$OUT/lib"
