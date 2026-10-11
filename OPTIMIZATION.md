# BullyRecomp Optimization Plan (target: 60 fps in game)

Hardware under test: NVIDIA RTX 5060 Laptop GPU, 20 logical CPU threads, Windows 11,
Clang 23 `-O3` Release, D3D12 Xenos backend.

## What we know and what we don't

There is **no frame-time measurement yet**. The "bad performance" report is unquantified,
so Phase 0 comes first. Observations from `logs/runtime-18.log` and the config:

| Observation | Why it may matter |
|---|---|
| Black-screen fix switched `render_target_path_d3d12` to `rov` | ROV (rasterizer-ordered views) is the slowest, most-compatible EDRAM emulation path. Strongest GPU-side suspect. Last known-good path (default/RTV) renders a black world, so it can't simply be switched back. |
| `test-launch.ps1` runs `--log_level debug` | Debug logging is on for every test run. The log was 1.4 MB / 13.8k lines in ~70 s, including 866 repeated `NtQueryInformationFile(XFileXctdCompressionInformation) unimplemented` errors. `play.cmd` uses `info`, so players are less affected, but profiling must use `info`/`warning`. |
| `async_shader_compilation=false` was used in the diagnostic runs | Shaders and pipelines compile on the draw thread, causing hitches the first time each is seen. 69 pipelines were created at startup from the storage cache; more are created at runtime. Compile caches under `D:\Shaders\Generated\*.cache` fail to open. |
| Process used ~92 CPU-seconds over 70 s wall time (~1.3 cores busy) | Suggests one hot guest thread (render/main) rather than good parallelism. Needs per-thread profiling to confirm. |
| Capture thread (`--bully_capture_dir`) reads back the presented frame every 3 s | Only active in test runs, not `play.cmd`. Exclude from profiling. |
| Guest code is statically recompiled (182 MB generated C++) | CPU cost depends on codegen quality: guest memory translation, byte-swapping, VMX128 vector ops, and indirect calls. |
| Failed file opens (`SURFTEST.sea`, shader cache, `\Device\Image`) and denied game-relative writes | Possible repeated failing syscalls on a hot path. Needs verification. |

These are hypotheses until Phase 0 proves them.

## Results: Phases 0 and 1 (done, automated)

Everything below is reproducible with no manual input: `tools/perf-run.ps1` launches the game
in a visible window, waits for the world, drives Jimmy with injected keys (W + arrow-key camera
sweeps), records PresentMon (portable copy in `tools/presentmon`) plus the in-process logger, and
writes `logs/perf/<name>.summary.json`. `tools/perf-matrix.ps1` runs many configs back to back.
The game window takes focus while a run is in progress (about 2 minutes each).

### What "bad performance" actually was

- **Display looked like 60 fps the whole time** (PresentMon: p99 17.2 ms, no hitches). The presenter
  repeats frames to fill 60 Hz, which hid the problem.
- **The game itself only ran ~41 swaps/s** (avg 24.5 ms, p99 ~45 ms, worst ~67 ms): frames alternating
  between one and two vblanks. That is the real "bad performance".
- **It was not GPU-bound**: GPU busy ~1.5 ms per frame, `GPU Commands` thread ~4% of a core.
- **It was not compute-bound either.** `Main XThread` sat at ~98% of a core, but the sampling profiler
  (`--bully_profile`, resolved by `tools/resolve-profile.py`) showed ~95% of that in two functions,
  `sub_828D3C90` and `sub_828D8DB8`: the game's D3D "block until GPU fence" poll, a spin loop using
  the 360's `db16cyc` backoff hint. The main thread was burning a core waiting on GPU progress.
- **A minimized window presents nothing**, and the guest then swaps ~1500/s; always benchmark with a
  visible window.

### Fix found by the matrix (`logs/perf/matrix1.md`, `matrix2.md`)

| Config | Guest swaps/s | Guest avg ms | Guest p99 ms |
|---|---|---|---|
| baseline | 41.0 | 24.5 | 44.8 |
| `host_present_from_non_ui_thread=true` | 38.0 | 26.4 | 48.7 |
| `vsync=false` | 41.4 | 24.2 | 44.6 |
| `async_shader_compilation=true` | 40.1 | 25.0 | 43.9 |
| **`d3d12_submit_on_primary_buffer_end=false`** | **59.5** | **16.8** | **31.3** |
| submit=false + `d3d12_pipeline_creation_threads=8` | 60.2 | 16.6 | 29.0 |
| submit=false + vsync off / async / tiled shared memory | ~60 | ~16.6 | 30-32 |

