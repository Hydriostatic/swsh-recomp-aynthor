# Shared by the Android build scripts: the flags the game code is compiled with and its id (runtime/gamelib.h
# GAMELIB_ID), computed as the Makefile does - over the rebased copy, so the app and the library agree.
#   . tools/android/env.sh <rebased src dir>   ->  $GENFLAGS_ANDROID, $GAMELIB_ID_ANDROID
SRC_DIR=${1:?rebased source dir}
GENFLAGS_ANDROID="-O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -Iruntime -fPIC"
GAMELIB_ID_ANDROID=$(cd "$SRC_DIR" && (cat tools/gen.py tools/lifter.py tools/analyze.py tools/nso.py tools/passes.py \
    tools/patches.py tools/simd_c.py tools/gamelib_entry.c runtime/hle/hle_symbols.txt runtime/cpu.h runtime/simd.h \
    runtime/simd_fp.h runtime/gamelib.h; echo "$GENFLAGS_ANDROID") | sha256sum | cut -c1-16)
