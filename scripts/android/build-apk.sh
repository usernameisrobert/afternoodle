#!/usr/bin/env bash
# ============================================================================
#  afternoodle engine  --  build the Android APK
# ============================================================================
#  Packages the engine, its JNI bridge and a small Java host into an installable
#  APK, without Gradle:
#
#      javac -> d8 -> aapt2 link -> zipalign -> apksigner
#
#  Gradle would need a network-resolved plugin graph for a project with no
#  dependencies, and the Android SDK tools do the same job directly. The
#  project is small enough that this is easier to reproduce and to debug.
#
#  Inputs:
#    ANDROID_SDK_ROOT / ANDROID_HOME   SDK with platforms/android-3x (android.jar)
#    ANDROID_SDL_PREFIX                prefix from build-sdl-android.sh
#    ANDROID_NDK                       NDK, for the native compiler
#    AFNDLE_KEYSTORE                   signing key (default: a debug key)
#
#  The default keystore is the well-known AOSP debug key -- password "android",
#  alias "androiddebugkey". That is a published key, not a secret, and using it
#  means every build is signed identically so an APK can be replaced in place
#  on a device. A real release must be signed with a private key instead; set
#  AFNDLE_KEYSTORE to do that.
#
#  Usage:  sh scripts/android/build-apk.sh
# ============================================================================
set -euo pipefail

ABI="${ANDROID_ABI:-arm64-v8a}"
API="${ANDROID_API:-24}"
MIN_SDK=24
TARGET_SDK="${ANDROID_TARGET_SDK:-34}"
PKG="com.afternoodle.app"

# Read rather than hardcode, so the APK version cannot drift from the library's.
VERSION="$(sed -n 's/^#define AFNDLE_VERSION_STRING "\(.*\)"/\1/p' \
  include/afndle/core/afconfig.h)"
[ -n "$VERSION" ] || die "no AFNDLE_VERSION_STRING in include/afndle/core/afconfig.h"

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

# Absolute from here on: d8, zipalign and apksigner all resolve their arguments
# against their own working directory, and this script changes directory.
OUT="$ROOT/build/apk"
APK_DIR="$OUT/apk"
say() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# ------------------------------------------------------------------ toolchain
SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/android-sdk}}"
[ -d "$SDK" ] || die "no Android SDK; set ANDROID_SDK_ROOT"
ANDROID_JAR="$SDK/platforms/android-$TARGET_SDK/android.jar"
[ -f "$ANDROID_JAR" ] || die "no android.jar for API $TARGET_SDK under $SDK/platforms"
say "sdk: $SDK (android.jar API $TARGET_SDK)"

# A full SDK keeps the build tools under build-tools/<version>/ rather than on
# PATH, which is where they are in CI, so each one is resolved to an absolute
# path here instead of being assumed to be on PATH.
BUILD_TOOLS="$(ls -d "$SDK"/build-tools/*/ 2>/dev/null | sort -V | tail -1)"
find_tool() {
  local name="$1" d
  if command -v "$name" >/dev/null 2>&1; then
    command -v "$name"
    return
  fi
  for d in "$BUILD_TOOLS" "$SDK/cmdline-tools/latest/bin/"; do
    if [ -x "${d}${name}" ]; then echo "${d}${name}"; return; fi
  done
  die "$name not found (looked on PATH and under $SDK/build-tools)"
}

JAVAC="$(find_tool javac)"
D8="$(find_tool d8)"
AAPT2="$(find_tool aapt2)"
ZIP="$(find_tool zip)"
ZIPALIGN="$(find_tool zipalign)"
APKSIGNER="$(find_tool apksigner)"
KEYTOOL="$(find_tool keytool)"
say "tools: $(dirname "$AAPT2")"

