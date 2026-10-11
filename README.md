# BullyRecomp

A static recompilation of **Bully: Scholarship Edition** (Xbox 360) to native Windows x64,
built with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk). The game's PowerPC code is
translated to C++ ahead of time and runs on the ReXGlue runtime with a Direct3D 12 GPU backend.

> **This repository contains no game code or assets.** You need your own legally obtained copy
> of Bully: Scholarship Edition for Xbox 360. The recompiled sources (`generated/`) are produced
> locally from your copy's `default.xex` and are never committed. Bully is a trademark of
> Rockstar Games / Take-Two Interactive; this project is not affiliated with or endorsed by them.
> See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and [AI_USAGE.md](AI_USAGE.md).

## Status

Boots through the title menu and plays: the Bullworth tutorial yard runs at ~60 fps
(p95 frame time ~18 ms) on an RTX 5060 Laptop GPU, and a 10-minute unattended session passes
with no crashes, hangs or memory growth. Wider coverage (missions, town, interiors) is untested.
See [OPTIMIZATION.md](OPTIMIZATION.md) for the performance work and measurements.

Title ID `5454081A`, media ID `0867AD86` (NTSC-U disc).

## Setup

Requirements: Windows 10/11 x64, Clang (tested with 23.1.2), CMake 3.25+, Ninja, the ReXGlue
SDK `0.10.0.24-dev.gbd833a2` installed at `../rexglue` (next to this folder), and
[xdvdfs](https://github.com/antangelo/xdvdfs) to extract the disc image.

1. Put your game in `assets/`:

   ```powershell
   xdvdfs unpack '.\assets\Bully Scholarship Edition [NTSCU].iso' '.\assets\game'
   Copy-Item .\assets\game\default.xex .\assets\default.xex
   ```

2. Generate the recompiled sources from your `default.xex` (writes `generated/`):

   ```powershell
   & '..\rexglue\bin\rexglue.exe' codegen .\BullyRecomp_manifest.toml
   ```

3. Configure and build (`CMakeUserPresets.example.json` points the preset at `../rexglue`;
   copy it to `CMakeUserPresets.json`):

   ```powershell
   Copy-Item CMakeUserPresets.example.json CMakeUserPresets.json
   cmake --preset local-win-release
   cmake --build --preset local-win-release
   ```

4. Play with `play.cmd`, or let the game play itself with `autoplay.cmd` (see below).

## Settings (BullyRecomp.ini)

`play.cmd` reads `BullyRecomp.ini` next to it, created with commented defaults on first run.
Keys are setting names (any SDK flag works, e.g. `keybind_a = Space`); `[sections]` only group
them. Command-line flags override the INI, and unknown keys are reported in the log.

**Resolution scale** (`resolution_scale` under `[Graphics]`): `1` (1280x720), `2`
(2560x1440), `3` (3840x2160), `4` or `max`. The output is capped at 4K, so for this 720p game
`4` and `max` give 3x; the log states the resolution used. Each step is expensive on the GPU
(2x is four times the pixels): on the test laptop (RTX 5060 Laptop, power-limited) 2x runs at
~45-50 fps and 3x at ~21 fps in the tutorial yard. The first launch at a new scale rebuilds
the shader cache and can sit on the splash screen for 20-60 s; later launches are instant.

Scaling above 1x needs the patched GPU plugin (`patches/rexglue-sdk-scaled-compressed-textures.patch`,
see below); without it some NPC clothing renders as dark, speckled noise.

## Development notes

Code generation needed two repaired branches and 23 extra static initializers that automatic
discovery missed (`config/function-boundaries.toml`, `config/static-initializers.toml`), and two
jump tables with a misdetected index register (`config/switch-tables.toml`). No generated source
is patched by hand. The focused branch checks (`-DBULLYRECOMP_BUILD_BRANCH_TESTS=ON`, then
`ctest`) execute selected generated functions with the original XEX loaded in tool mode.

## Black-screen fix

Loading screens and the HUD rendered but the 3D world stayed black. The Xenos D3D12
backend's default render-target path was the cause; `render_target_path_d3d12=rov`
renders the world correctly. `OnPreSetup` in `src/BullyRecomp_app.h` now loads the GPU
plugin (the flag is registered by the plugin) and defaults the flag to `rov` unless the
user set it. Verified in local testing (run logs are not published):
the Bullworth tutorial renders and Jimmy moves.
`BULLYRECOMP_GPU_DIAGNOSTICS` is left OFF because that build fails to link.

## Commands

Run in this project directory using PowerShell.

Re-run strict validation, bypassing the cached generation stamp:

```powershell
& '..\rexglue\bin\rexglue.exe' codegen --ignore-stamp .\BullyRecomp_manifest.toml
```

Build using the installed Clang and local SDK:

```powershell
cmake --preset local-win-release
cmake --build --preset local-win-release --parallel 4
```

The local preset uses the sibling SDK directory. To switch compilers on an
already-configured tree, configure with `--fresh` and explicit
`-DCMAKE_C_COMPILER=...` and `-DCMAKE_CXX_COMPILER=...` arguments.

Run the focused branch checks:

```powershell
cmake --preset local-win-release -DBULLYRECOMP_BUILD_BRANCH_TESTS=ON
cmake --build --preset local-win-release --target BullyRecomp_branch_tests --parallel 4
ctest --test-dir out/build/local-win-release --output-on-failure
```

Launch using the extracted game directory:

```powershell
& '.\out\build\local-win-release\BullyRecomp.exe' --game_data_root "$PWD\assets\game"
```

The runtime expects a directory; passing the unextracted ISO as `game_data_root`
does not supply the game data.

The extraction command used was:

```powershell
& '..\xdvdfs\xdvdfs.exe' unpack '.\assets\Bully Scholarship Edition [NTSCU].iso' '.\assets\game'
```

## Tools

`tools/inspect-xex` is a small offline inspector using this installed SDK's XEX
loader. It reads image bytes without launching guest code. Its executable needs
the SDK `bin` directory on `PATH` to locate `rexruntime.dll`.

Official toolkit documentation: https://github.com/rexglue/rexglue-sdk
The installed CLI's `--help` reflects this development version; older wiki
examples use different initialization options.

## Performance tooling

`tools/perf-run.ps1 -Name <n> [-Profile]` runs a fully automatic benchmark (visible window,
scripted walk, PresentMon + in-process logger) and writes `logs/perf/<n>.summary.json`;
`tools/perf-matrix.ps1` compares configurations. See `OPTIMIZATION.md` for results.
`--bully_perf_log <csv>` and `--bully_profile` enable the in-process logger/profiler manually.

## Automated gameplay (autoplay)

`autoplay.cmd [minutes] [seed]` (or `tools\autoplay.ps1 -Minutes 10 -Seed 1`) plays the game by
itself and writes `logs\autoplay\<time>\report.md`: PASS/FAIL verdict, frame pacing, a per-minute
table, worst hitches, memory growth (8+ minutes of play) and a screenshot contact sheet.
It detects crashes (and names the crashing function from the linker map), hangs (no guest frame
for `-HangSeconds`), long stalls, black screens and frozen images.

Input is fed inside the game (`src/autoplay.cpp` overrides the `XamInputGetState` import), so the
window does not need focus and you can keep using the PC. Keep the window visible, though: a
minimized window presents nothing. A real controller still works on top of the script.

- `--bully_autoplay=wander` (default): taps A through the title menu and intro for
  `--bully_autoplay_boot_s` seconds (45), then wanders the world with seeded random movement,
  camera sweeps, sprint taps and the odd jump. The same seed replays the same inputs.
- `--bully_autoplay=<file>`: a timeline script for repeatable routes; see
  `tools\autoplay\yard-loop.txt` for the format.
- `-PresentMon` also records display-side frame times; `-Env @{vsync='true'}` / `-ExtraArgs`
  pass settings through, so A/B checks work the same way as with `perf-run.ps1`.

## Local SDK runtime

Two patches apply to a rexglue-sdk checkout at `bd833a2` (the same commit as the installed SDK):

- `patches/rexglue-sdk-scaled-compressed-textures.patch` (GPU plugin): with resolution scaling,
  pages once written by a render-to-texture copy stay marked "scaled" after the game reuses the
  memory for a streamed texture, because GPU writes never clear the mark. Such textures were
  sampled from the stale 2x copy (garbage NPC clothing). Compressed (DXT) textures can never be
  resolve targets, so they are no longer treated as scaled.
- `patches/rexglue-sdk-timer-queue.patch` (runtime), with unit tests: the timer queue thread
  never blocked; it spun through `_mm_pause`/yield waiting for the 1 ms `KeTimeStampBundle` tick
  (15-65% of a core). It now waits on a high-resolution waitable timer.

Build them and stage the DLLs over the prebuilt ones:

```powershell
cd ..\rexglue-sdk
cmake --preset win-amd64
cmake --build out/build/win-amd64 --config Release --target rexruntime rexgpu-xenos
cd ..\BullyRecomp
cmake -S . -B out/build/local-win-release -D BULLYRECOMP_RUNTIME_OVERRIDE_DIR="$PWD/../rexglue-sdk/out/win-amd64/Release"
cmake --build out/build/local-win-release
```

Clear `BULLYRECOMP_RUNTIME_OVERRIDE_DIR` to go back to the prebuilt runtime. On Windows, git
checks submodule symlinks out as text files; `thirdparty/libmspack` and `thirdparty/o1heap` had
their 16 symlinks replaced with copies of the targets to build.
