// romfs_pack: builds romfs.bin from a folder of game files with the runtime's own builder (runtime/romfs_build.c),
// the same image the start screen makes. For dumps whose RomFS comes out as files only: an update applied over the
// base game (hactool writes a patched RomFS with --romfsdir, not --romfs).
//   make build/romfs_pack.exe && build/romfs_pack.exe <romfs folder> <out romfs.bin>
#include <stdio.h>

#include "romfs_build.h"

static void progress(uint64_t done, uint64_t total) {
    static int last = -1;
    int pct = total ? (int)(done * 100 / total) : 0;
    if (pct / 10 != last / 10) { last = pct; fprintf(stderr, "  %d%%\n", pct); }
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: romfs_pack <romfs folder> <out romfs.bin>\n"); return 2; }
    char why[256] = "";
    if (!romfs_build(argv[1], argv[2], progress, why, sizeof why)) { fprintf(stderr, "failed: %s\n", why); return 1; }
    fprintf(stderr, "ok: %s\n", argv[2]);
    return 0;
}