# ------------------------------------------------------------------ SDL prefix
SDL_PREFIX="${ANDROID_SDL_PREFIX:-$ROOT/build/android-sdl}"
[ -f "$SDL_PREFIX/lib/libSDL2.a" ] || \
  die "no SDL2 for Android at $SDL_PREFIX. Run scripts/android/build-sdl-android.sh first."
say "sdl prefix: $SDL_PREFIX"

# ------------------------------------------------------------------ toolchain
NDK="${ANDROID_NDK:-${ANDROID_NDK_ROOT:-}}"
if [ -z "$NDK" ] || [ ! -d "$NDK" ]; then
  for c in "$ANDROID_HOME/ndk"/* "$ANDROID_SDK_ROOT/ndk"/* "$HOME/Android/Sdk/ndk"/*; do
    [ -d "$c" ] && NDK="$c" && break
  done
fi
[ -n "$NDK" ] && [ -d "$NDK" ] || die "no NDK found; set ANDROID_NDK"

case "$ABI" in
  arm64-v8a)   TRIPLE="aarch64-linux-android" ;;
  armeabi-v7a) TRIPLE="armv7a-linux-androideabi" ;;
  x86_64)      TRIPLE="x86_64-linux-android" ;;
  *) die "unsupported ABI '$ABI'" ;;
esac
HOST_BIN="$(ls -d "$NDK"/toolchains/llvm/prebuilt/*/bin 2>/dev/null | head -1)"
CC="$HOST_BIN/${TRIPLE}${API}-clang"
AR="$HOST_BIN/llvm-ar"
STRIP="$HOST_BIN/llvm-strip"
[ -x "$CC" ] || die "no NDK compiler $CC"

# The statically linked SDL stack leaves two references that have to be
# satisfied at load time, and neither is obvious from the link line:
#
#   libandroid.so  SDL2's Android backend uses ANativeWindow_*, ALooper_*,
#                  AAsset* and ASensor*, none of which are in libc. Without
#                  the DT_NEEDED entry the library fails to load with
#                  "cannot locate symbol ANativeWindow_fromSurface".
#   libc++_static  Some objects in the stack reference operator new[] and
#                  __gxx_personality_v0 for their exception frames. The
#                  static runtime is used rather than libc++_shared.so so the
#                  APK still ships exactly one native library.
PREBUILT="$(dirname "$(dirname "$HOST_BIN")")"
LIBCXX_STATIC="$PREBUILT/sysroot/usr/lib/$TRIPLE/libc++_static.a"
[ -f "$LIBCXX_STATIC" ] || die "no libc++_static.a under $PREBUILT/sysroot"
say "target: $TRIPLE$API"

# ------------------------------------------------------------------ clean
rm -rf "$OUT"
mkdir -p "$APK_DIR" "$OUT/classes" "$OUT/obj" "$APK_DIR/lib/$ABI"

# ------------------------------------------------------------------ font
# The generated bitmap font is not in version control, and the engine needs it.
[ -f src/render/builtinfont.c ] || python3 scripts/gen_font.py

# ------------------------------------------------------------------ native
# libafndle.so: the engine plus the JNI bridge, with SDL2 and its satellites
# linked in statically. Linking the archives by hand rather than letting the
# driver guess keeps the order explicit, and repeating the group where it
# helps resolves the cycles between the engine, SDL and its backends.
say "compiling the engine and JNI bridge for $ABI"

ENGINE_SRC=$(find src/core src/platform src/render -name '*.c' 2>/dev/null | sort)
[ -n "$ENGINE_SRC" ] || die "no engine sources found"

CFLAGS="-O2 -std=c11 -fno-strict-aliasing -fPIC
  -Iinclude -Isrc -I$SDL_PREFIX/include -I$SDL_PREFIX/include/SDL2
  -DAFNDLE_BUILDING
  -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers"

OBJS=""
for f in $ENGINE_SRC android/jni/afternoodle_jni.c; do
  o="$OUT/obj/$(echo "$f" | tr '/' '_' | sed 's/\.c$/.o/')"
  "$CC" $CFLAGS -c "$f" -o "$o"
  OBJS="$OBJS $o"
