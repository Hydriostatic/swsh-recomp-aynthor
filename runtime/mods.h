// Mods: game files replaced or added from a folder, as Atmosphere's LayeredFS (fan translations, textures, models come
// this way; mods of the game's code - exefs, IPS patches, cheats - can't apply to the recompiled code), and the game's
// language. runtime/mods.c; the start screen's Mods tab (runtime/ui/launcher_lg.mm) picks the active ones: several at
// once, in order - where two replace the same file, the first one's wins (g_settings.mod: "<name>|<name>...").
//
//   mods/<name>/romfs/...                                  the game's RomFS paths (bin/message/English/...)
//   mods/<name>/atmosphere/contents/<title id>/romfs/...   (or any folder holding a romfs/ up to 4 levels down:
//                                                           a download unpacked as it came)
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODS_MAX 32
// The folders under mods/ that hold a romfs/: names[i] (up to MODS_MAX). Returns how many.
int mods_list(char names[][128], int max);
// The mods/ folder, made if missing, as an absolute path (the start screen opens it)
const char *mods_dir_abs(void);
// The active mods' romfs/ folders, the first one's files first (up to max). Returns how many.
int mod_romfs_dirs(const char **out, int max);
// path: a RomFS path ("bin/message/English/..."). True with the host file when an active mod replaces it (the first).
bool mod_romfs_override(const char *path, char *out, size_t cap);
// The language whose texts a mod replaces (its romfs/bin/message/<language>/), as a language code ("en-US"), or NULL
// (none, or several).
const char *mod_language(const char *name);
// Installs a mod from a file the player downloaded (.zip / .rar / .7z, unpacked with Windows' tar) or a folder, on a
// background thread: its romfs/ (Let's Go Eevee's first) is copied to mods/<file name>/romfs.
enum { MOD_INSTALL_IDLE, MOD_INSTALL_RUNNING, MOD_INSTALL_DONE, MOD_INSTALL_FAIL_UNPACK, MOD_INSTALL_FAIL_NOT_MOD };
void mod_install_start(const char *src);
int mod_install_state(void);
const char *mod_install_name(void);  // after MOD_INSTALL_DONE: the mods/ folder name
// The language the game is told (nn::settings / applet "desired language"): the first active mod's that replaces one
// language's texts, else g_settings.game_lang.
const char *game_language(void);

#ifdef __cplusplus
}
#endif
