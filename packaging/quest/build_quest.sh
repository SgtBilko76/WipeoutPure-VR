#!/usr/bin/env bash
# Builds Wipeout Pure VR, the APK for Meta Quest and PICO headsets (arm64, OpenXR), from a checkout whose
# generated/ corpus exists (tools/regen.sh, or psp_recomp from a host build: README-QUEST.md).
#
#   packaging/quest/build_quest.sh [install]
#
# Writes build/quest/WipeoutPureVR-<version>.apk and with `install` installs it through adb. The APK carries no
# game data: packaging/quest/push_data.sh copies the player's disc image (and an HD texture pack) to the headset.
#
# Environment: ANDROID_HOME (the SDK, default ~/Android/Sdk or ~/workspace/android-sdk), ANDROID_NDK (default the
# newest NDK in the SDK), PURE_QUEST_DEPS (where SDL3 and the OpenXR loader are fetched and built, default
# build/quest-deps), JOBS (parallel compile jobs, default 4; each generated unit needs ~1 GB), VERSION_NAME,
# KEYSTORE/KEYSTORE_PASS/KEY_ALIAS (default the SDK's debug key, so builds install over each other), RELEASE=1 (a
# release: not debuggable, VERSION_NAME required), QUEST_ADHOC=1 (ad hoc play through the master server, off by
# default until it has been tested).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/packaging/quest"
INSTALL=
for arg in "$@"; do
    case "$arg" in
        install) INSTALL=1 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

SDK="${ANDROID_HOME:-}"
if [[ -z "$SDK" ]]; then
    for candidate in "$HOME/Android/Sdk" "$HOME/workspace/android-sdk"; do
        [[ -d "$candidate/build-tools" ]] && SDK="$candidate" && break
    done
fi
[[ -d "$SDK/build-tools" ]] || { echo "set ANDROID_HOME to the Android SDK" >&2; exit 1; }
NDK="${ANDROID_NDK:-$(ls -d "$SDK"/ndk/* | sort -V | tail -1)}"
BUILD_TOOLS="$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)"
ANDROID_JAR="$(ls -d "$SDK"/platforms/android-* | sort -V | tail -1)/android.jar"
DEPS="${PURE_QUEST_DEPS:-$ROOT/build/quest-deps}"
B="$ROOT/build/android"
OUT="$ROOT/build/quest"
APP_ID=online.portroyale.wipeoutpurevr
MIN_SDK=29
SDL_VERSION=3.4.16
SDL_SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
OPENXR_VERSION=1.1.63
mkdir -p "$DEPS" "$OUT"

# SDL3 for Android, the version PortableKit pins (packaging/sources.sh).
SDL_SRC="$DEPS/SDL3-$SDL_VERSION"
SDL_INSTALL="$DEPS/sdl3-android-install"
if [[ ! -f "$SDL_INSTALL/lib/libSDL3.so" ]]; then
    tarball="$DEPS/SDL3-$SDL_VERSION.tar.gz"
    [[ -f "$tarball" ]] || curl -fsSL -o "$tarball" \
        "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.tar.gz"
    echo "$SDL_SHA256  $tarball" | sha256sum -c --quiet
    tar xzf "$tarball" -C "$DEPS"
    cmake -S "$SDL_SRC" -B "$DEPS/sdl3-android" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$MIN_SDK -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON \
        -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DCMAKE_INSTALL_PREFIX="$SDL_INSTALL" \
        -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384" > "$DEPS/sdl3-configure.log"
    cmake --build "$DEPS/sdl3-android" -j4 > "$DEPS/sdl3-build.log"
    cmake --install "$DEPS/sdl3-android" > /dev/null
fi

