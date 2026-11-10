#!/bin/bash
# Prepares the Android build: the rebased copy of the sources (rebase_guest.py), SDL's Java side in the app and the
# shipped files as assets. Run from the repo root: tools/android/prepare.sh <SDL3 source dir>
set -euo pipefail
SDL=${1:?SDL3 source dir}
python3 tools/android/rebase_guest.py . build/android-src
cp -R third_party build/android-src/
. tools/android/env.sh build/android-src
echo "$GAMELIB_ID_ANDROID" > build/android-src/GAMELIB_ID
mkdir -p android/app/src/main/java/org/libsdl
rm -rf android/app/src/main/java/org/libsdl/app
cp -R "$SDL/android-project/app/src/main/java/org/libsdl/app" android/app/src/main/java/org/libsdl/
mkdir -p android/app/src/main/assets
cp third_party/fonts/Baloo2/Baloo2-SemiBold.ttf android/app/src/main/assets/
cp docs/icon/icon_1024.png android/app/src/main/assets/icon.png
echo "prepared: game code id $GAMELIB_ID_ANDROID"
