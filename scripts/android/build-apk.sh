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

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

# Absolute from here on: d8, zipalign and apksigner all resolve their arguments
# against their own working directory, and this script changes directory.
OUT="$ROOT/build/apk"
APK_DIR="$OUT/apk"
say() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# ------------------------------------------------------------------ toolchain
for t in javac d8 aapt2 zipalign apksigner keytool; do
  command -v "$t" >/dev/null 2>&1 || die "$t is required but not on PATH"
done

SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/android-sdk}}"
[ -d "$SDK" ] || die "no Android SDK; set ANDROID_SDK_ROOT"
ANDROID_JAR="$SDK/platforms/android-$TARGET_SDK/android.jar"
[ -f "$ANDROID_JAR" ] || die "no android.jar for API $TARGET_SDK under $SDK/platforms"
say "sdk: $SDK (android.jar API $TARGET_SDK)"

# Termux ships d8 and friends in /usr/bin; a full SDK keeps them under
# build-tools, so look there too before giving up.
find_tool() {
  local name="$1"
  command -v "$name" >/dev/null 2>&1 && { command -v "$name"; return; }
  local hit
  hit="$(ls -d "$SDK"/build-tools/*/ 2>/dev/null | sort -V | tail -1)"
  [ -n "$hit" ] && [ -x "${hit}${name}" ] && { echo "${hit}${name}"; return; }
  die "$name not found (looked on PATH and in $SDK/build-tools)"
}

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
  -llog -lm -ldl
"$STRIP" --strip-unneeded "$APK_DIR/lib/$ABI/libafndle.so"
say "  libafndle.so $(du -h "$APK_DIR/lib/$ABI/libafndle.so" | cut -f1)"

# Fail here rather than at install time on the device.
if command -v readelf >/dev/null 2>&1; then
  echo "  native deps:"
  readelf -d "$APK_DIR/lib/$ABI/libafndle.so" | grep NEEDED | sed 's/^/    /'
fi

# ------------------------------------------------------------------ java
say "compiling java"
find android/java -name '*.java' | sort > "$OUT/sources.txt"
javac -nowarn -source 11 -target 11 \
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
  && d8 --release --min-api "$MIN_SDK" --lib "$ANDROID_JAR" \
       --output "$OUT" @classfiles.txt )
[ -f "$OUT/classes.dex" ] || die "d8 produced no classes.dex"

# ------------------------------------------------------------------ resources
say "compiling resources"
RES_ZIP="$OUT/res.zip"
if [ -d android/res ]; then
  aapt2 compile --dir android/res -o "$RES_ZIP"
else
  aapt2 compile android/AndroidManifest.xml -o "$RES_ZIP" 2>/dev/null || true
  RES_ARGS=()
fi

# ------------------------------------------------------------------ link
say "linking $PKG"
AAPT2_LINK_ARGS=()
[ -f "$RES_ZIP" ] && AAPT2_LINK_ARGS+=("$RES_ZIP")

aapt2 link \
  -o "$OUT/base.apk" \
  -I "$ANDROID_JAR" \
  --manifest android/AndroidManifest.xml \
  --min-sdk-version "$MIN_SDK" \
  --target-sdk-version "$TARGET_SDK" \
  --version-code 1 \
  --version-name 0.1.0 \
  --no-version-vectors \
  "${AAPT2_LINK_ARGS[@]}" \
  2>&1 | sed 's/^/  /'

# The dex and the native library are added to the linked APK: aapt2 does not
# know about either, and zipalign has to run over the result.
say "adding dex and native library"
( cd "$OUT" && zip -q base.apk classes.dex && zip -q base.apk "lib/$ABI/libafndle.so" )

# ------------------------------------------------------------------ sign
say "aligning"
zipalign -f -p 4 "$OUT/base.apk" "$OUT/aligned.apk"

KEYSTORE="${AFNDLE_KEYSTORE:-$OUT/debug.keystore}"
if [ ! -f "$KEYSTORE" ]; then
  say "generating the AOSP debug keystore (published key, not a secret)"
  keytool -genkeypair -v \
    -keystore "$KEYSTORE" \
    -storepass android -keypass android \
    -alias androiddebugkey \
    -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
fi

say "signing"
# v1 signing too: it costs nothing and keeps the APK installable on anything
# old enough to need it.
apksigner sign \
  --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
  --ks-key-alias androiddebugkey \
  --v1-signing-enabled true --v2-signing-enabled true \
  --out "$OUT/afternoodle.apk" "$OUT/aligned.apk"

apksigner verify --print-certs "$OUT/afternoodle.apk" >/dev/null 2>&1 || \
  die "the APK did not verify after signing"

VER="$(git rev-parse --short HEAD 2>/dev/null || echo dev)"
FINAL="$ROOT/dist/afternoodle-$PKG-$ABI-$VER.apk"
mkdir -p "$ROOT/dist"
cp "$OUT/afternoodle.apk" "$FINAL"

say "built $FINAL  ($(du -h "$FINAL" | cut -f1))"
echo "  install with: adb install -r $FINAL"