# The OpenXR loader (headers and libopenxr_loader.so) from Khronos' Maven release.
OPENXR_DIR="$DEPS/openxr-$OPENXR_VERSION"
if [[ ! -d "$OPENXR_DIR/prefab" ]]; then
    aar="$DEPS/openxr_loader-$OPENXR_VERSION.aar"
    curl -fsSL -o "$aar" "https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/$OPENXR_VERSION/openxr_loader_for_android-$OPENXR_VERSION.aar"
    mkdir -p "$OPENXR_DIR" && unzip -qo "$aar" -d "$OPENXR_DIR"
fi
OPENXR_LIB="$OPENXR_DIR/prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so"

# The native code. The corpus is the slow part (hours the first time; ccache keeps it afterwards).
cmake -S "$ROOT" -B "$B" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$MIN_SDK -DCMAKE_BUILD_TYPE=Release \
    -DPORTABLEKIT_ANDROID_APP=ON -DPORTABLEKIT_RELEASE=ON -DSDL3_DIR="$SDL_INSTALL/lib/cmake/SDL3" \
    -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH -DPython3_EXECUTABLE="$(command -v python3)" \
    -DPSPRECOMP_GENERATED_JOBS="${JOBS:-4}" -DPORTABLEKIT_BUILD_LABEL=Quest \
    -DPURE_QUEST_VR=ON -DPURE_OPENXR_DIR="$OPENXR_DIR" -DPURE_ADHOC="$([[ -n "${QUEST_ADHOC:-}" ]] && echo ON || echo OFF)" \
    > "$OUT/configure.log"
cmake --build "$B" --target PureNative -j"${JOBS:-4}"

# The APK, packed without Gradle as PortableKit's packaging/android/build_apk.sh does, with this manifest and the
# OpenXR loader added.
PK="$ROOT/vendor/portablekit/packaging/android"
work="$OUT/apk"
rm -rf "$work"
mkdir -p "$work/classes" "$work/dex" "$work/lib/arm64-v8a" "$work/assets/licenses"
echo "compiling SDL's Java activity and PortableKit's"
javac -nowarn --release 11 -classpath "$ANDROID_JAR" -d "$work/classes" \
    $(find "$SDL_SRC/android-project/app/src/main/java" "$PK/java" "$HERE/java" -name '*.java') 2> "$work/javac.log"
"$BUILD_TOOLS/d8" --release --min-api $MIN_SDK --lib "$ANDROID_JAR" --output "$work/dex" \
    $(find "$work/classes" -name '*.class')