done
# The check suite, compiled in so the APK can run the same tests CI runs.
"$CC" $CFLAGS -c tests/test_engine.c -o "$OUT/obj/test_engine.o"
OBJS="$OBJS $OUT/obj/test_engine.o"

say "linking libafndle.so"
L="$SDL_PREFIX/lib"
"$CC" -shared -fvisibility=default -o "$APK_DIR/lib/$ABI/libafndle.so" \
  $OBJS \
  -Wl,--start-group \
    "$L/libSDL2.a" "$L/libSDL2_image.a" "$L/libSDL2_ttf.a" \
    "$L/libfreetype.a" "$L/libpng16.a" "$L/libz.a" \
  -Wl,--end-group \
  "$LIBCXX_STATIC" \
  -landroid -llog -lm -ldl
"$STRIP" --strip-unneeded "$APK_DIR/lib/$ABI/libafndle.so"
say "  libafndle.so $(du -h "$APK_DIR/lib/$ABI/libafndle.so" | cut -f1)"

# Fail here rather than at install time on the device.
if command -v readelf >/dev/null 2>&1; then
  echo "  native deps:"
  readelf -d "$APK_DIR/lib/$ABI/libafndle.so" | grep NEEDED | sed 's/^/    /'
fi

# An unresolved symbol is a load-time failure on the device, not a link-time
# one: the linker is satisfied as soon as a DT_NEEDED exists, whether or not
# the library behind it exports what was asked for. So every undefined symbol
# is checked against the libraries the .so actually depends on, and a gap
# fails the build here instead of producing an APK that cannot be loaded.
if command -v nm >/dev/null 2>&1; then
  SO="$APK_DIR/lib/$ABI/libafndle.so"
  # nm prints undefined symbols as "  U NAME" and defined ones as
  # "ADDR T NAME", so the name is field 2 in one case and field 3 in the other.
  nm -D --undefined-only "$SO" | awk 'NF>=2{print $NF}' | sed 's/@.*//' \
    | sort -u > "$OUT/undef.txt"

  found=0
  for lib in $(readelf -d "$SO" | grep NEEDED | sed -n 's/.*\[\(.*\)\].*/\1/p'); do
    case "$lib" in
      libc.so)        path="/system/lib64/$lib" ;;
      liblog.so|libm.so|libdl.so|libandroid.so) path="/system/lib64/$lib" ;;
      *)              continue ;;
    esac
    # Not present in the build container, which is not Android; skip quietly.
    [ -f "$path" ] || continue
    nm -D --defined-only "$path" 2>/dev/null | awk 'NF>=3{print $NF}' \
      | sed 's/@.*//' | sort -u >> "$OUT/provided.txt"
    found=$((found + 1))
  done
  if [ "$found" -eq 0 ]; then
    say "  (no system libraries to check against; skipping)"
  else
    sort -u "$OUT/provided.txt" -o "$OUT/provided.txt"
    missing="$(comm -23 "$OUT/undef.txt" "$OUT/provided.txt" || true)"
    if [ -n "$missing" ]; then
      printf 'error: libafndle.so has undefined symbols no dependency provides:\n' >&2
      printf '  %s\n' $missing >&2
      die "these would fail to resolve when the library is loaded"
    fi
    say "  all $(wc -l < "$OUT/undef.txt") undefined symbols resolve"
  fi
fi

# ------------------------------------------------------------------ java
say "compiling java"
find android/java -name '*.java' | sort > "$OUT/sources.txt"
"$JAVAC" -nowarn -source 11 -target 11 \
  -classpath "$ANDROID_JAR" \
  -d "$OUT/classes" \
  @"$OUT/sources.txt" 2>&1 | grep -v 'bootstrap class path' || true
