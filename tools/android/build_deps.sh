#!/bin/bash
# FFmpeg (LGPL, decoders the game needs only), shaderc and zstd for Android arm64 into <prefix>.
#   tools/android/build_deps.sh <prefix> <work dir>      ($ANDROID_NDK set)
set -euo pipefail
P=$(mkdir -p "$1" && realpath "$1"); W=$(mkdir -p "$2" && realpath "$2")
API=33
TC=$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64
CMAKE_TC="-DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$P"
cd "$W"
[ -d vma ] || git clone -q --depth 1 -b v3.1.0 https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator vma
mkdir -p "$P/include" && cp vma/include/vk_mem_alloc.h "$P/include/"
[ -d zstd ] || git clone -q --depth 1 -b v1.5.6 https://github.com/facebook/zstd
cmake -S zstd/build/cmake -B zstd-b -G Ninja $CMAKE_TC -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF
cmake --build zstd-b && cmake --install zstd-b
[ -d shaderc ] || { git clone -q --depth 1 -b v2024.4 https://github.com/google/shaderc && (cd shaderc && ./utils/git-sync-deps); }
cmake -S shaderc -B shaderc-b -G Ninja $CMAKE_TC -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON \
      -DSPIRV_SKIP_EXECUTABLES=ON -DENABLE_GLSLANG_BINARIES=OFF -DSPIRV_TOOLS_BUILD_STATIC=ON
cmake --build shaderc-b --target shaderc_combined
mkdir -p "$P/lib" "$P/include" && cp shaderc-b/libshaderc_util/../libshaderc/libshaderc_combined.a "$P/lib/" && cp -R shaderc/libshaderc/include/shaderc "$P/include/"
[ -d ffmpeg ] || git clone -q --depth 1 -b n7.1 https://github.com/FFmpeg/FFmpeg ffmpeg
cd ffmpeg
./configure --prefix="$P" --target-os=android --arch=aarch64 --enable-cross-compile --cc=$TC/bin/aarch64-linux-android$API-clang \
    --cxx=$TC/bin/aarch64-linux-android$API-clang++ --nm=$TC/bin/llvm-nm --ar=$TC/bin/llvm-ar --ranlib=$TC/bin/llvm-ranlib \
    --strip=$TC/bin/llvm-strip --enable-pic --disable-shared --enable-static --disable-programs --disable-doc --disable-everything \
    --enable-decoder=opus,vp9,vp8,h264,vorbis,aac --enable-demuxer=matroska,webm,mov,ogg --enable-parser=vp9,vp8,h264,opus,vorbis,aac \
    --enable-protocol=file --enable-swscale --enable-swresample --disable-avdevice --disable-avfilter --disable-network
make -j"$(nproc)" && make install
echo "deps in $P"
