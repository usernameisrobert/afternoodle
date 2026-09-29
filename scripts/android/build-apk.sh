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
NM="$HOST_BIN/llvm-nm"
READELF="$HOST_BIN/llvm-readelf"
[ -x "$CC" ] || die "no NDK compiler $CC"

# The statically linked SDL stack leaves several references that have to be
# satisfied at load time, and none of them are obvious from the link line:
#
#   libandroid.so  SDL2's Android backend uses ANativeWindow_*, ALooper_*,
#                  AAsset* and ASensor*, none of which are in libc. Without
#                  the DT_NEEDED entry the library fails to load with
#                  "cannot locate symbol ANativeWindow_fromSurface".
#
# The C++ runtime is three separate archives, and the two that are easy to
# forget are the ones that matter. libc++_static.a is the library proper;
# libc++_abi.a carries the allocation and termination entry points
# (operator new/new[]/delete/delete[], std::terminate) and libunwind.a carries
# __gxx_personality_v0, which the exception frames in the stack reference.
# Leaving either out links cleanly and then fails at load time with
# "cannot locate symbol __gxx_personality_v0" -- the linker is satisfied by the
# DT_NEEDED entries above, and no system library exports these, so nothing
# reports the gap until the device's dynamic linker refuses the library. The
# static runtime is used rather than libc++_shared.so so the APK still ships
# exactly one native library.
# $HOST_BIN is .../llvm/prebuilt/<host>/bin, so one dirname gives the host
# directory that the sysroot lives under.
PREBUILT="$(dirname "$HOST_BIN")"
SYSROOT_LIB="$PREBUILT/sysroot/usr/lib/$TRIPLE"
LIBCXX_STATIC="$SYSROOT_LIB/libc++_static.a"
LIBCXX_ABI="$SYSROOT_LIB/libc++_abi.a"
LIBUNWIND="$SYSROOT_LIB/libunwind.a"
for a in "$LIBCXX_STATIC" "$LIBCXX_ABI" "$LIBUNWIND"; do
  [ -f "$a" ] || die "no $(basename "$a") under $PREBUILT/sysroot"
done
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
    "$LIBCXX_STATIC" "$LIBCXX_ABI" "$LIBUNWIND" \
  -Wl,--end-group \
  -landroid -llog -lm -ldl
"$STRIP" --strip-unneeded "$APK_DIR/lib/$ABI/libafndle.so"
say "  libafndle.so $(du -h "$APK_DIR/lib/$ABI/libafndle.so" | cut -f1)"

# Fail here rather than at install time on the device.
if command -v readelf >/dev/null 2>&1; then
  echo "  native deps:"
  readelf -d "$APK_DIR/lib/$ABI/libafndle.so" | grep NEEDED | sed 's/^/    /'
fi

# System.loadLibrary("x") loads libx.so, and the name has to be right or the
# JVM throws UnsatisfiedLinkError the first time the class initialises. That
# happens on a worker thread, so the failure kills the app at startup with
# nothing on screen: the APK builds, installs, and launches to a dead process.
# Neither javac nor aapt2 can see the mismatch, so it is checked here against
# the libraries the APK actually carries.
say "checking loadLibrary names"
for want in $(grep -rho 'loadLibrary("[^"]*")' android/java \
             | sed 's/.*"\(.*\)".*/\1/' | sort -u); do
  [ -f "$APK_DIR/lib/$ABI/lib$want.so" ] \
    || die "System.loadLibrary(\"$want\") cannot resolve: the APK has no lib$want.so"
done
for so in "$APK_DIR"/lib/"$ABI"/*.so; do
  [ -e "$so" ] || continue
  base=${so##*/}
  name=${base#lib}; name=${name%.so}
  grep -rq "loadLibrary(\"$name\")" android/java \
    || die "the APK ships $base but no System.loadLibrary(\"$name\") loads it"
done
say "  loadLibrary names match the packaged libraries"

# An unresolved symbol is a load-time failure on the device, not a link-time
# one: the linker is satisfied as soon as a DT_NEEDED exists, whether or not
# the library behind it exports what was asked for. So every undefined symbol
# is checked against the libraries the .so actually depends on, and a gap
# fails the build here instead of producing an APK that cannot be loaded.
#
# The reference has to be the NDK's own API-level stubs, not /system/lib64.
# The build runs in a Linux container that has no /system at all, so a check
# that reads the device's libraries finds nothing, reports "skipping", and
# passes: that is how __gxx_personality_v0 and the operator new/delete pair
# reached a published APK, and why the app died at System.loadLibrary. The
# stubs under the sysroot are what the library was linked against, so they are
# the right answer to "will this symbol resolve on the device", and they are
# present everywhere the NDK is.
SO="$APK_DIR/lib/$ABI/libafndle.so"
say "checking undefined symbols"
if [ -x "$NM" ] && [ -x "$READELF" ]; then
  # Undefined symbols print as "         U NAME" and defined ones as
  # "ADDR TYPE NAME", so the name is field 2 in one case and field 3 in the
  # other. Version suffixes are dropped: the device resolves the default one.
  "$NM" -D --undefined-only "$SO" | awk 'NF>=2{print $NF}' | sed 's/@.*//' \
    | sort -u > "$OUT/undef.txt"
  # A check that reads nothing and finds nothing missing has proved nothing.
  # The previous version of this script printed its results through a pipe and
  # reported success on empty output, so a broken invocation looked identical to
  # a clean library.
  n_undef=$(wc -l < "$OUT/undef.txt")
  [ "$n_undef" -gt 0 ] \
    || die "read no undefined symbols out of libafndle.so, so the check did not run"

  : > "$OUT/provided.txt"
  for lib in $("$READELF" -d "$SO" | grep NEEDED | sed -n 's/.*\[\(.*\)\].*/\1/p'); do
    path="$SYSROOT_LIB/$API/$lib"
    # A DT_NEEDED entry with no matching stub cannot be verified, and saying
    # nothing about it is how the last version of this check passed a broken
    # library, so it is an error rather than a skip.
    [ -f "$path" ] \
      || die "no NDK stub for the $lib this library depends on, so its symbols cannot be checked"
    "$NM" -D --defined-only "$path" 2>/dev/null | awk 'NF>=3{print $NF}' \
      | sed 's/@.*//' >> "$OUT/provided.txt"
  done
  sort -u "$OUT/provided.txt" -o "$OUT/provided.txt"
  n_provided=$(wc -l < "$OUT/provided.txt")
  [ "$n_provided" -gt 100 ] \
    || die "read only $n_provided provided symbols out of the NDK stubs, so the check did not run"
  missing="$(comm -23 "$OUT/undef.txt" "$OUT/provided.txt" || true)"
  if [ -n "$missing" ]; then
    printf 'error: libafndle.so has undefined symbols no dependency provides:\n' >&2
    printf '  %s\n' $missing >&2
    die "these would fail to resolve when the library is loaded, so the app would die at System.loadLibrary"
  fi
  say "  all $n_undef undefined symbols resolve against the NDK stubs"
else
  die "no llvm-nm/llvm-readelf in the NDK, so undefined symbols cannot be checked"
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
