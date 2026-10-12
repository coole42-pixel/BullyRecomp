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

**Display VSync** (`d3d12_vsync = true` under `[Graphics]`) is enabled by default.
`d3d12_frame_pacing = true` keeps only one frame queued for presentation, and
`bully_fps_limit = 60` limits new game frames to a steady cadence without catch-up bursts.
Fullscreen uses `fullscreen_exclusive = true` and `fullscreen_refresh_rate = 120`
to request a 120 Hz display mode; the desktop mode is restored when fullscreen ends.
`d3d12_exclusive_fullscreen = true` also puts DXGI into exclusive fullscreen, with
`d3d12_vsync_frame_rate = 60` selecting two display refreshes per game frame at 120 Hz.
The interval is based on the actual display mode, so a 60 Hz mode uses one refresh.
Set `fullscreen_exclusive = false` to use borderless fullscreen at the desktop refresh rate,
and set `d3d12_exclusive_fullscreen = false` too. Set `fullscreen_refresh_rate = 60`
for a display that does not support 120 Hz. If DXGI exclusive mode is unavailable,
the renderer retains synchronized windowed presentation and reports the fallback.
The separate SDK `vsync = false` setting under `[Performance]` controls guest console
timing; enabling it can drop busy scenes to 30 FPS. Reaching 60 FPS still requires
enough GPU performance; use 1x resolution for the best chance. A 60 Hz or 120 Hz
display mode gives an even cadence for 60 FPS. Set `d3d12_vsync = false` to disable
display synchronization; VRR/tearing is separately opt-in with
`d3d12_allow_variable_refresh_rate_and_tearing = true`. `bully_fps_limit = 0` disables
the game frame limit. The presentation and fullscreen refresh settings require the local
runtime patches `patches/rexglue-sdk-host-vsync.patch` and
`patches/rexglue-sdk-fullscreen-refresh.patch`.

**Motion blur** (`bully_motion_blur` under `[Graphics]`): set `false` to zero the
game's motion-blur accumulation strength, or `true` for the original effect. The
first-run default is `true`; this option also appears in the F4 settings overlay.
It changes the game effect while preserving its render pass and history bookkeeping.
Driver frame generation is separate: NVIDIA Smooth Motion can produce doubled moving
edges even while the game's blur is off. Disable Smooth Motion for BullyRecomp in
the NVIDIA application profile if this occurs.

**Resolution scale** (`resolution_scale` under `[Graphics]`): `1` (1280x720), `2`
(2560x1440), `3` (3840x2160), `4` or `max`. The output is capped at 4K, so for this 720p game
`4` and `max` give 3x; the log states the resolution used. Each step is expensive on the GPU
(2x is four times the pixels): on the test laptop (RTX 5060 Laptop, power-limited) 2x runs at
~45-50 fps and 3x at ~21 fps in the tutorial yard. The first launch at a new scale rebuilds
the shader cache and can sit on the splash screen for 20-60 s; later launches are instant.

Scaling above 1x needs the patched GPU plugin (see "Local SDK runtime" below). Without it some
NPC clothing renders as dark, speckled noise, and shadows turn into a checkerboard.

**Shadows** come from a 1024x1024 shadow map, the most the Xbox 360's 10 MB of EDRAM fits
(colour + depth); a larger map overflows EDRAM and corrupts, so its size is not an option.
Resolution scale renders that map at 2048 (2x) or 3072 (3x) host pixels instead, and
`scaled_texture_size_constants = true` (the default) keeps the game's PCF shadow filter in step
with it, so shadows get sharper with the scale instead of checkered.

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

`src/debug_tools.cpp` has runtime reverse-engineering helpers (off by default):
`--bully_find_texture=1024x1024:36` logs guest texture fetch constants of a size/format,
`--bully_find_dwords=44800000,44800000` logs guest addresses holding a dword sequence, and
`--bully_watch_address=<hex guest address>` sets a hardware write breakpoint and logs the call
stacks (resolve the `E:` module offsets against `BullyRecomp.map`). This is how the shadow-map
creation path (`sub_8271BA38` -> `NiShadowManager` `sub_8268ED28`) was found.
`tools\autoplay\shadow-orbit.txt` stands Jimmy still and orbits the camera for shadow checks.

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

These patches apply to a rexglue-sdk checkout at `bd833a2` (the same commit as the installed SDK):

- `patches/rexglue-sdk-scaled-compressed-textures.patch` (GPU plugin): with resolution scaling,
  pages once written by a render-to-texture copy stay marked "scaled" after the game reuses the
  memory for a streamed texture, because GPU writes never clear the mark. Such textures were
  sampled from the stale 2x copy (garbage NPC clothing). Compressed (DXT) textures can never be
  resolve targets, so they are no longer treated as scaled.
