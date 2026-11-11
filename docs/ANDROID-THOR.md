# SwSh Recomp on Android (AYN Thor)

An Android build of the runtime for AArch64 handhelds, made for the AYN Thor (Snapdragon 8 Gen 2, Adreno 740,
Android 13). **Untested on a device so far**: it is built in CI, the first runs on the Thor are where the real
fixing starts.

## What differs from the PC build

- **Native ARM64 game code.** The recompiled code uses the generator's `--simd asm` path: the guest's FP/SIMD runs as
  the same AArch64 instructions on the host.
- **Guest window at 0x10_0000_0000.** The desktop builds map the game at 0x70_0000_0000, and an app forked from
  Android's zygote already has libraries there that it can't get rid of by re-executing. `tools/android/rebase_guest.py`
  writes a copy of `runtime/` and `tools/` with every guest address moved (149 literals), and both the app and the
  game code are built from that copy.
- **BC textures decoded on the CPU** (`runtime/nvn/bcdec.c`, third_party/bcdec). Adreno has ASTC but no BC1-7, which
  almost every texture in the game uses. Large levels are decoded on 4 threads. This is the first suspect for
  stutter.
- **No compiler on the device.** The game code library (`lasgame.so`) is built in your private repository and copied
  to the device along with the game files.
- **Android surfaces.** The app waits while it's in the background and makes a new surface and swapchain when it
  comes back. RGBA swapchain formats are accepted. Vulkan features the driver lacks are dropped and listed in the log
  instead of stopping vkCreateDevice.

## Building

1. **The app**: push to `thor` (or run the workflow by hand). `.github/workflows/android-thor.yml` builds FFmpeg,
   shaderc and zstd for Android (cached), SDL 3, and the APK. The artifact `swshrecomp-thor` holds
   `SwShRecomp-thor.apk`, `libmain.so` and `GAMELIB_ID`.
2. **The game code**: in your private repository with `exefs/` from your own Sword 1.3.2, add
   `tools/android/private-repo-workflow.yml` as `.github/workflows/gamelib.yml` and run it. It links against the
   `libmain.so` of the same commit. The artifact `lasgame-thor` holds `lasgame.so` and `lasgame.so.id`, and it stays
   private. Rebuild it whenever the app's `GAMELIB_ID` changes: an app only loads game code built for it.

## Setting it up on the Thor

1. Install the APK and open it.
2. Put these anywhere on the Thor's storage or SD card, in one folder:
   - `exefs/` and `romfs/` (or `romfs.bin`) from your own extracted game, and
   - `lasgame.so` and `lasgame.so.id`. They can also go in `Android/data/io.github.hydriostatic.swshrecomp/files/import/`.
3. On the start screen, press **Select game folder...**. The first time, Android asks for "All files access" for the
   app: allow it and press Back, and the folder picker opens. Pick the folder.
   The setup copies `exefs/` and builds the RomFS image (or copies `romfs.bin`) into the app's folder, so plan for
   about 15 GB free. Your folder is left as it was.

Controls: every controller is opened, and the one you press buttons on is the one the game reads, so the Thor's
built-in controls work even when Android lists other input devices first (the log names each one: `[input] gamepad`).
Android's Back works as Esc: it opens and closes the in-game menu.

Logs: `.../files/lasrecomp.log` (and `lasrecomp-prev.log`); logcat tag `SwShRecomp`.
Settings that would be environment variables on PC go one per line (`NAME=value`) in `.../files/larecomp.env`, for
example `LARECOMP_VK_PRESENT=fifo`.

## Known gaps

- Mod installs from archives (they use Windows' tar). Folder mods in `mods/` still work.
- The C++ sources (the UI and the ImGui / VMA glue) haven't been compiled for Android outside CI yet.
- RAM: the game needs about 5 GB. Thor models with 8 GB may run out.

## Bottom screen (AYN Thor)

The Thor's second display shows an information panel (`android/.../DualScreen.java`): the game and version, the state,
the frame rate (large, green / yellow / red against the target), frame time, shader preparation, memory, GPU,
battery and battery temperature. It refreshes 4 times a second, follows the top screen (it's gone while the app is
in the background), and stays the menus' grey when there's nothing to show.

To show more, publish it from anywhere in the runtime (`runtime/panel.h`):

    panel_set("party", "Lead", "%s  Lv %d", name, level);   // a "PARTY" card with one row
    panel_clear("party");                                   // gone again

Sections appear as cards in the order they're first set.