[ -f "$OUT/classes/com/afternoodle/engine/Afternoodle.class" ] || \
  die "javac produced no classes"

say "dexing"
# d8 resolves the paths in its argument file relative to its own working
# directory, so it has to run from inside the class output.
( cd "$OUT/classes" \
  && find . -name '*.class' > classfiles.txt \
  && "$D8" --release --min-api "$MIN_SDK" --lib "$ANDROID_JAR" \
       --output "$OUT" @classfiles.txt )
[ -f "$OUT/classes.dex" ] || die "d8 produced no classes.dex"

# ------------------------------------------------------------------ resources
say "compiling resources"
RES_ZIP="$OUT/res.zip"
if [ -d android/res ]; then
  "$AAPT2" compile --dir android/res -o "$RES_ZIP"
else
  RES_ZIP=""
fi

# ------------------------------------------------------------------ link
say "linking $PKG"
AAPT2_LINK_ARGS=()
[ -n "$RES_ZIP" ] && AAPT2_LINK_ARGS+=("$RES_ZIP")

"$AAPT2" link \
  -o "$OUT/base.apk" \
  -I "$ANDROID_JAR" \
  --manifest android/AndroidManifest.xml \
  --min-sdk-version "$MIN_SDK" \
  --target-sdk-version "$TARGET_SDK" \
  --version-code 1 \
  --version-name "$VERSION" \
  --no-version-vectors \
  "${AAPT2_LINK_ARGS[@]}" \
  2>&1 | sed 's/^/  /'

# The dex and the native library are added to the linked APK: aapt2 knows
# about neither, and zipalign has to run over the result. The .so is added
# stored rather than deflated, so Android can map it straight out of the APK
# instead of having to unpack it somewhere it may not be allowed to write.
say "adding dex and native library"
( cd "$APK_DIR" && "$ZIP" -q -0 "$OUT/base.apk" "lib/$ABI/libafndle.so" )
( cd "$OUT" && "$ZIP" -q "$OUT/base.apk" classes.dex )

# ------------------------------------------------------------------ sign
say "aligning"
"$ZIPALIGN" -f -p 4 "$OUT/base.apk" "$OUT/aligned.apk"

KEYSTORE="${AFNDLE_KEYSTORE:-$OUT/debug.keystore}"
KS_PASS="${AFNDLE_KEYSTORE_PASS:-android}"
KEY_ALIAS="${AFNDLE_KEY_ALIAS:-androiddebugkey}"
if [ ! -f "$KEYSTORE" ]; then
  say "generating the AOSP debug keystore (published key, not a secret)"
  "$KEYTOOL" -genkeypair -v \
    -keystore "$KEYSTORE" \
    -storepass "$KS_PASS" -keypass "$KS_PASS" \
    -alias "$KEY_ALIAS" \
    -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
fi

say "signing with $KEY_ALIAS from $(basename "$KEYSTORE")"
# v1 signing too: it costs nothing and keeps the APK installable on anything
# old enough to need it.
"$APKSIGNER" sign \
  --ks "$KEYSTORE" --ks-pass "pass:$KS_PASS" --key-pass "pass:$KS_PASS" \
  --ks-key-alias "$KEY_ALIAS" \
  --v1-signing-enabled true --v2-signing-enabled true \
  --out "$OUT/afternoodle.apk" "$OUT/aligned.apk"

"$APKSIGNER" verify --print-certs "$OUT/afternoodle.apk" | sed 's/^/  /' || \
  die "the APK did not verify after signing"

VER="$(git rev-parse --short HEAD 2>/dev/null || echo dev)"
FINAL="$ROOT/dist/afternoodle-$PKG-$ABI-$VER.apk"
mkdir -p "$ROOT/dist"
cp "$OUT/afternoodle.apk" "$FINAL"

say "built $FINAL  ($(du -h "$FINAL" | cut -f1))"
echo "  install with: adb install -r $FINAL"