Applied as defaults in `OnPreSetup` (only if the user has not set the flag): 
`d3d12_submit_on_primary_buffer_end=false`, `d3d12_pipeline_creation_threads=8`.
Verified with no overrides (`logs/perf/final-defaults.summary.json`): **62.2 swaps/s, avg 16.1 ms,
p99 27.9 ms, worst 39 ms** in the tutorial walk. Display stays at 60 fps with no hitches over 33 ms.

### Phase 1 checklist

| Item | Outcome |
|---|---|
| Quiet logging in `play.cmd` | Done (`--log_level warning`). |
| Async shader compilation | No gain in the light scene. Now on by default (see Phase 4): it removes multi-second compile freezes in the busy yard; 3 runs without a crash. |
| Shader cache path (`D:\Shaders\Generated\*.cache`) | Startup read probes that fail harmlessly with "not found"; no measurable effect. Not changed. |
| `XFileXctdCompressionInformation` error spam | About 12 log lines per second, negligible cost; the implementation lives in the prebuilt SDK, so not changed. |
| GPU/vsync/present flags | Covered by the matrix; only the two above helped. |
| Hybrid-GPU selection | Already on the NVIDIA RTX 5060 (`DXGI adapter` log line); nothing to do. |

### Findings that shape Phases 2-4

- Remaining guest frame-time jitter: p99 is still ~28-31 ms (some frames take two vblanks) and
  the main thread still spends ~75% of its samples in the fence-wait loop. Real game work is therefore
  a small fraction of a frame in this scene, so **CPU optimization of game code (Phase 3) is probably not the
  priority.** The target is the GPU-completion/present latency chain that the game waits on.
- `rex::thread::TimerQueue` uses ~15% of a core; worth checking in Phase 4.
- The tutorial yard is a light scene. Re-run the matrix in the town center and an interior before
  concluding; the benchmark route is currently just "walk forward and sweep the camera".

## Results: Phase 4 (frame pacing)

### The earlier numbers were from a broken scene

`tools/perf-run.ps1` crashed about 30 s into the yard (0xC000001D) in most runs. The cause was
two "pick a random case" jump tables (`sub_8235E420`, `sub_8234C948`; `r11 = rand()*N/32767`)
that codegen switched on `r3`, the raw `rand()` result, so the generated `switch` hit its
out-of-range `__builtin_trap`. Fixed with `config/switch-tables.toml` (`[[switch_tables]]`,
register 11); a scan of all 838 generated switches found no other word table with a
mismatched index register. With NPC AI running, the yard is much busier than in the Phase 1
runs (working set ~1.2 GB vs 568 MB), and the game fell to **a flat 30 fps with 0.3-6 s
freezes**.

### Causes and fixes (busy tutorial yard, guest frame times from per-swap QPC timestamps)

| Cause | Evidence | Fix (default in `OnPreSetup` unless the user sets the flag) |
|---|---|---|
| Command processor sleeps in 1 ms steps in every PM4 `WAIT_REG_MEM`, so the game's 360 swap scheduler misses vblanks | `GPU Commands` thread: 31% `SleepEx` from the `WAIT_REG_MEM` handler; main thread 57% in the GPU-fence spin; real game work only ~14 ms/frame | `vsync=false`. In this SDK `vsync` is the *guest* vblank emulation (1 kHz when off) and the switch for that sleep, not host present sync. Game time comes from `mftb`; nothing registers a vblank/swap callback (device+16540/16544 never written); host presentation still caps at 60. |
| New pipelines compile on the command-processor thread | 0.5-6 s stalls exactly when `D3D12 Pipelines` threads hit 400-600% CPU | `async_shader_compilation=true` (3 runs, no crash; same steady-state numbers with a warm cache) |
| Game heap lock (`malloc`/`free`) has spin count 0, so every contention is a host kernel wait | 15-19% of main-thread samples in `RtlEnterCriticalSection` → `KeWaitForSingleObject` | `src/guest_locks.cpp`: overrides the `__imp__RtlEnterCriticalSection` thunk with a 20 µs `_mm_pause` spin using the runtime's exact lock protocol, then calls the original. Kernel wait drops to 3-6%. `--bully_cs_spin_us=0` disables it. |

