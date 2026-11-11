#!/bin/bash
# Prepares the Android build: the rebased copy of the sources (rebase_guest.py), SDL's Java side in the app and the
# shipped files as assets. Run from the repo root: tools/android/prepare.sh <SDL3 source dir>
set -euo pipefail
SDL=${1:?SDL3 source dir}
python3 tools/android/rebase_guest.py . build/android-src
cp -R third_party build/android-src/
# SMAA's shader as a C string for runtime/nvn/smaa_data.c (the Makefile's rule)
mkdir -p build/android-src/inc
LC_ALL=C tr -d '\r' < third_party/smaa/SMAA.hlsl | LC_ALL=C tr '\200-\377' '?' | \
    sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$/\\n"/' > build/android-src/inc/smaa_hlsl.inc
. tools/android/env.sh build/android-src
echo "$GAMELIB_ID_ANDROID" > build/android-src/GAMELIB_ID
echo "$GAMELIB_CFLAGS_ANDROID" > build/android-src/GAMELIB_CFLAGS
mkdir -p android/app/src/main/java/org/libsdl
rm -rf android/app/src/main/java/org/libsdl/app
cp -R "$SDL/android-project/app/src/main/java/org/libsdl/app" android/app/src/main/java/org/libsdl/
mkdir -p android/app/src/main/assets
cp third_party/fonts/Baloo2/Baloo2-SemiBold.ttf android/app/src/main/assets/
cp docs/icon/icon_1024.png android/app/src/main/assets/icon.png
echo "prepared: game code id $GAMELIB_ID_ANDROID"
