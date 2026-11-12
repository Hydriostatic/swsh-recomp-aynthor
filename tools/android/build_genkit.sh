#!/bin/bash
# The game code builder for the Android app (runtime/gamebuild.c on the device): what the PC release ships as
# lasgen/ and zig/, made for Android arm64. Run in CI after tools/android/prepare.sh:
#   tools/android/build_genkit.sh <work dir>        ($ANDROID_NDK, $ANDROID_HOME set; network)
# Writes
#   android/app/src/main/jniLibs/arm64-v8a/  the programs and libraries, installed where Android lets an app run them:
#       libpyexe.so (python3.13), libpython3.13.so and its libraries, libcapstone.so, libunicorn.so, libzig.so (zig)
#   android/app/src/main/assets/genkit.zip   unpacked by the app on first start (MainActivity):
#       python/   the standard library;  pylib/  capstone, unicorn (their Python side), lz4 (tools/android/pylib)
#       tools/    the generator (the rebased copy);  gamelib-src/  what the game code is compiled against
#       zig/lib   the headers the game code includes;  zig/rt/libcompiler_rt.a  zig's runtime, built here
set -euo pipefail
W=$(mkdir -p "${1:?work dir}" && realpath "$1")
R=$PWD
S=$R/build/android-src
API=33
PYVER=3.13
JNI=$R/android/app/src/main/jniLibs/arm64-v8a
KIT=$W/genkit
rm -rf "$KIT" && mkdir -p "$JNI" "$KIT"
CMAKE_TC="-DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API -DCMAKE_BUILD_TYPE=Release"
cd "$W"

# 1. Python (CPython's own Android build: Android/android.py)
if [ ! -d cpython/cross-build/aarch64-linux-android/prefix ]; then
    [ -d cpython ] || git clone -q --depth 1 -b $PYVER https://github.com/python/cpython
    (cd cpython && python3 Android/android.py configure-build && python3 Android/android.py make-build &&
     python3 Android/android.py configure-host aarch64-linux-android && python3 Android/android.py make-host aarch64-linux-android)
