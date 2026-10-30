#!/usr/bin/env bash
# Linux x86-64 desktop build of Wipeout Pure (Vulkan, SDL3) for Ubuntu 24.04 and newer, with ad hoc play through the
# master server, packed as out/WipeoutPure-linux-x64.tar.gz. It holds the compiled game: for the owner's own PCs only,
# never published.
#
# Built against an Ubuntu 24.04 sysroot (tools/linux/make_noble_sysroot.py, glibc 2.39, GCC 13's libstdc++) with the
# NDK's clang, so the binary runs on 24.04 although this host is newer. SDL3 3.4.16 is built here too, with the float
# math functions glibc 2.43 re-versioned bound to their old version (glibc239_compat.h).
#
#   tools/linux/build_linux.sh [deps|configure|build|package|all]     (default: all)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
D="${PURE_LINUX_DEPS:-$HOME/workspace/pure-vr-deps/linux}"
NDK_BIN="${NDK_BIN:-$HOME/workspace/android-ndk-r30/toolchains/llvm/prebuilt/linux-x86_64/bin}"
B="$ROOT/build/linux-x64"
NINJA="${NINJA:-$(command -v ninja || echo "$HOME/.local/bin/ninja")}"
JOBS="${JOBS:-4}"
export CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/pure-recomp/ccache}"

deps() {
    mkdir -p "$D/bin"
    [[ -d "$D/noble-sysroot/usr/include" ]] || python3 "$ROOT/tools/linux/make_noble_sysroot.py" "$D/noble-sysroot"
    local S="$D/noble-sysroot"
    for l in lib lib64; do [[ -e "$S/$l" ]] || ln -s "usr/$l" "$S/$l"; done
    for t in clang clang++; do
        printf '#!/bin/sh\nexec %s/%s --target=x86_64-pc-linux-gnu --sysroot=%s --gcc-toolchain=%s/usr -fuse-ld=lld "$@"\n' \
            "$NDK_BIN" "$t" "$S" "$S" > "$D/bin/noble-$t"
        chmod +x "$D/bin/noble-$t"
    done
    if [[ ! -d "$D/Vulkan-Headers-1.4.341/include" ]]; then
        curl -fsSL -o "$D/vh.tar.gz" https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.341.tar.gz
        tar xzf "$D/vh.tar.gz" -C "$D"
    fi
    cat > "$D/glibc239_compat.h" <<'EOF'
/* Bind the float math functions glibc 2.43 re-versioned to their old version, so SDL3 runs on glibc 2.39. */
#if defined(__x86_64__) && defined(__linux__)
__asm__(".symver atan2f,atan2f@GLIBC_2.2.5");
__asm__(".symver acosf,acosf@GLIBC_2.2.5");
__asm__(".symver asinf,asinf@GLIBC_2.2.5");
__asm__(".symver log10f,log10f@GLIBC_2.2.5");
__asm__(".symver sqrtf,sqrtf@GLIBC_2.2.5");
#endif
EOF
    if [[ ! -f "$D/sdl3/lib/libSDL3.so.0" ]]; then
        local SDL_SRC="${SDL_SRC:-$HOME/workspace/pure-vr-deps/SDL3-3.4.16}"
        cmake -S "$SDL_SRC" -B "$D/sdl3-build" -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_COMPILER="$HOME/tools/hostclang/clang" -DCMAKE_CXX_COMPILER="$HOME/tools/hostclang/clang++" \
            "-DCMAKE_C_FLAGS=-include $D/glibc239_compat.h" -DCMAKE_INSTALL_PREFIX="$D/sdl3" -DSDL_SHARED=ON \
            -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_X11=ON -DSDL_WAYLAND=ON -DSDL_PULSEAUDIO=ON \
            -DSDL_ALSA=ON -DSDL_PIPEWIRE=OFF -DSDL_VULKAN=ON -DSDL_OPENGL=ON -DSDL_HIDAPI=ON
        "$NINJA" -C "$D/sdl3-build" -j"$JOBS" install
    fi
}

configure() {
    CC="$D/bin/noble-clang" CXX="$D/bin/noble-clang++" cmake -S "$ROOT" -B "$B" -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" \
        -DCMAKE_BUILD_TYPE=Release -DPORTABLEKIT_RELEASE=ON -DPURE_ADHOC=ON \
        -DCMAKE_C_COMPILER="$D/bin/noble-clang" -DCMAKE_CXX_COMPILER="$D/bin/noble-clang++" \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DSDL3_DIR="$D/sdl3/lib/cmake/SDL3" -DVulkan_INCLUDE_DIR="$D/Vulkan-Headers-1.4.341/include" \
        -DVulkan_LIBRARY=/usr/lib/x86_64-linux-gnu/libvulkan.so.1 -DPORTABLEKIT_GLSLANG="$(command -v glslangValidator)" \
        '-DCMAKE_BUILD_RPATH=$ORIGIN/lib'
}

build() { "$NINJA" -C "$B" -j"$JOBS" PureNative; }

package() {
    local P="$ROOT/out/WipeoutPure-linux-x64"
    rm -rf "$P" && mkdir -p "$P/lib"
    cp "$B/bin/PureNative" "$P/"
    cp -L "$B/bin/lib/"* "$P/lib/"
    cp -L "$D/sdl3/lib/libSDL3.so.0" "$P/lib/"
    cp "$D/sdl3/share/licenses/SDL3/LICENSE.txt" "$P/lib/SDL3-LICENSE.txt"
    cp "$ROOT/tools/linux/wipeout-pure.sh" "$ROOT/tools/linux/ANLEITUNG-PC.txt" "$P/"
    chmod +x "$P/wipeout-pure.sh" "$P/PureNative"
    # Nothing in it may need a newer C library than Ubuntu 24.04's 2.39.
    local newest
    newest=$(for f in "$P/PureNative" "$P"/lib/*.so*; do objdump -T "$f" | grep -oE 'GLIBC_[0-9.]+'; done | sort -V -u | tail -1)
    echo "newest glibc symbol: $newest"
    [[ "$(printf '%s\nGLIBC_2.39\n' "$newest" | sort -V | tail -1)" == "GLIBC_2.39" ]] || { echo "too new for 24.04" >&2; exit 1; }
    tar czf "$P.tar.gz" -C "$ROOT/out" "$(basename "$P")"
    ls -l "$P.tar.gz"
}

case "${1:-all}" in
    deps) deps ;;
    configure) configure ;;
    build) build ;;
    package) package ;;
    all) deps; [[ -f "$B/build.ninja" ]] || configure; build; package ;;
    *) sed -n 2,10p "$0"; exit 2 ;;
esac