echo "linking resources"
cp "$ROOT/packaging/THIRD_PARTY_NOTICES.md" "$SDL_SRC/LICENSE.txt" "$work/assets/licenses/"
mv "$work/assets/licenses/LICENSE.txt" "$work/assets/licenses/SDL3-LICENSE.txt"
cp "$B"/bin/lib/FFmpeg-COPYING.LGPLv2.1.txt "$B"/bin/lib/FFmpeg-SOURCE.txt "$work/assets/licenses/"
# The port menu's text face (the app unpacks assets/fonts/ to its storage; Android's own faces are variable fonts).
mkdir -p "$work/assets/fonts"
cp "$HERE/fonts/DejaVuSans.ttf" "$work/assets/fonts/"
cp "$HERE/fonts/DejaVu-LICENSE.txt" "$work/assets/licenses/"
# The shader pipelines seen on a headset (android_import.inc copies them to the data folder on the first start).
if compgen -G "$HERE/pipeline/*.bin" > /dev/null; then
    mkdir -p "$work/assets/pipeline"
    cp "$HERE"/pipeline/*.bin "$work/assets/pipeline/"
fi
"$BUILD_TOOLS/aapt2" compile --dir "$PK/res" -o "$work/res.zip"
"$BUILD_TOOLS/aapt2" compile --dir "$HERE/res" -o "$work/game-res.zip"
if [[ -n "${RELEASE:-}" ]]; then
    [[ -n "${VERSION_NAME:-}" ]] || { echo "a release needs VERSION_NAME" >&2; exit 1; }
    debug_flag=()
else
    debug_flag=(--debug-mode)
fi
version_name="${VERSION_NAME:-0.1.0-alpha}"
version_code="${VERSION_CODE:-$(git -C "$ROOT" rev-list --count HEAD)}"
"$BUILD_TOOLS/aapt2" link -I "$ANDROID_JAR" --manifest "$HERE/AndroidManifest.xml" \
    --min-sdk-version $MIN_SDK --target-sdk-version 32 --version-name "$version_name" --version-code "$version_code" \
    "${debug_flag[@]}" -A "$work/assets" -o "$work/unsigned.apk" --rename-manifest-package "$APP_ID" \
    "$work/res.zip" -R "$work/game-res.zip" --auto-add-overlay

echo "adding native libraries"
lib="$work/lib/arm64-v8a"
cp "$SDL_INSTALL/lib/libSDL3.so" "$B/bin/libmain.so" "$B/bin/lib/libavcodec.so" "$B/bin/lib/libavutil.so" \
    "$OPENXR_LIB" "$lib/"
shopt -s nullglob
overlays=("$B"/bin/overlays/libovl*.so)
[[ ${#overlays[@]} -gt 0 ]] && cp "${overlays[@]}" "$lib/"
llvm="$(ls -d "$NDK"/toolchains/llvm/prebuilt/*/bin | head -1)"
"$llvm/llvm-strip" --strip-unneeded "$lib"/*.so
# Every function a packed library imports must be in Android 10's system libraries or another packed one.
stubs="$llvm/../sysroot/usr/lib/aarch64-linux-android/$MIN_SDK"
{
    for system in libc libm libdl liblog libandroid libvulkan libOpenSLES libGLESv1_CM libGLESv2 libEGL libaaudio; do
        "$llvm/llvm-nm" -D --defined-only "$stubs/$system.so"
    done
    "$llvm/llvm-nm" -D --defined-only "$lib"/*.so
} 2> /dev/null | awk 'NF >= 3 { sub(/@.*/, "", $3); print $3 }' | LC_ALL=C sort -u > "$work/exports.txt"
missing="$("$llvm/llvm-nm" -D --undefined-only "$lib"/*.so | awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' |
    LC_ALL=C sort -u | LC_ALL=C comm -23 - "$work/exports.txt")"
if [[ -n "$missing" ]]; then
    echo "error: the libraries import what Android $MIN_SDK lacks:" $missing >&2
    exit 1
fi
(cd "$work" && cp dex/classes.dex . && python3 - unsigned.apk classes.dex lib <<'EOF'
# Appends to the APK aapt2 made (a `zip` on PATH may be something else).
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1], "a", zipfile.ZIP_DEFLATED, compresslevel=9) as apk:
    for item in sys.argv[2:]:
        paths = [item] if os.path.isfile(item) else sorted(
            os.path.join(d, f) for d, _, files in os.walk(item) for f in files)
        for path in paths:
            apk.write(path, path)
EOF
)

echo "aligning and signing"
keystore="${KEYSTORE:-$HOME/.android/debug.keystore}"
password="${KEYSTORE_PASS:-android}"
alias="${KEY_ALIAS:-androiddebugkey}"
if [[ ! -f "$keystore" ]]; then
    mkdir -p "$(dirname "$keystore")"
    keytool -genkeypair -keystore "$keystore" -storepass android -keypass android -alias androiddebugkey \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" > /dev/null 2>&1
fi
apk="$OUT/WipeoutPure-VR-$version_name.apk"
"$BUILD_TOOLS/zipalign" -f -P 16 4 "$work/unsigned.apk" "$work/aligned.apk"
"$BUILD_TOOLS/apksigner" sign --ks "$keystore" --ks-pass "pass:$password" --key-pass "pass:$password" \
    --ks-key-alias "$alias" --out "$apk" "$work/aligned.apk"
echo "wrote $apk ($(du -h "$apk" | cut -f1), version $version_name ($version_code))"
if [[ -n "$INSTALL" ]]; then adb install -r "$apk"; fi