| Config (busy yard) | Guest swaps/s | p50 ms | p95 ms | p99 ms | Worst ms | Jitter ms |
|---|---|---|---|---|---|---|
| Phase 1 defaults (`recheck-default`) | 29.9 | 33.4 | 35.9 | 45.3 | 58 | - |
| + `vsync=false` (`busy-vsyncoff`) | 59.5 | 16.7 | 18.1 | 20.4 | 36 | 1.34 |
| New defaults, no overrides (`final-pacing-1`) | 59.7 | 16.7 | 18.0 | 19.2 | 52 | 1.19 |
| New defaults, no overrides (`final-pacing-2`) | 58.2 | 16.7 | 19.0 | 34.0 | 51 | 1.78 |

Jitter is the mean absolute difference between consecutive frame times. Exit criterion
"1% low within 25% of the average" is met in `final-pacing-1` (19.2 vs 16.7 ms) and not quite in
`final-pacing-2`.

### What is left

- **The GPU is now the limit for display pacing.** PresentMon: GPU busy ~14-15 ms per frame,
  `Present()` blocking ~16.5 ms, frames shown ~30 ms after present, and uneven display
  intervals on the 144 Hz panel (Hardware Independent Flip, no VRR). `nvidia-smi` showed 80-86%
  utilization at only 1.2-1.5 GHz (max 3.09 GHz), P2, ~20-25 W: the laptop power mode is capping
  the GPU. Use the vendor Performance mode for play/benchmarks; the real fix is Phase 2 (ROV
  cost).
- ~~`rex::thread::TimerQueue` burns 20-65% of a core~~ **Fixed in the local SDK
  (`../rexglue-sdk`, `src/core/timer_queue.cpp`).** On Windows its only user is the 1 ms
  `KeTimeStampBundle` tick, and the disruptorplus `spin_wait_strategy` never blocks: each tick
  restarts ~4,000 `_mm_pause`s, then yields until the next one. It now blocks in
  `WaitForMultipleObjects` on a high-resolution waitable timer (next due time) plus an
  auto-reset event that `QueueTimer` signals; other platforms keep the old path.

  | Runtime DLL (same scene) | TimerQueue thread | Process CPU | Guest swaps/s | p99 ms |
  |---|---|---|---|---|
  | Prebuilt `rexglue/bin` | 15.1% | 1.30 cores | 60.7 | 33.1 |
  | Local build, unmodified | 15.9% | 1.31 cores | 60.4 | 33.2 |
  | Local build, fixed | **0.8%** | **1.19 cores** | 60.2 | 33.3 |

  New unit tests (`tests/unit/core/timer_queue_test.cpp`, `[timer_queue]`): due-time accuracy,
  recurring 1 ms timer (499-501 ticks in 500 ms), disarm, wakeup on an earlier timer, and CPU
  use with a 1 ms tick (old code 62+ ms per 500 ms and fails; new 0-15 ms). The rest of the
  suite is unchanged (one pre-existing failure in `output_stamp_test.cpp`, depfile escaping).
  Staged into the game via `BULLYRECOMP_RUNTIME_OVERRIDE_DIR` (see README).
- Synchronous guest file reads on the main thread (~2-3% of samples) and occasional 50 ms
  frames remain.

### Benchmarking pitfalls found

- Consecutive runs overheat the laptop: throughput fell 19 → 7.6 swaps/s across back-to-back
  runs and recovered after a cooldown. Let it cool, and re-run a baseline before trusting an A/B.
- A killed PresentMon leaves its `PresentMon` ETW session running and later captures are empty;
  `perf-run.ps1` now runs `logman stop PresentMon -ets` first.
- The pipeline cache (`Documents\BullyRecomp\cache\shaders\shareable`) warms up across runs, so
  early runs show compile stalls that later runs do not.

### New tooling

- **Unattended gameplay**: `autoplay.cmd` / `tools/autoplay.ps1` plays through the menu and
  wanders the world with in-process scripted input (works without window focus), and writes a
  PASS/FAIL report with pacing, memory, hitches, crash location and a screenshot contact sheet
  (`logs/autoplay/<name>/report.md`). Use it for Phase 5 soak tests and after every change.

