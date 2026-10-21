# Changelog

## 0.1.0 (unreleased)

The first version of the port for *Pokémon Sword* / *Shield*, forked from
[Let's Go Recomp](https://github.com/raulzrrs/letsgo-recomp) 0.3.1 (its changes up to that version are in its own
changelog). Tested with *Sword* 1.3.2 and its Expansion Pass on a Core i5-14600KF with an RTX 3080 at 2560x1440;
*Shield* is not tested. The measurements behind each item are in [docs/engineering](docs/engineering/README.md) and
the cycle reports.

### Runs the game
- **Sword and Shield are accepted** by the setup (program ids), and *Sword* boots to the title, the intro and the
  overworld: the game's `setjmp` / `longjmp`, `friend:u`'s service objects, host files closed when the game closes
  them, and every shader instruction the game uses on the way (`.FMZ` multiplies, `I2I .SAT`, `SHF` modes, `HSETP2`,
  `IPA` with its own multiplier).
- **The 1.3.2 update and the Expansion Pass**: add-on content is read from `<data root>/aoc/<data id>.bin`.
- **60 fps** for the whole game, an option for 1.0.0 and 1.3.2 (`fps = 60`).

### Graphics fixes
- The scene-change snapshot is no longer drawn upside down (NVN's mirror copy flags), and the rug at the foot of the
  stairs is no longer black (programs identified by the content of their code, hashed when they are drawn).
- Integer textures are sampled as integers; NVN barriers are honoured; depth clamp is applied.
- Draws **wait** for a shader that is still being built instead of being skipped (no missing objects).

### Performance
- **The open world went from about 33 to 60 fps** at 2560x1440: GPU copies no longer stall the render thread,
  texture readbacks land when the GPU is done with them, the game's floating-point code no longer goes through a slow
  path (flush-to-zero in hardware), static vertex and index data stays in video memory, and the render thread keeps
  only the copies that a write can actually reach.
- **Towns** went from about 65 to 100 fps without the frame cap (more submission slots; descriptor pools that grow).
- **First visits stall much less**: shaders with a computed branch are translated into code the driver compiles in
  milliseconds instead of up to a second, and at the first shader a frame has to wait for, the frame's other shaders
  are found and built in parallel (a town's first entry: 8.4 s → 1.4 s with an empty cache). Shader workers run below
  the game's threads, and the game memory the GPU reads is set up at boot instead of at the first effect that needs it.
- **Shader preparation on the first start**: every shader stage in the game's files (18,587, found in its packs and
  effect files: 97.5% of what a session draws with) is compiled in the background into its own pipeline part
  (graphics pipeline library, now on by default), so a first visit links instead of compiling. As a new install: a
  wild battle went from 50 frames over 50 ms to 9-12, a town's entry from 3.5-4 s of hitches to 0.8 s, and the first 6
  minutes of a new game from 7.75 s to about 0.6 s. About 6 minutes on 8 threads (a notice in a corner of the screen shows the progress) and 3 GB of disk;
  shaders on disk are compressed (zstd).

### Tools (for developers)
- A scenario harness with saved positions, regression checks and a supervisor (`tools/swsh/`, `tools/agent/`), a
  sampling profiler for the game and render threads (`LARECOMP_SAMPLE`), wait profiles (`LARECOMP_RT_PROF`,
  `LARECOMP_VK_WAITPROF`), and a way to measure true first-time shader compiles (`LARECOMP_DRIVER_CACHE_NONCE`).
  All in [docs/DEPURACAO.md](docs/DEPURACAO.md).
