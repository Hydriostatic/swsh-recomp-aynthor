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
- **The game code is built on the device**, as the PC release's start screen does it: the APK carries Python 3.13,
  capstone and unicorn built for Android, and zig's aarch64 build (`tools/android/build_genkit.sh`). The first setup
  takes one to two hours on the Thor. A `lasgame.so` + `lasgame.so.id` found in the game folder is used instead.
- **Android surfaces.** The app waits while it's in the background and makes a new surface and swapchain when it
  comes back. RGBA swapchain formats are accepted. Vulkan features the driver lacks are dropped and listed in the log
  instead of stopping vkCreateDevice.

## Building

Push to `thor` (or run the workflow by hand). `.github/workflows/android-thor.yml` builds FFmpeg, shaderc and zstd,
the game code builder (Python, capstone, unicorn, zig) and SDL 3, all cached, then the APK. The artifact
`swshrecomp-thor` holds `SwShRecomp-thor.apk`. It contains no game code and no game files.

`tools/android/private-repo-workflow.yml` still builds a `lasgame.so` in CI from a private repository, if you want
to skip building it on the device.

## Setting it up on the Thor

1. Install the APK and open it.
2. Put `exefs/` and `romfs/` (or `romfs.bin`) from your own extracted game in one folder, anywhere on the Thor's
   storage or SD card.
3. On the start screen, press **Select game folder...**. The first time, Android asks for "All files access" for the
   app: allow it and press Back, and the folder picker opens. Pick the folder.
   The setup copies `exefs/`, builds the RomFS image (or copies `romfs.bin`), then builds the game code. Keep the
   app open and the Thor plugged in. Plan for about 20 GB free. Your folder is left as it was.

Controls: every controller is opened, and the one you press buttons on is the one the game reads, so the Thor's
built-in controls work even when Android lists other input devices first (the log names each one: `[input] gamepad`).
Android's Back works as Esc: it opens and closes the in-game menu.

Folders (the start screen's **App** section; both in internal storage by default, and both kept when the app is
reinstalled):
- **Game files and recompiled game code**: `SwShRecomp/` (exefs, the RomFS image, `lasgame.so`, caches, settings).
  A new folder is used from the next start, and the files move there then.
- **Log and saves**: `Download/SwShRecomp/` (`lasrecomp.log`, `lasrecomp-prev.log`, `save-sword/`). The saves move
  at once to a newly picked folder.

Updates: **Check for updates** in the App section installs the newest build of the `thor` branch (the `thor-latest`
release) over the app, keeping the game files and the recompiled code. The first time, Android asks to allow
"Install unknown apps" for SwSh Recomp. Every build is signed with the same key (`android/signing/`, or the
repository secrets `SIGNING_KEYSTORE_B64` and `SIGNING_PASSWORD`).

A crash shows the last lines of the log in its message and on the bottom screen. Logcat tag: `SwShRecomp`.
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