- Per-swap QPC timestamps (`BullyFrameSwapHook` ring) → `<log>.frames.csv`; `perf-run.ps1`
  summaries now include `GuestFrameP50/P95/P99Ms`, `GuestFrameStdMs`, `GuestFrameJitterMs`,
  `GuestFramesOver20ms` and `DisplayChangeStdMs`.
- `--bully_profile_thread=<name prefix>` profiles any thread (e.g. `GPU Commands`,
  `rex::thread::TimerQueue`). `<log>.profile.csv.ext.csv` lists the DLL frame chain
  (`module+rva<...`) above the first exe frame, so time inside the runtime, D3D12 or ntdll is
  no longer charged to the exe caller.

## Phase 0: Measure (do before changing anything)

Goal: a reproducible baseline and a bottleneck verdict (CPU-guest, CPU-host/GPU driver,
GPU, or I/O).

1. Add a lightweight frame-time logger in `BullyRecomp_app.h` (swap-to-swap time, 1 s
   rolling avg, 1% low, worst frame), enabled by a `--bully_perf_log` cvar. Write CSV
   to `logs/perf/`.
2. Define a fixed benchmark route: new game, tutorial yard, walk the same path to the
   principal's office (about 60 s), plus one dense scene (town center) and one
   interior. Record baseline numbers for each at `info` log level, no capture thread.
3. Capture with **PIX on Windows** (GPU + CPU timing) and **Windows Performance
   Recorder / Superluminal** (per-thread CPU). Record:
   - GPU frame time and the longest passes
   - per-thread CPU load (guest main, GPU command processor, audio, kernel)
   - whether the GPU is waiting on CPU or vice versa
4. A/B matrix, same route each time: `rov` vs default render-target path (accepting black
   output for timing only), `vsync` on/off, `resolution_scale` 1 vs 2, async shader
   compilation on/off.
5. Decide the primary bottleneck and record it here. Phases 1-4 are ordered by expected
   payoff but must be re-ordered by this result.

Exit criteria: baseline table (avg fps, 1% low, worst frame) committed to
`logs/perf/baseline.md`.

## Phase 1: Free wins (config and logging, no engine changes)

1. Make `play.cmd` the profiled configuration: `--log_level warning`, no capture dir.
2. Enable async shader compilation by default and re-test the crash from attempt 12
   (exit code `-1073741819`, see `logs/crash12-summary.json`) before keeping it.
3. Fix the shader cache path: make `D:\Shaders\Generated` writable/redirected so the
   guest's own cache can be created. `play.cmd` already passes `--allow_game_relative_writes`;
   confirm the directory is created and cache files persist.
4. Suppress or implement `XFileXctdCompressionInformation` (return a valid
   "not compressed" result) so the kernel stops logging and failing ~866 times.
5. Check `vsync`, `d3d12_submit_on_primary_buffer_end`, `d3d12_pipeline_creation_threads`,
   and `d3d12_tiled_shared_memory` defaults against the measured bottleneck.
6. Ensure Windows is giving the game the dGPU and High Performance power plan
   (laptop hybrid graphics is a common 2x loss); verify via the DXGI adapter log line.

Exit criteria: measurable change vs baseline; no new visual regressions.

## Phase 2: Render-target path (likely the largest GPU win)

The black screen is a bug in the default path; ROV is the workaround, not a solution.

1. Find out *why* the default path leaves the 3D scene black. Use the draw-capture tooling
   in `src/gpu_diagnostics.cpp`. It currently fails to link because the SDK's
   `rex::gpu-xenos` target doesn't export the `D3D12CommandProcessor` symbols. Options:
   build against a patched SDK, or use PIX/RenderDoc-style captures instead.
2. Candidate causes to test: format/blend handling for the 3D pass, depth float24
   handling (`depth_float24_round`, `depth_float24_convert_in_pixel_shader`),
   `gamma_render_target_as_unorm16`, `native_2x_msaa`, and the resolve path
   (`readback_resolve`, `direct_host_resolve`).
3. If the default path can be made to work for this title, switch to it and compare the
   benchmark route. If not, upstream a minimal repro to the SDK.