fi
PFX=$W/cpython/cross-build/aarch64-linux-android/prefix
cp "$PFX/bin/python$PYVER" "$JNI/libpyexe.so"
for so in "$PFX"/lib/*.so; do cp "$so" "$JNI/"; done
mkdir -p "$KIT/python/lib"
cp -R "$PFX/lib/python$PYVER" "$KIT/python/lib/"
L=$KIT/python/lib/python$PYVER
rm -rf "$L"/{test,idlelib,tkinter,turtledemo,ensurepip,lib2to3,pydoc_data,venv,sqlite3/test,config-*}
find "$L" -name __pycache__ -prune -exec rm -rf {} +
ls "$L/lib-dynload" | grep -q _ctypes || { echo "python: no _ctypes (capstone / unicorn need it)"; exit 1; }

# 2. capstone and unicorn: the libraries for Android, their Python side as is
[ -d capstone ] || git clone -q --depth 1 -b 5.0.6 https://github.com/capstone-engine/capstone
cmake -S capstone -B capstone-b -G Ninja $CMAKE_TC -DBUILD_SHARED_LIBS=ON -DCAPSTONE_BUILD_TESTS=OFF -DCAPSTONE_BUILD_CSTOOL=OFF \
      -DCAPSTONE_ARCHITECTURE_DEFAULT=OFF -DCAPSTONE_ARM64_SUPPORT=ON
cmake --build capstone-b
cp capstone-b/libcapstone.so "$JNI/"
[ -d unicorn ] || git clone -q --depth 1 -b 2.1.1 https://github.com/unicorn-engine/unicorn
cmake -S unicorn -B unicorn-b -G Ninja $CMAKE_TC -DUNICORN_ARCH=aarch64 -DUNICORN_BUILD_TESTS=OFF -DUNICORN_INSTALL=OFF
cmake --build unicorn-b
cp "$(ls unicorn-b/libunicorn.so* | head -1)" "$JNI/libunicorn.so"
mkdir -p "$KIT/pylib"
cp -R capstone/bindings/python/capstone "$KIT/pylib/"
rm -rf "$KIT/pylib/capstone/lib" "$KIT/pylib/capstone/include"
cp -R unicorn/bindings/python/unicorn "$KIT/pylib/"
rm -rf "$KIT/pylib/unicorn/lib" "$KIT/pylib/unicorn/include"
cp -R "$R/tools/android/pylib/lz4" "$KIT/pylib/"

# 3. the generator and what the game code is compiled against (the rebased copy: the app's own addresses)
mkdir -p "$KIT/tools" "$KIT/gamelib-src"
cp "$S"/tools/{gen,lifter,analyze,nso,passes,patches,simd_c}.py "$R/tools/android/genpar.py" "$KIT/tools/"
cp "$S"/runtime/{cpu.h,simd.h,simd_fp.h,gamelib.h} "$S/tools/gamelib_entry.c" "$S/runtime/hle/hle_symbols.txt" "$KIT/gamelib-src/"

# 4. zig: the aarch64-linux build runs on the device (a static binary); the x86_64 one here finds what it needs
VER=0.17.0
(cd "$R" && ZIG_TARGET=aarch64-linux bash tools/fetch_zig.sh && rm -rf "$W/zig-arm" && mv build/zig "$W/zig-arm" &&
          ZIG_TARGET=x86_64-linux bash tools/fetch_zig.sh && rm -rf "$W/zig-x86" && mv build/zig "$W/zig-x86")
cp "$W/zig-arm/zig" "$JNI/libzig.so"
. "$R/tools/android/env.sh" "$S"
P=$W/zigprobe
rm -rf "$P" && mkdir -p "$P/cache"
export ZIG_GLOBAL_CACHE_DIR=$P/cache ZIG_LOCAL_CACHE_DIR=$P/cache
printf '#include "cpu.h"\n#include "simd.h"\n' > "$P/probe.c"
Z=$W/zig-x86/zig
(cd "$S" && $Z cc $GENFLAGS_ANDROID -c "$P/probe.c" -o "$P/probe.o" -MD -MF "$P/probe.d" &&
            $Z cc $GENFLAGS_ANDROID '-DGAMELIB_ID="0"' -c tools/gamelib_entry.c -o "$P/entry.o" -MD -MF "$P/entry.d")
ZL=$W/zig-x86/lib
mkdir -p "$KIT/zig/lib/std" "$KIT/zig/rt"
for h in $(cat "$P/probe.d" "$P/entry.d" | tr ' ' '\n' | sed -n "s|^$ZL/||p" | sort -u); do
    mkdir -p "$KIT/zig/lib/$(dirname "$h")"
    cp "$W/zig-arm/lib/$h" "$KIT/zig/lib/$h"
done
cp "$W/zig-arm/lib/std/std.zig" "$KIT/zig/lib/std/"
cp "$W/zig-arm/LICENSE" "$KIT/zig/"
# zig's runtime library (128-bit division and the like) for this target, as a full link of a probe builds it
printf 'unsigned __int128 probe(unsigned __int128 a, unsigned __int128 b) { return a / b; }\n' > "$P/rt.c"
$Z cc -target aarch64-linux-musl -fPIC -shared -o "$P/rt.so" "$P/rt.c"
CRT=$(find "$P/cache" -name 'libcompiler_rt.a' | head -1)
[ -f "$CRT" ] || { echo "no libcompiler_rt.a in zig's cache after the probe link"; find "$P/cache" -name '*.a'; exit 1; }
cp "$CRT" "$KIT/zig/rt/libcompiler_rt.a"
# check: the slim copy compiles the probe with an empty cache, and links as the device does (runtime/gamebuild.c)
rm -rf "$P/cache2" && mkdir -p "$P/cache2"
(cd "$S" && ZIG_LIB_DIR=$KIT/zig/lib ZIG_GLOBAL_CACHE_DIR=$P/cache2 ZIG_LOCAL_CACHE_DIR=$P/cache2 \
    $Z cc $GENFLAGS_ANDROID -I"$KIT/gamelib-src" -c "$P/probe.c" -o "$P/probe2.o")
$Z ld.lld -shared -z max-page-size=16384 -o "$P/check.so" "$P/probe2.o" "$KIT/zig/rt/libcompiler_rt.a"
echo "zig check: ok"
# zig cc's -O1 is clang's -O2 (ReleaseFast): runtime/gamebuild.c adds -Xclang -O1 - the last -O must be that one
lastO=$(cd "$S" && $Z cc $GENFLAGS_ANDROID -Xclang -O1 -I"$KIT/gamelib-src" -c "$P/probe.c" -o "$P/probe3.o" -v 2>&1 | grep -- '-cc1' | tr ' ' '\n' | grep -E '^"?-O[0-9sz]"?$' | tail -1)
echo "zig cc: last optimization flag to clang: ${lastO:-?}"
# the direct clang the device compiles with ("zig clang", runtime/gamebuild.c add_direct_clang): zig cc's target and
# include folders, in its order; @ZIG@ = the zig lib folder on the device. Checked: the probe compiles with them alone.
CF=$KIT/zig/clang_flags.txt
printf -- '-target\naarch64-unknown-linux-musl\n-nostdinc\n-fno-stack-protector\n' > "$CF"
for d in include libc/include/aarch64-linux-musl libc/include/generic-musl libc/include/aarch64-linux-any libc/include/any-linux-any; do
    [ -d "$KIT/zig/lib/$d" ] && printf -- '-isystem\n@ZIG@/%s\n' "$d" >> "$CF"
done
cat "$CF"
mapfile -t CFL < <(sed "s#@ZIG@#$KIT/zig/lib#" "$CF")
(cd "$S" && $Z clang "${CFL[@]}" ${GAMELIB_CFLAGS_ANDROID/-target aarch64-linux-musl /} -I"$KIT/gamelib-src" -c "$P/probe.c" -o "$P/probe4.o" &&
            $Z clang "${CFL[@]}" ${GAMELIB_CFLAGS_ANDROID/-target aarch64-linux-musl /} -I"$KIT/gamelib-src" '-DGAMELIB_ID="0"' -c tools/gamelib_entry.c -o "$P/entry4.o")
$Z ld.lld -shared -z max-page-size=16384 -o "$P/check4.so" "$P/probe4.o" "$P/entry4.o" "$KIT/zig/rt/libcompiler_rt.a"
echo "direct clang check: ok"

echo "$GAMELIB_ID_ANDROID $(date -u +%Y%m%d%H%M%S)" > "$KIT/VERSION"
rm -f "$R/android/app/src/main/assets/genkit.zip"
mkdir -p "$R/android/app/src/main/assets"
(cd "$KIT" && zip -qr -9 "$R/android/app/src/main/assets/genkit.zip" .)
du -sh "$R/android/app/src/main/assets/genkit.zip" "$JNI"