- `patches/rexglue-sdk-scaled-shadow-filter.patch` (GPU plugin): Bully filters its shadow map
  by hand with `ShadowMapSize = 1024` (Gamebryo's `TexSizeShadow` constant). With resolution
  scaling the map is 2048+ host texels, so the filter skipped texels and weighted on the wrong
  grid (checkerboard shadows). Pixel shader constants that exactly equal the guest size (or
  1/size) of a sampled resolution-scaled texture are now scaled with it
  (`scaled_texture_size_constants`, default on).
- `patches/rexglue-sdk-timer-queue.patch` (runtime), with unit tests: the timer queue thread
  never blocked; it spun through `_mm_pause`/yield waiting for the 1 ms `KeTimeStampBundle` tick
  (15-65% of a core). It now waits on a high-resolution waitable timer.
- `patches/rexglue-sdk-top5-performance.patch` (GPU plugin and codegen), applied after the
  shadow-filter and TimerQueue patches: async submission no longer drains pipeline creation;
  bindless depth-only shaders can compile asynchronously; shader publication and cache-flush
  requests are synchronized. Real occlusion results are queued until their GPU fence completes,
  with query-slot reuse protected and idle/WAIT_REG_MEM completion polling. `db16cyc` emits
  a portable processor pause. Includes codegen and query-lifecycle regression tests.
- `patches/rexglue-sdk-host-vsync.patch` (runtime): adds `d3d12_vsync` to synchronize
  display presentation independently of guest console timing. Bully enables it by default
  while keeping guest `vsync` off for 60 FPS. Also adds `d3d12_frame_pacing`: a DXGI
  waitable swap chain with one queued frame, preserving its flags across resize and
  closing its handle on teardown. Synchronized presentation disables the separate UI
  tick wait. Tearing is disabled during synchronized presents.
  It also supports DXGI exclusive fullscreen and a VSync interval matched to the
  requested frame rate and actual display refresh, preserving all swap-chain flags
  across resize and returning to windowed mode during teardown.
- `patches/rexglue-sdk-fullscreen-refresh.patch` (runtime): adds
  `fullscreen_refresh_rate` so exclusive fullscreen can use a 60 Hz mode rather than
  the desktop's 144 Hz cadence. This is separate from guest `video_mode_refresh_rate`.
- `patches/rexglue-sdk-controller-input.patch` (runtime): serializes UI/guest input reads
  and SDL event draining so older stick events cannot overwrite a newer release. Guest
  packet numbers now follow the final merged input, including dialog neutralization and
  disconnects, so keyboard packet counters cannot hide controller changes. Includes
  a current SDL axis snapshot on every read so missed motion events cannot leave an
  old running direction cached. Regression tests cover rapid left/right reversals
  while A is held, stick release, dialog blocking/resume, and disconnect clearing.

The local `BullyRecomp.ini` now uses `resolution_scale=1`, matching first-run defaults. The INI
is ignored by Git; higher scales remain opt-in quality settings. ROV remains the render-target
default: RTV testing narrowed its black-world failure to depth handling, but disabling depth
is only a diagnostic and is not a correctness fix. See `PERF_ISSUES.md` for results and limits.

Build them and stage the DLLs over the prebuilt ones:

```powershell
cd ..\rexglue-sdk
cmake --preset win-amd64
cmake --build out/build/win-amd64 --config Release --target rexruntime rexgpu-xenos rexglue unit_tests
cd ..\BullyRecomp
& '..\rexglue-sdk\out\win-amd64\Release\rexglue.exe' codegen --ignore-stamp .\BullyRecomp_manifest.toml
cmake -S . -B out/build/local-win-release -D BULLYRECOMP_RUNTIME_OVERRIDE_DIR="$PWD/../rexglue-sdk/out/win-amd64/Release" -D BULLYRECOMP_CODEGEN_EXECUTABLE="$PWD/../rexglue-sdk/out/win-amd64/Release/rexglue.exe"
cmake --build out/build/local-win-release
```

Set `BULLYRECOMP_CODEGEN_EXECUTABLE` so automatic regeneration also preserves the pause hints;
using the old installed CLI would drop them again. Runtime overrides now fail configuration
if either DLL is missing. Override DLLs are link dependencies, so a subsequent game build
relinks/restages changed SDK binaries even when the game's own objects are up to date.

`d3d12_async_occlusion_queries=false` restores synchronous real-query completion for A/B checks.
`d3d12_log_perf_counters=true` logs cumulative query completions/waits and skipped async draws;
it is off in normal play. `d3d12_log_depth_state`, `d3d12_diagnostic_disable_depth` and
`d3d12_diagnostic_disable_stencil` are explicitly opt-in rendering diagnostics, not play settings.

Clear `BULLYRECOMP_RUNTIME_OVERRIDE_DIR` and `BULLYRECOMP_CODEGEN_EXECUTABLE` to go back to the prebuilt SDK. On Windows, git
checks submodule symlinks out as text files; `thirdparty/libmspack` and `thirdparty/o1heap` had
their 16 symlinks replaced with copies of the targets to build.