4. If staying on ROV: reduce its cost (avoid unnecessary MSAA, check `draw_resolution_scale_*`,
   confirm the 1280x720 output is not being rendered at a larger scale).

Exit criteria: GPU frame time under ~12 ms on the benchmark route.

## Phase 3: CPU, recompiled guest code

Only if Phase 0 shows the guest thread is the limit.

1. Profile the guest main/render thread. Find the top functions by exclusive time and
   see whether they are game code (culling, animation, skinning), runtime helpers
   (memory translation, `NtQuery*`, synchronization), or the GPU command processor.
2. Check synchronization: the game likely busy-waits or spins on GPU/VBlank state
   (the "GPU VSync" thread). Make sure waits yield and don't burn a core or serialize
   frames.
3. Review recompiler config for hot-path costs: indirect call tables, unresolved-call
   fallbacks, and any `function-boundaries.toml` thunks on hot paths.
4. Try build-level options: LTO/ThinLTO, `-march` for the host (`x86-64-v3`) if the SDK
   allows, PGO using the benchmark route as the training workload, and
   unity builds to cut generated-code overhead. Re-measure build time vs gain.
5. Consider whether any hot guest loops (skinning, particle updates) benefit from a
   hand-optimized host implementation hooked at that address. Last resort; high effort.

Exit criteria: guest main thread under ~12 ms per frame.

## Phase 4: Stutter removal

Average fps can look fine while frame pacing is bad.

1. Shader/pipeline hitches: pre-warm the pipeline cache. Ship or generate a cache
   from a full playthrough so the first visit to an area does not compile on the
   draw thread.
2. Texture and asset streaming hitches: check synchronous file reads from `assets/game`
   (718 loose files, ~3.1 GB), antivirus scanning, and whether guest I/O waits on host
   disk. Consider a RAM cache or reading from an SSD path.
3. Audio: confirm `audio_compat` is not blocking the guest thread when the SDL buffer
   is full.
4. Frame pacing: confirm vsync/swap behaviour gives an even 16.7 ms cadence at 60 Hz
   rather than 30 fps with duplicated frames. The original game targets 30 fps on
   360, so verify what the guest's own frame limiter does and whether it must be
   patched or unlocked (see Risks).

Exit criteria: 1% low frame time within 25% of the average.

## Phase 5: Verify and lock in

1. Re-run the Phase 0 benchmark on all three scenes and compare to baseline.
2. Soak test: 30 min free roam, check memory growth and no performance decay.
3. Keep the perf logger and benchmark script in the repo so regressions are caught.
4. Update `README.md` with the final recommended settings.

## Risks and open questions

- **30 fps game logic.** Bully on Xbox 360 targets 30 fps, and gameplay timing may be tied
  to a frame limiter. "60 fps" may require a frame-rate unlock patch that can break
  animations, physics, or cutscenes. Decide up front whether the goal is "never drop
  below 30 fps, render at 60 Hz output" or a true 60 fps simulation. A real
  60 fps unlock is a separate project with its own testing burden.
- **ROV may be the floor.** If the default path can't be fixed, ROV performance on
  this GPU may cap what is achievable without SDK changes.
- **SDK is a prebuilt install.** `../rexglue` has headers and libs only, so deep GPU
  backend fixes need the SDK source.
- **Hybrid laptop GPU.** Thermal and power limits can dominate; measure plugged in.
- **Evidence gap.** Every number above beyond the log facts is a hypothesis until Phase 0
  is done.

## Tracking

| Phase | Status | Result |
|---|---|---|
| 0 Measure | done | Tooling in `tools/`; real cause is guest GPU-fence waits, not display fps or compute |
| 1 Free wins | done | Guest 41 -> 62 swaps/s via `d3d12_submit_on_primary_buffer_end=false` |
| 2 Render-target path | not started | |
| 3 Guest CPU | not started | |
| 4 Stutter | mostly done | Busy yard 30 fps + multi-second freezes -> ~59 fps, p95 18-19 ms (crash fix, `vsync=false`, async shaders, heap-lock spin). Display pacing now GPU/power-limited |
| 5 Verify | started | `autoplay.cmd` soak, 10 min wander (`logs/autoplay/soak-10min`): PASS, 59.4 fps, p95 18.3 ms, p99 22.7 ms, worst 53 ms, no stalls, working set flat (~1.2 GB). 30 min soak still to do |
