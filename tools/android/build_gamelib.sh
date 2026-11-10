#!/bin/bash
# The game code library for the Android app, from your own game files: tools/android/build_gamelib.sh <exefs dir> <out dir>
# Needs: tools/android/prepare.sh run first, the app's libmain.so (ANDROID_LIBMAIN, the library links against it),
# $ANDROID_NDK, python3 with capstone + unicorn. Writes <out>/lasgame.so and lasgame.so.id: copy both, with the game
# files, into Android/data/io.github.hydriostatic.swshrecomp/files/import on the device (docs/ANDROID-THOR.md).
set -euo pipefail
EXEFS=$(realpath "${1:?exefs dir}"); OUT=$(mkdir -p "${2:?out dir}" && realpath "$2")
LIBMAIN=$(realpath "${ANDROID_LIBMAIN:?path to the app\'s libmain.so}")
S=build/android-src
. tools/android/env.sh $S
CC="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang --target=aarch64-linux-android33"
GEN=build/android-gen
rm -rf $GEN && mkdir -p $GEN
for m in "sdk 0x7004000000" "main 0x7000000000" "subsdk0 0x7006000000" "subsdk1 0x7008000000"; do
    set -- $m
    base=$(python3 tools/android/rebase_guest.py --base $2)
    extra=$([ $1 = sdk ] && echo "--hle ../runtime/hle/hle_symbols.txt" || true)
    (cd $S/tools && python3 gen.py $1 "$EXEFS/$1" $base "$(realpath ../../android-gen)/$1" $extra --simd asm)
done
OBJ=build/android-gamelib-obj
rm -rf $OBJ && mkdir -p $OBJ
ls $GEN/*/*.c | xargs -P "$(nproc)" -I{} sh -c 'o='$OBJ'/$(echo {} | tr / _).o; cd '$S' && '"$CC"' '"$GENFLAGS_ANDROID"' -c ../../{} -o ../../$o'
(cd $S && $CC $GENFLAGS_ANDROID -DGAMELIB_ID="\"$GAMELIB_ID_ANDROID\"" -c tools/gamelib_entry.c -o ../../$OBJ/gamelib_entry.o)
ls $OBJ/*.o > $OBJ/objs.rsp
$CC -shared -fuse-ld=lld -Wl,-z,max-page-size=16384 -o "$OUT/lasgame.so" @$OBJ/objs.rsp "$LIBMAIN"
echo "$GAMELIB_ID_ANDROID" > "$OUT/lasgame.so.id"
echo "built $OUT/lasgame.so ($(du -h "$OUT/lasgame.so" | cut -f1)), id $GAMELIB_ID_ANDROID"
