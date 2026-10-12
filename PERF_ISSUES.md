# BullyRecomp Performance Issues

## Summary

- Highest expected GPU win: resolve the RTV black-world bug so ROV software output merging is no longer required; the root cause is still unknown.
- Highest immediate settings opportunity: the shipped player INI selects 2× resolution (4× pixel area), while the first-run template selects 1×.
- Highest remaining cold-scene hitch risk: async shader compilation still drains work and waits at submission end; vertex-only translation also remains synchronous.
- Highest confirmed CPU waste: the guest GPU-fence loop loses its `db16cyc` backoff; newer profiles still attribute about 31.8% exclusive samples to the two fence functions.
- Investigate occlusion waits, enabled memexport readback/fallbacks, repeat shared-memory uploads and synchronous guest file reads before optimizing ordinary game compute.
- The TimerQueue fix is already in the local SDK and selected by the current build cache; fresh-build/runtime staging remains fragile.
- Smaller opportunities include critical-section entry overhead, SIMD/localization/build experiments, audio/wait allocations and instrumentation overhead.
- Existing yard evidence shows approximately 60 swaps/s but exact p99 of 19.182–33.973 ms; the 10-minute soak still has three frames over 50 ms. Town, missions and interiors lack equivalent measurements.

Original audit date: 2026-10-11. Reviewed BullyRecomp `main` at `2f82040`, the installed SDK linkage/configuration, local `../rexglue-sdk` source, selected upstream `../xenia-edge` paths, sampled generated code, and existing logs. The original audit was read-only apart from this report. The later implementation pass below was separately authorized and did build, regenerate, test and run the game. Historical citations/measurements in the issue descriptions refer to the audit snapshot, not necessarily current post-change line numbers.

Supplementary review: also checked `C:/Users/Admin/Downloads/BullyRecomp Performance Optimization.md` against this checkout. This adds two Low/Hypothesis findings (37–38) and scalar FP evidence to issue 19, bringing the report to 38 issues. The document supplies ideas, not new BullyRecomp measurements; its generic percentage gains were not adopted. The applicability review at the end records accepted, duplicate and unsupported recommendations.

**Interpretation:** “Confirmed” means the cited mechanism/default or historical measurement is demonstrable; it does not imply an isolated FPS benefit for its proposed fix. “Hypothesis” means relevance or the suspected avoidable cost needs profiling. Impact estimates are deliberately qualitative where no controlled A/B exists. Severity reflects potential user-visible performance, not correctness severity. Findings are ordered by severity, then approximate expected impact; dependent gains must not be added together.

Paths are relative to the BullyRecomp root. Line references describe this checkout; generated-function names are the stable locator. SDK fixes require rebuilding compatible DLLs and staging them over the prebuilt `../rexglue` runtime that supplies headers/import libraries. Editing SDK source alone will not alter a player executable. Current `CMakeCache.txt` establishes intended override staging, not the identity of every historical or running DLL.

## Implementation update: top five issues (2026-10-11)

The SDK changes are implemented in the local checkout and packaged as `patches/rexglue-sdk-top5-performance.patch`, verified to apply after the existing shadow-filter and TimerQueue patches at SDK commit `bd833a2`. Existing SDK edits were preserved. The rebuilt GPU DLL is staged into the player executable directory, and generated pause hints were rebuilt into the executable. This is not a claim that all five issues are solved.

| Issue | Current result | Evidence / remaining limitation |
|---|---|---|
| 1: ROV / black RTV world | **Unresolved, narrowed to depth handling; ROV retained.** | Normal RTV, depth-conversion/rounding alternatives, resolve/gamma/MSAA alternatives and stencil-only bypass all retain the black world. Disabling depth/stencil together renders the yard, with incorrect overlap; that bypass is diagnostic only. Explicit DSV initialization did not fix it and was reverted. Viewport diagnostics show the expected float-depth remapping (0…0.5, including reverse-Z draws), but the exact depth-test/ownership discrepancy is not proven. |
| 2: 2× default | **Local player INI changed to 1×.** | `BullyRecomp.ini` now matches `src/settings_ini.cpp`'s first-run template. A repository check also corrected an audit assumption: this INI is **Git-ignored**, not checked in. The tracked template already used 1×; the fix applies to this user's actual player settings. |
| 3: async submission waits | **Implemented and exercised in cold-cache yard.** | Async `EndSubmission` no longer drains/waits for creation workers. Bindless depth-only draws also qualify for async. Pipeline state is acquired before draw recording; workers are joined before object destruction, translation status is atomically published, binding setup is serialized, and shader-storage flush requests use atomic exchange. Bindful root-signature construction remains synchronous deliberately. Cold run observed 910 skipped async draws and no >=5 s stall. |
| 4: lost `db16cyc` | **Implemented, regenerated and rebuilt.** | SDK codegen emits `simde_mm_pause()` for the actual Xenon `0x7FFFFB78` encoding. Regeneration changed six generated files; sampled fence code now has eight pause calls at the hint sequence. A regression test verifies actual instruction emission. `BULLYRECOMP_CODEGEN_EXECUTABLE` selects the patched CLI for automatic regeneration, preventing the installed older CLI from dropping the fix. CPU/power benefit still needs a controlled comparison. |
| 5: immediate occlusion waits | **Implemented; no yard throughput benefit established.** | Query endings enqueue real results without per-query submission/completion waits; primary-buffer/idle wait boundaries submit pending query work. Completed fences publish results and free slots, with an event waking the idle processor and payload ordered before guest completion markers. Only query-heap exhaustion can require a wait. Queue tests cover in-flight slot reuse and batched completion. All observed yard runs reported zero queries, so actual GPU-query rendering validation in other scenes remains outstanding. `d3d12_async_occlusion_queries=false` retains the synchronous comparison path. |

Validation completed so far:

- Release SDK GPU/CLI and game builds succeeded; compatible local runtime/GPU DLLs were staged. Patched CLI selection was confirmed in the generated Ninja command. Game `repaired_branches` CTest passed.
- Targeted SDK tests: **11 cases, 100 assertions passed**, including query lifecycle and pause codegen.
- Broader SDK suite excluding the known output-stamp group: **203 cases passed, four existing BitStream cases skipped; 911 assertions passed**. Running the excluded group separately reproduces its pre-existing depfile-escaping failure (one case, two assertions); it was not fixed as part of this work.
- `logs/autoplay/top5-updated-rov/summary.json`: 2-minute warm run, PASS, 58.89 swaps/s, p95 18.25 ms, p99 33.4 ms, worst 50.1 ms, no long stall. This run predates final telemetry/wakeup refinements.
- `logs/autoplay/top5-cold-rov/summary.json`: fresh isolated host shader cache, PASS, 59.18 swaps/s, p95 18.2 ms, p99 33.3 ms, worst 66.8 ms, no long stall; cumulative telemetry records 910 skipped async draws and zero queries. Existing user caches were not deleted.
- `logs/autoplay/top5-final-rov/summary.json`: final 3-minute run, PASS, **59.0 swaps/s, p95 18.16 ms, p99 32.94 ms, worst 50.2 ms**, one frame over 50 ms and no long stall. Warm-cache telemetry reports zero skipped async draws and zero queries. The contact sheet renders the yard in six captures and contains one black capture at t=75 s; there is no sustained black sequence, but the transient capture is not explained and full visual parity is not established by the PASS verdict. Final restaging hashes match the rebuilt SDK GPU DLL (`SHA256 09592538AEACB659573C06CB279C2F0D03F45BFC945AF947EB758B17834DFD3B`).
- RTV evidence: `logs/autoplay/top5-baseline-rtv`, `top5-rtv-depth-conversion`, `top5-rtv-resolve-msaa`, `top5-rtv-no-depth`, `top5-rtv-no-stencil`, `top5-rtv-viewport` and `top5-rtv-depth-init`. The **images** were inspected: short black runs can report PASS because autoplay requires three post-boot black screenshots to fail. That verdict alone does not establish RTV correctness. The D3D12 debug layer could not be enabled on this machine (`top5-rtv-depth-debug/game.log:1`).

These runs were capture-enabled and were not thermally/cache-matched before/after pairs. No isolated FPS uplift is claimed. Town, mission and interior validation remains necessary; full PIX depth/EDRAM ownership captures are the next step for issue 1. Optional depth-bypass/state-log flags are disabled in normal play. The configuration now fails if an explicitly requested runtime override is missing a required DLL, rather than silently staging the prebuilt GPU backend. Override DLLs are link dependencies, so future game builds relink/restage SDK changes; the final configured build and branch CTest succeeded.

The issue table and detailed descriptions below retain the original audit findings for traceability; use this update for their current implementation state.

## Issue table

| # | Title | Area | Severity (High/Med/Low) | Status (Confirmed/Hypothesis) | Est. impact | Fix location | Effort (S/M/L) |
|---|---|---|---|---|---|---|---|
| 1 | ROV remains the expensive workaround for the black RTV world | GPU backend | High | Confirmed | Largest GPU opportunity; gain unmeasured | SDK rebuild | L |
| 2 | The checked-in player INI selects 2× rendering, unlike fresh-file defaults | Settings / GPU / memory | High | Confirmed | 4× pixel area; observed lower throughput | Repo INI / settings template | S |
| 3 | Async pipeline creation still drains and waits at submission end | GPU shaders / CPU-GPU sync | High | Confirmed | Cold-area hitch potential; duration unmeasured | SDK rebuild | L |
| 4 | Fence polling has lost the guest’s db16cyc backoff | Guest CPU / synchronization / codegen | Med | Confirmed | CPU/power win; FPS gain not established | Codegen + regeneration; optional repo hook | M |
| 5 | Host occlusion results submit and wait for the GPU immediately | GPU readback / synchronization | Med | Confirmed | Potential serialization per query; rate unknown | SDK rebuild | L |
| 6 | Memexport readback is enabled globally and its fast path can fully synchronize | GPU readbacks / settings / sync | Med | Confirmed | Potential per-draw stalls; usage unknown | SDK rebuild / repo flag experiment | M |
| 7 | Guest file reads remain synchronous even for asynchronous handles | Kernel HLE / I/O / streaming | Med | Confirmed | Measured 2–3% main samples; hitch potential | SDK rebuild | L |
| 8 | Vertex-only draws bypass the async shader path | GPU shaders | Med | Confirmed | Cold shadow/depth hitch potential | SDK rebuild | M |
| 9 | Eight pipeline workers serialize shader translation behind one mutex | GPU shaders / threading | Med | Confirmed | Cold compile throughput limit; gain unknown | SDK rebuild | M |
| 10 | CPU-uploaded shared-memory pages lose validity every frame | GPU uploads / memory | Med | Confirmed | Repeated copy/bandwidth cost; bytes unknown | Repo flag experiment / SDK rebuild | M |
| 11 | Unknown memexport destinations invalidate the entire shared-memory range | GPU texture invalidation / memory | Med | Hypothesis | Potential broad cache invalidation; frequency unknown | SDK shader analysis / rebuild | L |
| 12 | TimerQueue’s measured fix is not selected by fresh release presets | Build / kernel threading / deployment | Med | Confirmed | ~0.14 core saved when fix is deployed | Repo staging / SDK rebuilt distribution | S |
| 13 | Vblank emulation still sleeps coarsely and catches up in bursts | Frame pacing / kernel timing | Med | Hypothesis | Possible residual tail latency | SDK rebuild | M |
| 14 | Short guest delays are rounded down to host milliseconds | Kernel waits / frame pacing | Med | Hypothesis | Busy-yield or timing jitter if exercised | SDK rebuild | M |
| 15 | The 20 µs critical-section spin applies to every guest lock | Kernel HLE / locks | Med | Hypothesis | Potential CPU waste outside measured yard | Repo src/guest_locks.cpp | M |
| 16 | Volatile guest copies prevent broad copy optimization | Guest CPU / memory / codegen | Med | Hypothesis | Small yard cost; streaming gain unknown | Repo host hook or codegen + regeneration | M |
| 17 | Supported register-localization options are disabled | Guest CPU / CR-XER / codegen | Med | Hypothesis | Unmeasured compute reduction | Repo codegen config + regeneration | M |
| 18 | Weak/noinline boundaries and no LTO or PGO limit cross-function optimization | Build / guest CPU | Med | Hypothesis | Unknown; likely secondary in yard | Repo CMake; possible codegen / SDK rebuild | M |
| 19 | Some guest FP and VMX128 operations still lower to scalar or expensive exact math | Guest SIMD / build / codegen | Med | Hypothesis | Potential animation/compute win; hotness unknown | SDK codegen + regeneration; optional repo ISA flags | M |
| 20 | First-touch GPU shared-memory commitment may hitch during streaming | GPU memory / tiled resources | Med | Hypothesis | First-visit tail risk; not measured | SDK rebuild / measured flag experiment | M |
| 21 | Guest heap allocation scans page state while holding the heap mutex | Memory / kernel HLE | Med | Hypothesis | Fragmentation/streaming risk; unmeasured | SDK rebuild | L |
| 22 | Guest frame pacing still differs from displayed pacing and latency | Presentation / frame pacing | Med | Confirmed | Observed uneven display cadence; fix unknown | SDK presenter or display settings | M |
| 23 | Audio submission holds the global region across a contended driver path | Audio / threading | Low | Hypothesis | Possible small lock-tail cost | Repo audio compatibility + SDK rebuild | M |
| 24 | Each timebase read crosses into the shared clock and may lock | Guest CPU / mftb / kernel timing | Low | Hypothesis | Unknown; probably small in yard | SDK rebuild | M |
| 25 | The 4× anisotropic override changes eligible guest texture filtering | Settings / GPU texture sampling | Low | Hypothesis | Small quality/performance tradeoff | Repo INI / settings template | S |
| 26 | Compression-query errors still format and synchronously flush logs | Kernel HLE / logging | Low | Confirmed | Known small cost; no material FPS gain shown | SDK rebuild | S |
| 27 | The lock override calibrates for 5 ms and times even a free lock | Guest locks / startup CPU | Low | Confirmed | One-time 5 ms; small repeated entry overhead | Repo src/guest_locks.cpp | S |
| 28 | Multi-object waits allocate vectors in three layers | Kernel HLE / memory / threading | Low | Confirmed | Small per-call allocation; rate unknown | SDK rebuild | S |
| 29 | Screenshot capture adds full-frame queue work and allocation to benchmarks | Diagnostics / GPU readback / memory | Low | Confirmed | Periodic benchmark disturbance; absent in play | Repo tools; SDK capture reuse if needed | S |
| 30 | The sampling profiler suspends the measured thread while allocating and unwinding | Diagnostics / measurement bias | Low | Confirmed | Profile-only perturbation; magnitude unknown | Repo src/bully_profiler.h | M |
| 31 | The perf logger itself wakes at 1 kHz and changes timer resolution | Diagnostics / CPU / measurement bias | Low | Confirmed | Perf-only CPU/wakeup cost | Repo src/BullyRecomp_app.h | S |
| 32 | The frame timestamp hook runs even without perf logging | Diagnostics / guest hot path | Low | Confirmed | Tiny cost per swap | Repo src/main.cpp or codegen hook config | S |
| 33 | Explicit RE debug modes scan memory or do synchronous per-draw I/O | Diagnostics / logging / memory | Low | Confirmed | Potentially large when explicitly enabled | Repo debug tools / diagnostic backend | S |
| 34 | Native D3D12 PSO blobs are not explicitly persisted | GPU cache / startup | Low | Hypothesis | Small observed warm-start opportunity | SDK rebuild | M |
| 35 | The scaled-shadow fix scans texture bindings and can reupload constants | GPU CPU bindings / existing fix side cost | Low | Confirmed | Small at scale >1; unmeasured | SDK GPU plugin rebuild | S |
| 36 | The primary perf script bypasses the player INI | Settings / measurement coverage | Low | Confirmed | Can hide shipped quality-setting costs | Repo tools/perf-run.ps1 | S |
| 37 | Floating-point mode guards recur at control-flow and call boundaries | Guest FP / codegen | Low | Hypothesis | Small guard/transition cost; rate unknown | SDK codegen + regeneration | M |
| 38 | Host draw recording is still performed per surviving guest draw | GPU command recording | Low | Hypothesis | Dense-scene CPU opportunity; unmeasured | SDK GPU backend rebuild | L |

## Issues

### 1. ROV remains the expensive workaround for the black RTV world

- **Area:** GPU backend
- **Severity / Status:** High / Confirmed. Estimated impact: Largest GPU opportunity; gain unmeasured; effort L.
- **Where:** `src/BullyRecomp_app.h:59–66`; `../rexglue-sdk/src/graphics/d3d12/render_target_cache.cpp:176–190`; `../rexglue-sdk/src/graphics/pipeline/shader/dxbc_translator_om.cpp:364,1820,2127`.
- **Evidence:** The app sets `"render_target_path_d3d12", "rov"`. The SDK says “The ROV path is currently much slower generally” and implements depth/stencil and color handling in generated pixel shaders. `logs/perf/final-pacing-1.summary.json:21` records 14.738 ms GPU busy; `final-pacing-2.summary.json:21` records 14.139 ms.
- **Why it costs performance:** ROV emulates Xenos EDRAM/output merging through ordered UAV access and software depth/blending instead of using the host fixed-function output merger. Overlapping pixels and overdraw increase this cost. ROV also retains the compute resolve path: direct-host resolve is limited to host render targets (`render_target_cache.cpp:1198–1201`), while the fallback dispatches at `:1303`. These measurements show little GPU headroom, but do not isolate ROV from power limits or resolution.
- **Suggested fix:** Fix the RTV correctness problem in `../rexglue-sdk`, then rebuild and stage the GPU DLL; retain ROV until visual parity is established. The cause of the black world is **not established** by this audit. Compare the first divergent world draw, clear, depth target and resolved texture against ROV; inspect float24 conversion, gamma storage, MSAA and resolve destination/format. Do not describe a guessed format bug as the cause. Fix location: SDK rebuild.
- **How to verify:** Future PIX GPU capture of identical yard/town/interior scenes in ROV and RTV, plus `tools/perf-matrix.ps1`. Track GPU busy, output-merger/resolve duration, guest p95/p99 and image parity. Black RTV output is useful for diagnosis but is not a valid performance success.

### 2. The checked-in player INI selects 2× rendering, unlike fresh-file defaults

- **Area:** Settings / GPU / memory
- **Severity / Status:** High / Confirmed. Estimated impact: 4× pixel area; observed lower throughput; effort S.
- **Where:** `BullyRecomp.ini:10`; `src/settings_ini.cpp:41`; `play.cmd:3`; `../rexglue-sdk/src/graphics/d3d12/render_target_cache.cpp:207,1229,1303`.
- **Evidence:** Checked-in `resolution_scale = 2`; generated INI template `resolution_scale = 1`. EDRAM allocation multiplies by `scale_x * scale_y`. `logs/perf/scale2.summary.json` reports 51.295 swaps/s, exact p95 34.653 ms, p99 36.952 ms, jitter 6.759 ms; `final-pacing-1.summary.json` reports 59.746, 17.963, 19.182 and 1.189 respectively.
- **Why it costs performance:** 2× multiplies both dimensions: 1280×720 becomes 2560×1440, with four times the pixels, scaled EDRAM storage and resolve traffic, including scaled render-to-texture/shadow work. The 4K cap limits the maximum but does not make 2× inexpensive. `play.cmd` explicitly loads the checked-in INI. The two historical runs are directional evidence, **not a controlled 2× penalty**: capture, thermal state and cache history differ.
- **Suggested fix:** Make the shipped INI and first-run template agree on a measured performance default, preferably 1× on the tested laptop; expose 2×/3× as quality choices. Keep `scaled_texture_size_constants=true`, which fixes the shadow filter at higher scales rather than unnecessarily increasing the scale. Fix location: Repo INI / settings template.
- **How to verify:** Future capture-free `perf-matrix.ps1` 1×/2×/3× runs with identical scene, warm cache, cooling and power mode. Report effective scale, GPU busy, guest p99 and VRAM/working set. Confirm shadows remain correct.

### 3. Async pipeline creation still drains and waits at submission end

- **Area:** GPU shaders / CPU-GPU sync
- **Severity / Status:** High / Confirmed. Estimated impact: Cold-area hitch potential; duration unmeasured; effort L.
- **Where:** `../rexglue-sdk/src/graphics/d3d12/pipeline_cache.cpp:750–782,3246–3264`; `../rexglue-sdk/src/graphics/d3d12/command_processor.cpp:2407–2409,3417,3437`.
- **Evidence:** `EndSubmission()` calls `CreateQueuedPipelinesOnProcessorThread()` and then `rex::thread::Wait(creation_completion_event_.get(), false)`. This precedes `direct_queue->ExecuteCommandLists`. The processor drain invokes shader preparation and `CreateD3D12Pipeline`. Upstream `../xenia-edge/src/xenia/gpu/d3d12/pipeline_cache.cc:538–542` says “Don’t wait for pipeline creation” and only notifies workers.
- **Why it costs performance:** Skipping an unready draw avoids its immediate wait, but the same submission later performs queued compilation on the command processor and waits for busy workers. This can still delay unrelated GPU work and guest fence completion on a cold encounter. The documented elimination of multi-second freezes in the yard remains useful evidence; it does not prove fully nonblocking submission in unseen scenes.
- **Suggested fix:** Port a nonblocking submission design with safe pipeline ownership/publication and deferred destruction from the upstream approach. Do not merely remove waits relied upon by recorded command lists. This requires an SDK GPU rebuild and staging into the linked prebuilt setup. Fix location: SDK rebuild.
- **How to verify:** Future cold and warm town/interior/mission runs using `perf-run.ps1`, with CPU sampling of `GPU Commands` and `D3D12 Pipelines`. Check command-processor compilation/wait samples, worst frame, p99, skipped-draw artifacts and device stability.

### 4. Fence polling has lost the guest’s db16cyc backoff

- **Area:** Guest CPU / synchronization / codegen
- **Severity / Status:** Med / Confirmed. Estimated impact: CPU/power win; FPS gain not established; effort M.
- **Where:** Generated `sub_828D8DB8` (`generated/default/BullyRecomp_recomp.172.cpp:23279–23292`) and `sub_828D3C90` (`.144.cpp:23925–23940`); `../rexglue-sdk/src/codegen/builders/system.cpp:63–67`.
- **Evidence:** Eight `// db16cyc` comments emit no operation; the codegen builder explicitly treats the hint as having no effect. The remaining loop updates `ctx.xer`, CR and the counter. `logs/perf/cs-on-2.profile.txt:5–6` reports 19.82% self in `sub_828D8DB8`, 11.96% self in `sub_828D3C90`, and 30.35% inclusive in the latter.
- **Why it costs performance:** The guest deliberately backs off while waiting for GPU progress; the host spins without the hint, consuming CPU and competing with other threads. The latest sample sum is about 31.8% exclusive, not the older 75–95% from earlier configurations. Samples indicate residency, not a guaranteed recoverable fraction of frame time.
- **Suggested fix:** Lower the delay hint to a portable processor pause in SDK codegen and regenerate. Separately evaluate an ABI-preserving title-specific adaptive fence hook, short spin then GPU-completion notification, retaining guest timeout and progress behavior. A fixed millisecond sleep risks recreating the missed-vblank issue. Fix location: Codegen + regeneration; optional repo hook.
- **How to verify:** Future `perf-run.ps1 -Profile` / external WPR runs: main-thread fence residency, process CPU and power should fall without worsening guest p99 or latency. Run autoplay through multiple scenes; CPU savings alone are a valid outcome.

### 5. Host occlusion results submit and wait for the GPU immediately

- **Area:** GPU readback / synchronization
- **Severity / Status:** Med / Confirmed. Estimated impact: Potential serialization per query; rate unknown; effort L.
- **Where:** `../rexglue-sdk/src/graphics/command_processor.cpp:45`; `../rexglue-sdk/src/graphics/d3d12/command_processor.cpp:4994–5029,3140–3147`.
- **Evidence:** Occlusion is enabled by default. `EndGuestOcclusionQuery` resolves data, calls `EndSubmission(false)`, then `CheckSubmissionFence(query_submission)` before reading samples. The fence helper uses `SetEventOnCompletion` and `WaitForSingleObject(..., INFINITE)` when incomplete.
- **Why it costs performance:** Every actual query reaching this path breaks batching and makes the command processor wait for GPU completion. This can undermine the lazy-submit fix and extend guest fence spinning. **The code path is confirmed; frequent use by Bully and its contribution to current p99 are unmeasured.**
- **Suggested fix:** Count queries first. If important, implement asynchronous result retrieval with correct guest ordering or a verified title-specific occlusion fallback in the SDK. Do not globally fake visibility without validating rendering and gameplay effects. Fix location: SDK rebuild.
- **How to verify:** Future PIX/WPR plus `perf-run.ps1` A/B of occlusion handling: queries and submissions per guest frame, completion-wait duration and p99. Check hidden objects, lights and effects before retaining a faster path.

### 6. Memexport readback is enabled globally and its fast path can fully synchronize

- **Area:** GPU readbacks / settings / sync
- **Severity / Status:** Med / Confirmed. Estimated impact: Potential per-draw stalls; usage unknown; effort M.
- **Where:** `../rexglue-sdk/src/graphics/command_processor.cpp:62–69,184–189`; `../rexglue-sdk/src/graphics/d3d12/command_processor.cpp:2720–2730,2760,2818,2840–2864`; `logs/perf/final-pacing-1.log:19`.
- **Evidence:** The shared `readback_memexport=true` default takes precedence unless a legacy backend flag was explicitly supplied; the log confirms it is true. `readback_memexport_fast=true` still calls the full path when the prior slot is not ready (`:2863–2864`), and buffer replacement awaits queue completion (`:2818`). The full path copies GPU ranges and calls `AwaitAllQueueOperationsCompletion()`.
- **Why it costs performance:** The legacy `d3d12_readback_memexport=false` default does **not** mean effective memexport readback is off. First-use, changing ranges or late GPU completion may turn the double-buffered path into a synchronous stall. Bully’s memexport draw frequency/readback volume has not been measured, so active defaults do not establish a material frame cost.
- **Suggested fix:** Count actual memexport draws, bytes and full-path fallbacks; if important, redesign coherency/readback scheduling in the SDK. Only disable readback for a title after proving the guest does not consume the exported data. Do not blanket-disable a coherency feature. Fix location: SDK rebuild / repo flag experiment.
- **How to verify:** Future PIX/WPR of memexport draws and CPU readback waits plus `perf-matrix.ps1` with controlled shared flags. Track fallback rate, submissions, p99 and visual/gameplay correctness.

### 7. Guest file reads remain synchronous even for asynchronous handles

- **Area:** Kernel HLE / I/O / streaming
- **Severity / Status:** Med / Confirmed. Estimated impact: Measured 2–3% main samples; hitch potential; effort L.
- **Where:** `../rexglue-sdk/src/kernel/xboxkrnl/xboxkrnl_io.cpp:213–245`; `../rexglue-sdk/src/system/xfile.cpp:158`; `OPTIMIZATION.md`, Phase 4 “What is left”.
- **Evidence:** `if (true || file->is_synchronous())` always calls `file->Read` on the caller; an async request returns `X_STATUS_PENDING` only after the read is complete. `XFile` calls `ReadSync` directly into guest memory. Existing notes identify synchronous main-thread reads at about 2–3% of samples and occasional 50 ms frames.
- **Why it costs performance:** Disk access and cached-copy time block the guest caller. New assets in town, missions and interiors may make this more important than a warmed yard. Upstream `../xenia-edge/src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc:470` has the same unconditional synchronous branch, so it is not an available drop-in fix.
- **Suggested fix:** Implement real async requests/completion in the SDK while preserving guest event/APC/IOSB semantics; alternatively evaluate bounded read-ahead for observed asset access. Windows files already use cached I/O; do not prescribe unbuffered I/O or commit game assets. Fix location: SDK rebuild.
- **How to verify:** Future WPR file/disk traces aligned to exact guest frame CSV, especially first visits and mission loads. Check main-thread blocked time, read size/latency, p99 and frames over 50 ms, including completion correctness.

### 8. Vertex-only draws bypass the async shader path

- **Area:** GPU shaders
- **Severity / Status:** Med / Confirmed. Estimated impact: Cold shadow/depth hitch potential; effort M.
- **Where:** `../rexglue-sdk/src/graphics/d3d12/pipeline_cache.cpp:912–931`; `../xenia-edge/src/xenia/gpu/d3d12/pipeline_cache.cc:1149–1151`.
- **Evidence:** SDK `use_async` requires `pixel_shader != nullptr`; otherwise untranslated vertex shaders call `TranslateAnalyzedShader` synchronously. Upstream allows vertex-only async when its skip-draw behavior is enabled.
- **Why it costs performance:** An enabled async flag does not cover every pipeline. New depth-only/shadow passes may stall the command processor even after fixing submission-end draining. The frequency and duration of such passes in Bully are not yet measured.
- **Suggested fix:** Extend the SDK async path to vertex-only draws with safe missing-depth/shadow behavior. This is a separate branch from the submission-end wait in issue 3 and both need review. Fix location: SDK rebuild.
- **How to verify:** Future cold shadow/interior encounters, shader translation timings, command-processor stacks and worst guest frame; inspect transient missing shadows/depth and cache warm-up.

### 9. Eight pipeline workers serialize shader translation behind one mutex

- **Area:** GPU shaders / threading
- **Severity / Status:** Med / Confirmed. Estimated impact: Cold compile throughput limit; gain unknown; effort M.
- **Where:** `src/BullyRecomp_app.h:78`; `../rexglue-sdk/src/graphics/d3d12/pipeline_cache.cpp:3139–3149,369`.
- **Evidence:** Queued preparation holds `translation_request_lock_` across `AnalyzeUcode` and `TranslateAnalyzedShader(*shader_translator_, ...)`. The app chooses eight pipeline workers; startup replay has a separate worker translation design.
- **Why it costs performance:** Driver PSO creation may run concurrently, but runtime translation uses shared translator/compiler state under a single lock. More threads cannot scale this phase and can contend for CPU with gameplay. No current trace apportions translation versus driver PSO time.
- **Suggested fix:** Measure contention before changing the worker count. If translation dominates, use worker-local translator state and per-shader ownership; preserve synchronization for shared storage and publication. SDK rebuild required. Fix location: SDK rebuild.
- **How to verify:** Future cold-cache `perf-matrix.ps1` at 1/2/4/8 workers with WPR lock/CPU data. Compare queue drain time, translation throughput, guest p99 and total CPU, keeping power/cooling controlled.

### 10. CPU-uploaded shared-memory pages lose validity every frame

- **Area:** GPU uploads / memory
- **Severity / Status:** Med / Confirmed. Estimated impact: Repeated copy/bandwidth cost; bytes unknown; effort M.
- **Where:** `../rexglue-sdk/src/graphics/command_processor.cpp:40–42`; `../rexglue-sdk/src/graphics/d3d12/command_processor.cpp:3461`; `../rexglue-sdk/src/graphics/shared_memory.cpp:100–105`; `../rexglue-sdk/src/graphics/d3d12/shared_memory.cpp:318–330`.
- **Evidence:** `clear_memory_page_state=true` replaces valid flags with GPU-written flags at frame end: “Pages that are valid only because the CPU uploaded them lose their valid bit ... next frame re-reads them.” Uploads perform `memcpy` then `CopyBufferRegion`. Upstream `../xenia-edge/src/xenia/gpu/command_processor.cc:56` defaults this flag to false.
- **Why it costs performance:** Unchanged CPU-sourced vertex/index data requested next frame can be uploaded again. The behavior is explicit, but the volume of redundant Bully uploads is unmeasured. It is a coherency safeguard, not obviously disposable work.
- **Suggested fix:** A/B the flag for this title only after measuring upload bytes; retain the safe default until streaming and CPU/GPU write coherency are proven. Improve dirty-page tracking in the SDK if the repeated traffic is material. Fix location: Repo flag experiment / SDK rebuild.
- **How to verify:** Future PIX copy durations and upload-byte/page counts, WPR memcpy samples and `perf-matrix.ps1`. Exercise streaming, shadows and interiors and check visual parity, not only FPS.

### 11. Unknown memexport destinations invalidate the entire shared-memory range

- **Area:** GPU texture invalidation / memory
- **Severity / Status:** Med / Hypothesis. Estimated impact: Potential broad cache invalidation; frequency unknown; effort L.
- **Where:** `../rexglue-sdk/src/graphics/d3d12/command_processor.cpp:2709–2718`; `../rexglue-sdk/src/graphics/shared_memory.cpp:257–268`.
- **Evidence:** When a memexport shader has no known export ranges, the fallback is `RangeWrittenByGpu(0, SharedMemory::kBufferSize)`. That path fires watches for the affected pages. Known destinations instead invalidate only their ranges.
- **Why it costs performance:** A draw with unknown dynamic destinations can trigger full-range invalidation of memory watches/texture bindings and subsequent reloads, rather than a bounded range. The inspected code explains the correctness need; no log proves Bully reaches this branch frequently.
- **Suggested fix:** Improve destination analysis or supply correct bounded range tracking for measured shader patterns in the SDK. Keep conservative invalidation where the destination genuinely cannot be determined. Fix location: SDK shader analysis / rebuild.
- **How to verify:** Future counters for unknown-range memexport, watch fan-out and texture reload/upload bytes in town/missions. Correlate these with copy time and frame tails before attempting a narrower invalidation.

### 12. TimerQueue’s measured fix is not selected by fresh release presets

- **Area:** Build / kernel threading / deployment
- **Severity / Status:** Med / Confirmed. Estimated impact: ~0.14 core saved when fix is deployed; effort S.
- **Where:** `../rexglue-sdk/src/core/timer_queue.cpp:51–54,182–203`; `CMakeLists.txt:39–52`; `CMakeUserPresets.json:8–10`; `out/build/local-win-release/CMakeCache.txt:25`; `play.cmd:3`.
- **Evidence:** The source uses a high-resolution waitable timer plus wake event. `OPTIMIZATION.md` records TimerQueue 15.1% → 0.8% of one core and process 1.30 → 1.19 cores, with essentially unchanged p99. Current local cache explicitly selects the rebuilt SDK DLL directory. The project cache variable defaults empty; release presets do not set it and the override silently skips missing DLLs.
- **Why it costs performance:** **The current local setup appears to select the fix; do not re-report its old spin as an active source defect.** A fresh configured build can stage the old prebuilt runtime, or silently fall back when an override DLL is missing. The cache records intended staging, not proof of which binary an arbitrary player loaded.
- **Suggested fix:** Make runtime provenance and required override availability explicit in project staging/distribution, or replace the distributed prebuilt SDK with a compatible fixed build. Avoid hardcoding a machine-specific absolute path as a portable preset. Fix location: Repo staging / SDK rebuilt distribution.
- **How to verify:** Future baseline run recording loaded DLL identity/hash and TimerQueue thread CPU. Verify fresh and local builds both load the intended runtime; expect sub-1% TimerQueue CPU on the documented hardware, not an FPS jump.

### 13. Vblank emulation still sleeps coarsely and catches up in bursts

- **Area:** Frame pacing / kernel timing
- **Severity / Status:** Med / Hypothesis. Estimated impact: Possible residual tail latency; effort M.
- **Where:** `../rexglue-sdk/src/graphics/graphics_system.cpp:159–169`; `../rexglue-sdk/src/core/threading_win.cpp:96–100`; `src/BullyRecomp_app.h:79–85,248`.
- **Evidence:** With `vsync=false`, the vblank worker uses `guest_tick_frequency / 1000`, emits overdue `MarkVblank()` calls in a loop, then sleeps 1 ms. The Windows sleep path uses integer-millisecond `Sleep`. Only the optional project perf logger requests `timeBeginPeriod(1)` in the inspected production paths; the TimerQueue fix does not rewrite this worker.
- **Why it costs performance:** Late wakes may deliver several synthetic vblanks together and perturb the guest scheduler/fence progress. This is a plausible remaining p99 mechanism, **not proof that normal play still suffers the old 30 FPS WAIT_REG_MEM bug**. Benchmark logging also changes timer conditions relative to player launches.
- **Suggested fix:** Measure wake lateness; use a high-resolution deadline wait for vblank if important and decouple the pacing policy from command-processor polling. Preserve guest timing and avoid making host refresh a substitute for guest timebase. Fix location: SDK rebuild.
- **How to verify:** Future WPR context-switch/timer trace correlated to vblank timestamps, guest swaps and fence completion; compare visible player-like runs with and without the perf logger. Track late wakes and guest p99. Microsoft notes that timer resolution is process-sensitive and Windows 11 may ignore a higher resolution for occluded/minimized processes: [timeBeginPeriod documentation](https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod).

### 14. Short guest delays are rounded down to host milliseconds

- **Area:** Kernel waits / frame pacing
- **Severity / Status:** Med / Hypothesis. Estimated impact: Busy-yield or timing jitter if exercised; effort M.
- **Where:** `../rexglue-sdk/src/core/threading_win.cpp:96–105`; `../rexglue-sdk/src/system/xobject.cpp:197`; `../rexglue-sdk/src/system/xthread.cpp:1010`.
- **Evidence:** Sleep/SleepEx duration conversions and guest tick-to-ms conversions truncate sub-millisecond precision. A positive delay below 1 ms may reach `Sleep(0)`. Upstream `../xenia-edge/src/xenia/base/threading_win.cc:151–160` provides `NanoSleep` through `NtDelayExecution` with 100 ns duration units.
- **Why it costs performance:** A requested short delay may become a scheduler yield rather than a timed wait; subsequent guest polling can consume more CPU or shift deadlines. Bully’s actual short-delay call rate is not established. This differs from the separate 1 ms vblank worker in issue 13.
- **Suggested fix:** Audit delay/timeout rounding semantics, then provide a precise timed-wait path for positive short durations. Preserve zero-duration yield, infinite timeout and alertability behavior; do not assume high API resolution guarantees equally precise scheduling. Fix location: SDK rebuild.
- **How to verify:** Future WPR stacks/call duration histograms for guest delay and wait imports. Confirm the short-wait path is used before changing it; compare caller CPU, timeout accuracy and guest p99 in town/interiors.

### 15. The 20 µs critical-section spin applies to every guest lock

- **Area:** Kernel HLE / locks
- **Severity / Status:** Med / Hypothesis. Estimated impact: Potential CPU waste outside measured yard; effort M.
- **Where:** `src/guest_locks.cpp:51–74`; `OPTIMIZATION.md`, Phase 4 heap-lock results.
- **Evidence:** The overridden import has no heap-address filter; every nonrecursive guest critical-section entry can spin until `__rdtsc() < deadline`, with default 20 µs. The documented benefit reduces kernel-wait samples from 15–19% to 3–6%.
- **Why it costs performance:** The budget was chosen for short heap holds in the yard. Long holds or a descheduled owner cannot benefit from spinning; constrained-core systems or other scenes may lose CPU and owner execution time. **Starvation is not demonstrated:** CAS only accepts `lock_count == -1`, and the original wait/recursion protocol remains the fallback.
- **Suggested fix:** Retain the proven fix; instrument successes, fallback waits and hold/owner scheduling. Tune the budget or restrict spinning to measured short-hold locks if needed. Avoid increasing it blindly. Fix location: Repo src/guest_locks.cpp.
- **How to verify:** Future `perf-matrix.ps1` budgets 0/5/10/20/40 µs in yard, town and interiors, including a CPU-constrained case. Compare total CPU, kernel wait time, spin successes and guest p99; require correctness/soak parity.

### 16. Volatile guest copies prevent broad copy optimization

- **Area:** Guest CPU / memory / codegen
- **Severity / Status:** Med / Hypothesis. Estimated impact: Small yard cost; streaming gain unknown; effort M.
- **Where:** `generated/default/BullyRecomp_pch.h:127–137`; generated `sub_8289E0B0` (`generated/default/BullyRecomp_recomp.72.cpp:24103`); `../rexglue-sdk/src/codegen/builders/system.cpp:150–160`.
- **Evidence:** All scalar RAM loads/stores use volatile pointers and multibyte byte-swap builtins. The sampled copy routine uses scalar 64-bit/byte loops; `dcbt`/`dcbtst` prefetch hints are comment-only no-ops. `logs/perf/cs-on-2.profile.txt` attributes 1.75% self samples to `sub_8289E0B0`.
- **Why it costs performance:** Volatile constrains combining, redundant-load elimination and vectorization of private copies. Lost prefetch hints may matter for large streams, but manual host prefetch can also waste bandwidth. Required endianness conversion itself is not a defect, and emitted C++ does not prove the final instruction cost.
- **Suggested fix:** Prefer an ABI-preserving host implementation for a verified guest memcpy-like function, after proving overlap/bounds/return/register behavior. Alternatively add codegen treatment only for proven private guest RAM. **Do not globally remove volatile from MMIO, fence words or shared guest data.** Fix location: Repo host hook or codegen + regeneration.
- **How to verify:** Future profile of copy functions during streaming, instruction/memory bandwidth measurements and town/interior p99. Validate guest boundaries, physical aliases, overlap semantics and register results before retaining a hook.

### 17. Supported register-localization options are disabled

- **Area:** Guest CPU / CR-XER / codegen
- **Severity / Status:** Med / Hypothesis. Estimated impact: Unmeasured compute reduction; effort M.
- **Where:** `../rexglue-sdk/include/rex/codegen/config.h:92–98`; `../rexglue-sdk/src/codegen/config.cpp:105–116`; `BullyRecomp_manifest.toml`; generated `sub_828D8DB8` and `sub_828D3C90`.
- **Evidence:** `ctr_as_local`, `xer_as_local`, `cr_as_local`, `non_argument_as_local` and `non_volatile_as_local` default false and are not enabled by the project manifest. Hot functions continually manipulate `ctx.r*`, `ctx.cr*` and `ctx.xer`, including hint-loop carry/CR state.
- **Why it costs performance:** Context fields can escape through guest calls, limiting scalar promotion and dead flag elimination. The compiler may already eliminate unused state, so source-level materialization is not itself a measured instruction-count defect.
- **Suggested fix:** Selectively enable supported localization in project codegen config and regenerate. SDK `builders/context.cpp:45–48` accounts for register-sharing funclets, but ABI, exception and shared-register behavior still require validation. Fix location: Repo codegen config + regeneration.
- **How to verify:** Future matched builds, assembly inspection and `perf-run.ps1` profiles of exclusive game-work time rather than total fence residency. Exercise branch checks and autoplay across all scenes before relying on gains.

### 18. Weak/noinline boundaries and no LTO or PGO limit cross-function optimization

- **Area:** Build / guest CPU
- **Severity / Status:** Med / Hypothesis. Estimated impact: Unknown; likely secondary in yard; effort M.
- **Where:** `generated/default/BullyRecomp_pch.h:74–77`; `generated/default/sources.cmake:9–181`; `out/build/local-win-release/build.ninja:202`; `CMakePresets.json`; `../rexglue-sdk/cmake/rexglue_helpers.cmake:59–62`.
- **Evidence:** 173 recompilation translation units; guest symbols are weak/noinline aliases. Actual generated flags include `-O3 -DNDEBUG -msse4.1 -fasync-exceptions -gline-tables-only`; inspected compile/link rules contain no LTO/ThinLTO or PGO configuration.
- **Why it costs performance:** Separate units and replaceable noinline guest entries restrict interprocedural constant/state optimization, especially save/restore helper calls. On Windows these are **same-address aliases**, not an extra wrapper call. ThinLTO alone may not overcome intentional weak/noinline boundaries. This is an optimization opportunity, not evidence Release is miscompiled.
- **Suggested fix:** Experiment with ThinLTO and representative PGO in project CMake; keep replaceable entry points for hooks and permit safe internal calls only where proven. Whole-program/unity approaches must be judged against the 182 MB source/build-memory cost. SIMD targeting is covered separately in issue 19. Fix location: Repo CMake; possible codegen / SDK rebuild.
- **How to verify:** Future yard/town/interior profiles and binary/I-cache measurements, plus build time/memory and hook correctness. Train PGO on diverse routes, not only the yard; retain changes only for measured gains.

### 19. Some guest FP and VMX128 operations still lower to scalar or expensive exact math

- **Area:** Guest SIMD / build / codegen
- **Severity / Status:** Med / Hypothesis. Estimated impact: Potential animation/compute win; hotness unknown; effort M.
- **Where:** `../rexglue-sdk/src/codegen/builders/vector.cpp:128–139,173–179,889–902,929–935,994–1007`; `generated/default/BullyRecomp_recomp.102.cpp:15593–15597`; `.163.cpp:25020–25025`.
- **Evidence:** `vsraw`/`vsrw` have “TODO vectorize” lowering; sampled `vsraw` uses four scalar shifts. `vlogefp` emits four `log2f` calls. Reciprocal/rsqrt estimates use full SIMDe division/sqrt. SSE4.1-only target settings cannot use native AVX2 variable word shifts.
- **Supplementary evidence:** The supplied optimization document prompted a check of scalar guest math too. Profiled `sub_82722348` (`generated/default/BullyRecomp_recomp.57.cpp:19341–19344,19366–19367,19375–19382`) emits `std::fma` and `double(float(1.0 / sqrt(...)))`, followed by the guest's own refinement arithmetic. It accounts for 1.35% self samples in `logs/perf/cs-on-2.profile.txt:13`. SDK `../rexglue-sdk/src/codegen/builders/floating_point.cpp:200–258,275–277` supplies these lowerings. The inspected generated flags (`out/build/local-win-release/build.ninja:202`) do not enable FMA explicitly. Whether calls remain library calls, or hardware FMA is selected, needs final assembly inspection; source alone cannot establish that cost. Include hardware-FMA targeting and guest estimate accuracy in the existing ISA/math experiment, preserving fused rounding and special-value semantics. Do not replace a fused operation with separate multiply/add solely for speed.
- **Why it costs performance:** Some guest vector operations expand into several host scalar instructions or library calls; approximate guest math is sometimes computed through more expensive exact operations. Broad VMX lowering already uses SIMD, and these samples are not established hot functions in the current yard profile.
- **Suggested fix:** Optimize measured VMX operations in SDK codegen/helpers, regenerate and consider an explicitly compatible AVX2/x86-64-v3 build option. Preserve guest NaN, subnormal, rounding and estimate error behavior; blanket fast-math or approximate substitutions are not justified. Fix location: SDK codegen + regeneration; optional repo ISA flags.
- **How to verify:** Future animation/culling/particle CPU profiles in dense scenes, then exclusive cycles/assembly for the owning functions. Run numerical and gameplay regression checks on each changed lowering; report p99 only if these operations are actually frame-critical.

### 20. First-touch GPU shared-memory commitment may hitch during streaming

- **Area:** GPU memory / tiled resources
- **Severity / Status:** Med / Hypothesis. Estimated impact: First-visit tail risk; not measured; effort M.
- **Where:** `../rexglue-sdk/src/graphics/d3d12/shared_memory.cpp:24,281,297,318`; `../rexglue-sdk/include/rex/graphics/shared_memory.h:110`.
- **Evidence:** Tiled shared memory defaults on; first-needed ranges create heaps and call `UpdateTileMappings`. Physical allocation granularity is 4 MiB. Upload pages are pooled afterwards.
- **Why it costs performance:** Heap creation, tile mapping and first upload can add CPU/driver/GPU work when an unseen area requests data. Reserved address space is not committed VRAM, so the nominal shared/scaled resource size must not be presented as actual memory consumption.
- **Suggested fix:** Measure first-touch costs; consider bounded precommit/prefetch or a non-tiled comparison if useful on the target GPU. Retain memory limits and coherency. The earlier matrix found no material yard gain from tiled-memory changes. Fix location: SDK rebuild / measured flag experiment.
- **How to verify:** Future cold town/interior transitions with PIX heap/tile-map events, residency budgets and copy timings; WPR page faults and exact-frame outliers. Compare warmed reruns to distinguish first touch from sustained rendering.

### 21. Guest heap allocation scans page state while holding the heap mutex

- **Area:** Memory / kernel HLE
- **Severity / Status:** Med / Hypothesis. Estimated impact: Fragmentation/streaming risk; unmeasured; effort L.
- **Where:** `../rexglue-sdk/src/system/xmemory.cpp:1418–1419,1469–1496,1523–1535`.
- **Evidence:** `AllocRange` takes `heap_mutex_`, scans candidate pages and requested free ranges, then updates page state and host access. Some host-page reconciliation additionally uses a global lock only when guest pages are smaller than host pages (`:910–914`).
- **Why it costs performance:** Fragmented allocations can lengthen scanning and lock holds, while commit/state work may create first-touch cost. Do not claim every Windows allocation holds a global reconciliation lock: the ordinary page-size case returns an empty lock. No current profile proves allocation scans or page faults cause the remaining tails.
- **Suggested fix:** First count allocation sizes and scan lengths. If material, maintain a free-range index or improve allocation search while preserving guest address/alignment semantics. Keep existing bulk host-protection batching; it is already present around `:1000`. Fix location: SDK rebuild.
- **How to verify:** Future WPR heap/page-fault sampling and guest allocator instrumentation in town/mission transitions. Track allocation latency/scan lengths and guest p99; verify address/layout and memory-usage behavior.

### 22. Guest frame pacing still differs from displayed pacing and latency

- **Area:** Presentation / frame pacing
- **Severity / Status:** Med / Confirmed. Estimated impact: Observed uneven display cadence; fix unknown; effort M.
- **Where:** `logs/perf/final-pacing-1.summary.json`; `logs/perf/final-pacing-2.summary.json`; `OPTIMIZATION.md`, Phase 4 remaining GPU/presentation observations; `../rexglue-sdk/src/ui/d3d12/d3d12_presenter.cpp:1150–1160`; `../rexglue-sdk/src/ui/presenter.cpp:1651`.
- **Evidence:** Both summaries show about 60 presents/s and display present p99 near 17.2 ms, while exact guest p99 is 19.182/33.973 ms. `DisplayChangeStdMs` is 8.761/8.382. Notes record about 30 ms display latency and a 144 Hz panel without VRR. Host code actually calls `Present(0, ...ALLOW_TEARING...)`; the UI tick path can use `WaitForVBlank()`.
- **Why it costs performance:** Repeated presentation can hide slower or uneven game swaps, and a fixed 144 Hz display cannot place every 60 Hz update on evenly spaced refresh boundaries. This is a measured pacing/latency problem, **not evidence of hardcoded host Present(1)**. Power-capped GPU execution and UI tick/mailbox timing may also contribute.
- **Suggested fix:** Measure the entire guest swap → GPU completion → mailbox → host present → display chain. Compare a suitable 60/120 Hz or functioning VRR mode; tune presenter policy only after tracing it. Do not revert guest `vsync=false` based on host display metrics alone. Fix location: SDK presenter or display settings.
- **How to verify:** Future `perf-run.ps1` with PresentMon display-change/latency fields, exact guest CSV and PIX/WPR events. Track fresh displayed frames, p99 and end-to-end latency rather than `AvgFps` alone; use identical power/refresh conditions.

### 23. Audio submission holds the global region across a contended driver path

- **Area:** Audio / threading
- **Severity / Status:** Low / Hypothesis. Estimated impact: Possible small lock-tail cost; effort M.
- **Where:** `src/audio_compat.cpp:7–11`; `../rexglue-sdk/src/audio/sdl/sdl_audio_driver.cpp:116–136,183–222`.
- **Evidence:** `SubmitFrameIfActive` acquires `global_critical_region_` through `driver->SubmitFrame`. The driver takes `frames_mutex_`, copies a frame and takes the mutex again. The callback holds the queue mutex while downmixing and submitting to SDL. It allocates a frame only when the recycle pool is empty.
- **Why it costs performance:** Submit can wait behind callback processing while the compatibility layer also holds a broader lifetime-protection lock. A slow conversion/SDL queue operation could widen contention. There is **no direct buffer-full wait in SubmitFrame**, and steady per-frame heap allocation is not proven because buffers recycle.
- **Suggested fix:** Measure lock duration; shorten driver queue critical sections and consider bounded preallocated audio buffers. Preserve the compatibility fix’s client/driver lifetime protection when narrowing its lock. Fix location: Repo audio compatibility + SDK rebuild.
- **How to verify:** Future WPR AudioWorker/SDL stacks and global-region/frames-mutex waits, plus sound correctness under load. Compare audio CPU, submit tails and guest p99, including underruns and client teardown.

### 24. Each timebase read crosses into the shared clock and may lock

- **Area:** Guest CPU / mftb / kernel timing
- **Severity / Status:** Low / Hypothesis. Estimated impact: Unknown; probably small in yard; effort M.
- **Where:** `generated/default/BullyRecomp_pch.h:293`; `../rexglue-sdk/src/core/clock.cpp:20,66–89,158–160`; `../rexglue-sdk/src/core/clock_win.cpp:24–31`.
- **Evidence:** `REX_QUERY_TIMEBASE()` calls runtime `Clock::QueryGuestTickCount`. The scaled path samples QPC and attempts `tick_mutex_`; on contention it blocks on that mutex. `clock_no_scaling` defaults false.
- **Why it costs performance:** A guest hardware timebase read becomes a DLL call, host clock query and shared synchronization. High-frequency timing loops could magnify this, but neither mftb count nor clock mutex time has been isolated in the current profile.
- **Suggested fix:** Design a fast monotonic clock path preserving pause/scaling semantics if profiling warrants it. Test `clock_no_scaling` only after checking timing requirements; do not cache timebase values across timing-sensitive loops. Fix location: SDK rebuild.
- **How to verify:** Future QPC/clock samples and mftb call counts, especially vblank and guest timing functions. Track per-read cycles, contention and game-time behavior alongside guest p99.

### 25. The 4× anisotropic override changes eligible guest texture filtering

- **Area:** Settings / GPU texture sampling
- **Severity / Status:** Low / Hypothesis. Estimated impact: Small quality/performance tradeoff; effort S.
- **Where:** `BullyRecomp.ini:19`; `src/settings_ini.cpp:50`; `../rexglue-sdk/src/graphics/d3d12/texture_cache.cpp:973–976,997–999`.
- **Evidence:** `anisotropic_override = 3` maps to anisotropic filtering with `MaxAnisotropy = 4` on eligible mipmapped linear textures.
- **Why it costs performance:** This may increase sampling/bandwidth relative to the guest’s setting, particularly on oblique surfaces and with 2× rendering. It can also reduce a stronger guest setting; the net cost is not known.
- **Suggested fix:** Keep a measured quality default or use `-1` to honor the game. Compare 0/3/-1 rather than assuming override removal always improves performance. Fix location: Repo INI / settings template.
- **How to verify:** Future identical-route `perf-matrix.ps1` with PIX sampling-pass timings/GPU busy and side-by-side image inspection; expect any gain to be smaller than resolution/ROV changes.

### 26. Compression-query errors still format and synchronously flush logs

- **Area:** Kernel HLE / logging
- **Severity / Status:** Low / Confirmed. Estimated impact: Known small cost; no material FPS gain shown; effort S.
- **Where:** `../rexglue-sdk/src/kernel/xboxkrnl/xboxkrnl_io_info.cpp:151–159`; `../rexglue-sdk/src/core/logging.cpp:178–181,271`; `../rexglue-sdk/include/rex/logging/types.h:84`.
- **Evidence:** Every compression-information request logs `unimplemented` and returns `X_STATUS_INVALID_PARAMETER`. `logs/perf/final-pacing-1.log:52` onward and `logs/autoplay/soak-10min/game.log` each contain 866 such entries. The logger is synchronous, uses `basic_file_sink_mt`, and defaults to flushing at info and above.
- **Why it costs performance:** Errors survive the player’s warn-level filter and cause formatting, sink locking and a file flush on the guest caller. A flush here is buffered-file flushing, not necessarily a durable disk flush. The same 866 count in a long soak suggests startup/loading concentration; do not extrapolate the old ~12/s observation across the entire current session. Existing notes judge the cost negligible.
- **Suggested fix:** Implement a correctly shaped uncompressed response where valid, or rate-limit the repeated diagnostic without hiding unrelated errors. Consider buffered/async logging policy in the SDK if traces show meaningful caller delay. Fix location: SDK rebuild.
- **How to verify:** Future log count and WPR logging/file-write wall time at startup and scene loads, with p99 comparison. Expect quieter logs first; do not promise FPS improvement.

### 27. The lock override calibrates for 5 ms and times even a free lock

- **Area:** Guest locks / startup CPU
- **Severity / Status:** Low / Confirmed. Estimated impact: One-time 5 ms; small repeated entry overhead; effort S.
- **Where:** `src/guest_locks.cpp:34–46,61–65`.
- **Evidence:** First use busy-polls QPC until `frequency / 200` ticks, about 5 ms. `deadline = __rdtsc() + SpinBudgetTicks()` executes before the first free-lock CAS when the owner is not self.
- **Why it costs performance:** Calibration occurs on a guest critical-section call, including an uncontended one, and every nonrecursive fast entry computes a deadline that is unnecessary if the first CAS succeeds. This is a side cost of the otherwise useful spin fix.
- **Suggested fix:** Attempt the free-lock CAS before computing the spin deadline; initialize timing outside latency-sensitive guest execution. Preserve owner/recursion publication and fallback protocol. Fix location: Repo src/guest_locks.cpp.
- **How to verify:** Future first-lock/startup timing and thunk cycle measurements, then `perf-run.ps1` and autoplay. Compare uncontended entry CPU separately from contended wait savings.

### 28. Multi-object waits allocate vectors in three layers

- **Area:** Kernel HLE / memory / threading
- **Severity / Status:** Low / Confirmed. Estimated impact: Small per-call allocation; rate unknown; effort S.
- **Where:** `../rexglue-sdk/src/kernel/xboxkrnl/xboxkrnl_threading.cpp:876,902`; `../rexglue-sdk/src/system/xobject.cpp:265`; `../rexglue-sdk/src/core/threading_win.cpp:184`.
- **Evidence:** HLE builds a `vector<object_ref<XObject>>`, the object layer builds a `vector<WaitHandle*>`, and Windows translates again into `vector<HANDLE>`.
- **Why it costs performance:** Guest multiwaits pay heap allocation and repeated handle-list construction even for short lists. Aggregate cost depends on unmeasured call frequency; this is not evidence all waits are expensive.
- **Suggested fix:** Use bounded small/fixed-capacity storage or a span-based internal path within the guest/native wait limits, retaining object references and error/alertability semantics. Fix location: SDK rebuild.
- **How to verify:** Future ETW heap allocation stacks and multiwait call rates, particularly audio/kernel threads. Compare allocation count, wait setup CPU and total CPU; guest p99 may not move.

### 29. Screenshot capture adds full-frame queue work and allocation to benchmarks

- **Area:** Diagnostics / GPU readback / memory
- **Severity / Status:** Low / Confirmed. Estimated impact: Periodic benchmark disturbance; absent in play; effort S.
- **Where:** `tools/autoplay.ps1:35–39`; `src/BullyRecomp_app.h:121,164–176`; `../rexglue-sdk/src/ui/d3d12/d3d12_presenter.cpp:233,242,250,274,298–303,320`.
- **Evidence:** Autoplay always enables capture. `final-pacing-1.summary.json` and `scale2.summary.json` also include a capture directory. Each SDK capture creates a readback resource, allocator and command list, copies the full image on the direct queue and waits for completion; the app writes PPM via one `output.write(..., 3)` per pixel.
- **Why it costs performance:** Periodic full-frame readback competes on the rendering queue and allocates/converts large image buffers; millions of tiny stream writes add CPU overhead at high resolutions. A wait on the capture thread is not automatically a main-thread block, but the extra queue work can affect tails. Normal play returns early when the capture directory is empty.
- **Suggested fix:** Use capture-free timing runs and separate screenshot validation sessions. If capture timing itself matters, reuse resources and buffered whole-row/image output; SDK changes require rebuilding. Fix location: Repo tools; SDK capture reuse if needed.
- **How to verify:** Future `perf-run.ps1` without capture versus matched capture interval, correlate capture timestamps with frame outliers. For autoplay, use an explicit empty capture-directory override in a timing-only run and separately retain visual coverage.

### 30. The sampling profiler suspends the measured thread while allocating and unwinding

- **Area:** Diagnostics / measurement bias
- **Severity / Status:** Low / Confirmed. Estimated impact: Profile-only perturbation; magnitude unknown; effort M.
- **Where:** `src/bully_profiler.h:76–107`; `src/BullyRecomp_app.h:224–230`.
- **Evidence:** About every 2 ms, `SuspendThread` precedes stack unwinding, `vector` growth and possible module-name cache insertion; `ResumeThread` happens only after that work.
- **Why it costs performance:** This changes the scheduling/lock-hold timing of the thread being studied. Allocation and cache work during suspension can lengthen pauses and can be problematic if the suspended thread holds a resource needed by profiling. It is off unless both perf logging and `bully_profile` are requested.
- **Suggested fix:** Prefer external ETW sampling for authoritative pacing measurements; preallocate profiler scratch/cache work and minimize suspended duration if retaining the in-process sampler. Fix location: Repo src/bully_profiler.h.
- **How to verify:** Future paired profile-on/off `perf-run.ps1` and WPR captures. Compare p99/worst/CPU and sampled hot-function rankings; report profiler configuration with each result.

### 31. The perf logger itself wakes at 1 kHz and changes timer resolution

- **Area:** Diagnostics / CPU / measurement bias
- **Severity / Status:** Low / Confirmed. Estimated impact: Perf-only CPU/wakeup cost; effort S.
- **Where:** `src/BullyRecomp_app.h:224–225,248,265,314–359`.
- **Evidence:** When enabled, it calls `timeBeginPeriod(1)`, sleeps 1 ms between polls, flushes output each second and enumerates/opens process threads each second. It consumes a swap ring that can hold about 17 seconds at 60 swaps/s.
- **Why it costs performance:** The instrumentation contributes CPU, wakeups, file work and timer-policy changes. Polling every millisecond is unnecessary for collecting timestamps already written by the swap hook, though polling-derived per-second stats currently depend on it.
- **Suggested fix:** Drain the QPC ring less often or through a lightweight notification and calculate all frame statistics from exact timestamps; keep coarse CPU/thread sampling. Match timer policy between player and benchmark paths when investigating wake granularity. Fix location: Repo src/BullyRecomp_app.h.
- **How to verify:** Future external WPR/PresentMon logger-on/off pairs; compare instrumentation thread CPU, process CPU, wake count and frame tails. Ensure no ring loss and compare exact-frame CSV rather than polling-derived percentiles.

### 32. The frame timestamp hook runs even without perf logging

- **Area:** Diagnostics / guest hot path
- **Severity / Status:** Low / Confirmed. Estimated impact: Tiny cost per swap; effort S.
- **Where:** `config/perf-hooks.toml:3–5`; `src/main.cpp:11–16`.
- **Evidence:** The unconditional midasm hook calls `QueryPerformanceCounter`, writes the ring and publishes the atomic swap counter for every VdSwap, without checking `bully_perf_log`.
- **Why it costs performance:** Normal play still pays this instrumentation cost, unlike the gated logger/profiler/capture threads. At about 60 swaps/s the cost is expected to be tiny; no meaningful FPS loss is measured.
- **Suggested fix:** Cache an enable flag at setup and skip timestamp recording when not needed, preserving any counter consumers; alternatively retain the small cost intentionally for diagnostics. Fix location: Repo src/main.cpp or codegen hook config.
- **How to verify:** Future hook-on/off CPU microtiming and ordinary `perf-run.ps1` frame pacing. Do not treat this as a priority before GPU or synchronization work.

### 33. Explicit RE debug modes scan memory or do synchronous per-draw I/O

- **Area:** Diagnostics / logging / memory
- **Severity / Status:** Low / Confirmed. Estimated impact: Potentially large when explicitly enabled; effort S.
- **Where:** `src/debug_tools.cpp:47–55,76–83,143–164,183–233`; `src/gpu_diagnostics.cpp:23,33–60`; `CMakeLists.txt:53–58`; `src/BullyRecomp_app.h:94,116`.
- **Evidence:** Memory scanners traverse committed guest regions every 5 seconds; write-watch setup suspends threads and polls every 200 ms. Diagnostic GPU mode opens `capture-draws.txt` each frame and writes registers/fetch buffers for up to 96 captured draws. Debug threads are detached and continue polling/scanning even after reporting initial matches.
- **Why it costs performance:** Scans pollute caches and consume bandwidth; watchpoints cause exceptions/unwinding and thread suspension; synchronous draw dumps execute on the GPU command processor. These paths are **explicitly gated and absent from normal player settings**, with the GPU backend also build-option gated.
- **Suggested fix:** Use separate RE/visual sessions, limit scan ranges and stop tools after collecting evidence; add managed cancellation if diagnostic sessions need it. Do not treat debug-mode timings as clean baselines. Fix location: Repo debug tools / diagnostic backend.
- **How to verify:** Future clean autoplay/perf runs with all debug flags unset versus a deliberate diagnostic run, recording tool-thread CPU and outliers. Confirm no normal-play scanner/watch thread or diagnostic backend exists.

### 34. Native D3D12 PSO blobs are not explicitly persisted

- **Area:** GPU cache / startup
- **Severity / Status:** Low / Hypothesis. Estimated impact: Small observed warm-start opportunity; effort M.
- **Where:** `../rexglue-sdk/src/graphics/d3d12/pipeline_cache.cpp:251–254,329–331,2691–2692,3032`; `logs/perf/final-pacing-1.log:26`.
- **Evidence:** The SDK persists title/path-specific `.xpso` descriptions and `.xsh` shaders; the PSO descriptor is zeroed and `CreateGraphicsPipelineState` is called. No explicit CachedPSO/pipeline-library use was found. One warm log rebuilds 405 pipelines from storage in 54 ms.
- **Why it costs performance:** Startup replays PSO creation rather than explicitly restoring native device blobs. The driver may already cache these internally, and the measured warm replay is only 54 ms, so large startup or frame gains are not established.
- **Suggested fix:** Consider a device/driver/version-keyed native PSO cache only after tracing replay cost. Preserve invalidation and fallback behavior; do not confuse the host cache with failing guest `D:\\Shaders\\Generated` probes. Fix location: SDK rebuild.
- **How to verify:** Future cold/warm startup and new-area cache traces, recording PSO creation duration and cache hit rates. Compare startup separately from steady-state guest p99.

### 35. The scaled-shadow fix scans texture bindings and can reupload constants

- **Area:** GPU CPU bindings / existing fix side cost
- **Severity / Status:** Low / Confirmed. Estimated impact: Small at scale >1; unmeasured; effort S.
- **Where:** `../rexglue-sdk/src/graphics/d3d12/command_processor.cpp:4145–4187,4194–4212`; `BullyRecomp.ini:13`.
- **Evidence:** At scale >1 with the flag enabled, `UpdateBindings` scans sampled texture bindings, collects up to four scaled sizes, hashes a size signature and dirties the pixel constant buffer when it changes. Uploaded constant components are compared against texture sizes/reciprocals.
- **Why it costs performance:** The fix adds binding-side CPU work even while looking for relevant scaled textures, and changing size signatures can invalidate otherwise reusable constant buffers. It is bounded and gated, and it fixes visibly broken shadows; no measurable regression has been isolated.
- **Suggested fix:** Retain the correctness fix. If profiling finds material cost, cache scaled-size metadata by shader/binding/fetch generation and specialize known shadow constants rather than broadly scanning every draw. Preserve texture-reuse invalidation and the correct filter at higher scales. Fix location: SDK GPU plugin rebuild.
- **How to verify:** Future scaled-shadow orbit/yard runs and `perf-run.ps1` at 1×/2× with binding CPU/constant-upload counts. Compare visual parity; disabling the flag at 2× merely to win a timing number is not a valid fix.

### 36. The primary perf script bypasses the player INI

- **Area:** Settings / measurement coverage
- **Severity / Status:** Low / Confirmed. Estimated impact: Can hide shipped quality-setting costs; effort S.
- **Where:** `tools/perf-run.ps1:53–55`; `play.cmd:3`; `tools/autoplay.ps1:35–42`; `BullyRecomp.ini:10,19`.
- **Evidence:** `perf-run.ps1` always passes `--bully_ini none`; `play.cmd` loads the checked-in INI and autoplay loads it unless `-NoIni` is supplied. The current INI requests 2× resolution and a 4× anisotropic override.
- **Why it costs performance:** An unqualified 'default' perf run describes app/SDK defaults, not necessarily the player's shipped settings. Historical summaries also omit effective DLL identity and full effective cvar snapshots; old runs predate current settings. This is a comparability gap, not extra game work.
- **Suggested fix:** Keep a controlled no-INI matrix, but also measure an explicitly player-equivalent baseline and record effective settings/runtime provenance. Pass the player INI explicitly through ExtraArgs for a future script run or add a deliberate mode later. Fix location: Repo tools/perf-run.ps1.
- **How to verify:** Future paired `perf-run.ps1` controlled and player-INI runs plus autoplay, recording final scale/anisotropy/async/readback flags, DLL hashes and exact guest metrics. Do not compare differently configured runs as a clean A/B.

### 37. Floating-point mode guards recur at control-flow and call boundaries

- **Area:** Guest floating point / codegen
- **Severity / Status:** Low / Hypothesis. Estimated impact: small guard/transition overhead, frequency and frame contribution unknown; effort M.
- **Where:** `../rexglue-sdk/src/codegen/builders/context.cpp:332–340`; `../rexglue-sdk/src/codegen/function_graph.cpp:488,512–514`; `../rexglue-sdk/src/codegen/builders/control_flow.cpp:82,105,190`; `../rexglue-sdk/include/rex/ppc/context.h:180–201`; `../rexglue-sdk/include/rex/platform/fpscr.h:28–46`.
- **Evidence:** Codegen already suppresses repeated same-mode emission within a tracked sequence, but resets CSR knowledge at labels and calls. Generated `sub_82722348` (`generated/default/BullyRecomp_recomp.57.cpp:19289,19357`) repeats `disableFlushMode()` after a label although the shown preceding math is scalar. The helper checks cached `csr` bits and writes host MXCSR only on an actual change; known mode transitions use unconditional setters. Another sample (`.107.cpp:107,115,123`) guards again after guest calls. `cs-on-2.profile.txt:13` attributes 1.35% self time to the whole `sub_82722348`, not to its mode guards.
- **Why it costs performance:** Lost compile-time mode knowledge can add branches/context reads at otherwise same-mode merges; genuine scalar/VMX transitions write hardware FP state. This is an unmeasured optimization lead, not evidence that every guest FP instruction writes MXCSR. ReXGlue already implements hardware FP-mode handling, so the supplied document's suggestion to add it from scratch is inapplicable.
- **Suggested fix:** Measure mode-guard and actual transition counts. If worthwhile, propagate FP-mode facts through proven control-flow joins and summarize callees that preserve the mode; retain guarded handling at unknown boundaries. Scalar FPU and VMX request different flush modes (`builders/floating_point.cpp:22`, `builders/vector.cpp:76`), so a single global startup setting cannot replace them safely. Fix location: SDK codegen/helpers, followed by regeneration; update installed headers/rebuild as needed if helpers change.
- **How to verify:** Future assembly inspection and WPR/CPU profiles in animation/collision-heavy town/interior scenes; compare emitted guards, actual MXCSR writes and exclusive game-work cycles. Validate subnormal, rounding, mixed FPU/VMX and call-boundary behavior before comparing `perf-run.ps1` p99 and autoplay correctness.

### 38. Host draw recording is still performed per surviving guest draw

- **Area:** GPU command recording / potential batching
- **Severity / Status:** Low / Hypothesis. Estimated impact: possible CPU savings in draw-heavy scenes; unmeasured; effort L.
- **Where:** `../rexglue-sdk/src/graphics/d3d12/command_processor.cpp:2277,2401–2420,2637,2696,3425–3437`.
- **Evidence:** `IssueDraw` configures/checks pipeline and binding state, then records `D3DDrawInstanced(..., 1, ...)` or `D3DDrawIndexedInstanced(..., 1, ...)` for the surviving guest draw. Commands are already deferred into a shared command list and submitted together; this is not an isolated GPU queue submission per draw. The supplied optimization document's batching section (`C:/Users/Admin/Downloads/BullyRecomp Performance Optimization.md:46–49`) motivated this additional check.
- **Why it costs performance:** Large draw counts can still consume CPU for guest packet decoding, per-draw state checks and command recording even when queue submission is batched. No local trace establishes that this is a current bottleneck or that Bully has thousands of mergeable draws per frame. ROV blending, queries, EDRAM ownership, changing constants and memexport may require preserving exact draw order/state and constrain coalescing.
- **Suggested fix:** First measure draw counts, small-draw distribution and command-recording CPU. Only then investigate merging strictly compatible consecutive draws or an indirect path that preserves guest ordering and all state-dependent effects. Retain the current lazy command-list submission fix; generic multi-draw advice is not a drop-in SDK patch. Fix location: SDK GPU backend rebuild.
- **How to verify:** Future PIX draw/state counts and WPR `GPU Commands` exclusive samples in town/missions/interiors. Track CPU recording time and submissions separately from GPU busy; compare exact guest p99 and full visual correctness. The document's claimed 30–60% GPU gain is not a BullyRecomp estimate.

## Already fixed (from OPTIMIZATION.md): still OK?

| Existing fix / decision | Review |
|---|---|
| Lazy submission, `d3d12_submit_on_primary_buffer_end=false` | Still set through source-aware `SetDefaultFlag` (`src/BullyRecomp_app.h:77,206–210`). Matrix results document ~41 → ~60 swaps/s. SDK `d3d12/command_processor.cpp:2265–2268` checks the flag. Sound measured default; it does not eliminate submissions/waits from occlusion, memexport or pipeline draining. Trace these separately rather than re-reporting primary-buffer submission as unfixed. |
| Eight pipeline creation threads | Still set at `src/BullyRecomp_app.h:78`; early matrix p99 31.3 → 29.0 ms with eight workers. Reasonable for the measured machine, not a universal optimum. Translation remains serialized and compilation waits can occur at submission end. More workers can increase cold-load CPU pressure; compare smaller counts in unseen scenes. |
| Guest `vsync=false` / WAIT_REG_MEM sleep avoidance | Still set at `src/BullyRecomp_app.h:85` and `BullyRecomp.ini:31`. SDK `../rexglue-sdk/src/graphics/command_processor.cpp:1034–1050` yields instead of the millisecond sleep branch when this flag is false. Busy-yard results ~29.9 → 59.5 swaps/s remain valid. The flag controls guest vblank/polling, not host Present's sync interval. Side costs: synthetic vblank still runs at 1 kHz, yields still consume scheduler/CPU activity, and its worker still uses coarse sleeping. Do not blindly re-enable it to address host display cadence. |
| Async shaders | Still enabled in app/INI (`src/BullyRecomp_app.h:88`, `BullyRecomp.ini:29`). Existing busy-yard runs removed 0.5–6 s freezes and survived three runs. This remains a valuable fix, but it is incomplete: submission draining, vertex-only translation and shared translator locking are documented above. Skipped draws/pop-in are the existing correctness/quality tradeoff. |
| Heap critical-section spin | Still defaults to 20 µs (`src/guest_locks.cpp:22`) and faithfully falls back for recursion/contention. Existing measurements show kernel wait residency dropping 15–19% → 3–6%. No proven waiter barging/starvation was found. Side costs: blanket lock scope, timing before free-lock CAS and one-time 5 ms calibration; reevaluate the budget in town and on constrained CPUs. |
| TimerQueue wait strategy | Local SDK source is fixed (`src/core/timer_queue.cpp:51–54,182–203`), and current local build cache selects the rebuilt runtime. Existing controlled comparison: 15.1% → 0.8% of a core, process 1.30 → 1.19 cores, guest p99 33.1 → 33.3 ms. It saves CPU rather than raising FPS. Windows high-resolution timer creation falls back to a normal waitable timer if unavailable; check actual wake accuracy on that fallback. Deployment fragility is the remaining issue. |
| Quiet player logging / capture disabled | The current INI uses warn (`BullyRecomp.ini:47`) and player launcher has no capture/profile flags (`play.cmd:3`). Generic log argument formatting is gated by `should_log` in `../rexglue-sdk/include/rex/logging/macros.h:17–22`; no general disabled-log formatting defect was found. Error-level compression messages still pass the filter. Autoplay uses info and screenshots, so it is a different workload. |
| Random-case switch-table repair | `config/switch-tables.toml` records corrected index-register handling for `sub_8235E420` / `sub_8234C948`. The notes document scanning all 838 switches. No new mismatch was found in sampled generated code. Earlier light-scene/crashing results must not be treated as current busy-yard compute baselines. No codegen was rerun in this audit. |
| Guest shader-cache failed opens adjudicated harmless | `logs/runtime-18.log:149–156` records eight `D:\Shaders\Generated\Ni*.cache` probes returning `0xc000000f`; `logs/autoplay/ini-b/game.log:44–51` groups them at startup. The notes find no measurable effect. Host `.xsh`/`.xpso` storage is separate and does work: `logs/perf/final-pacing-1.log:25–26` loads 215 shaders in 15 ms and creates 405 PSOs in 54 ms. No evidence these guest misses disable host caching or recur per frame; do not prescribe committing caches/assets. |
| Resolution cap / shadow and texture scaling fixes (newer than optimization notes) | `src/settings_ini.cpp:102–105,172–195` caps scale to the nominal 4K output; this is a maximum guard, not a GPU budget. README's local SDK patch section (`README.md:183–198`) documents avoiding stale scaled treatment for DXT textures and scaling shadow-filter constants. Keep these correctness fixes; the shadow constant scan has a bounded binding-side cost documented above. The shipped 2× / template 1× mismatch is a separate performance policy issue. |

The opening “no frame-time measurement yet” and closing “every number ... is a hypothesis” language in `OPTIMIZATION.md` precede/surround later results; use its measured results sections and underlying logs rather than those stale planning sentences. Earlier 568 MB/light-yard runs are not interchangeable with the repaired NPC scene (~1.2 GB). Later shadow/scale experiments also use different settings: for example `logs/autoplay/ini-b/game.log:26–28` selects 3× and the run reports a hang, so it is not evidence that the current 2× settings have a proven root-cause defect.

## Areas checked with nothing found

These are negative findings within inspected/sampled paths, not a claim of exhaustive absence.

- **Descriptor heaps, root signatures and PSO switching:** SDK bindless mode defaults on (`../rexglue-sdk/src/graphics/d3d12/command_processor.cpp:35,915`), with persistent heaps (`:955–983`). Pipeline rebinding is guarded by handle change (`:2420`), sampler descriptors are cached (`:4415`) and heaps reused (`:4372–4375`). Sampler heap overflow clears a cache (`:4403`), but no log/profile demonstrates Bully overflowing it. No defensible per-frame heap recreation or runaway root/PSO-switch defect was found.
- **Barriers / EDRAM resolves:** Necessary ROV compute/resolve work is included in issue 1, and resolution amplification in issue 2. Interlock overlap logic (`../rexglue-sdk/src/graphics/pipeline/render_target/cache.cpp:667–677`) and ROV UAV-modification checks (`../rexglue-sdk/src/graphics/d3d12/render_target_cache.cpp:1745–1750`) avoid some redundant barriers. No evidence supports safely deleting them. D3D12 barriers express ordering/resource-state dependencies; minimizing them requires actual dependency/timing evidence ([Microsoft resource-barrier documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12)).
- **Upload heap use:** Shared memory uses pooled upload pages (`../rexglue-sdk/src/graphics/d3d12/shared_memory.cpp:318`), copies into DEFAULT resources (`:327–332`) and reclaims pooled pages (`:171`). No evidence the renderer permanently draws ordinary geometry from UPLOAD heaps. First-touch tile mapping and repeated validity clearing are the distinct findings above.
- **Texture lifetime/invalidation:** Texture watches and deferred loads are present (`../rexglue-sdk/src/graphics/pipeline/texture/cache.cpp:587,695,702,735`). No unconditional full texture recreation each frame was found. Unknown-destination memexport invalidation and per-frame shared-page validity are separately called out, not conflated with texture object recreation.
- **Resolve readbacks and effective memexport settings:** Resolve mode defaults to `none` (`../rexglue-sdk/src/graphics/command_processor.cpp:48`); the optional slow resolve copyback path exists (`d3d12/command_processor.cpp:3084–3103`) but ordinary player settings do not request it. Effective memexport readback **is on** through shared flag precedence despite legacy backend defaults; it is explicitly an issue above. No capture/readback contribution to ordinary play was assumed from diagnostic runs.
- **Present effects:** `present_effect=bilinear`, `swap_post_effect=none` (`BullyRecomp.ini:15,17`) avoid optional sharpening/upscaling and FXAA work. Tearing/VRR is permitted, not forced to work on the measured display. Host `Present(0)` is already used; no recommendation to simply replace `Present(1)` is applicable.
- **Memory growth / debug memory policies:** `scribble_heap=false`, `protect_on_release=false` (`../rexglue-sdk/src/system/xmemory.cpp:39–43`) avoid diagnostic fills/protection by default. Existing bulk host-protection batching (`:1000`) must not be re-reported as a new per-page protection defect. `logs/autoplay/soak-10min/summary.json:30–32` records 1138 MB start, 1261 MB max, and 26.9 MB growth per 10 minutes; this is not literally zero growth, but does not establish a leak. Scene transitions, private commit and GPU budgets remain unmeasured.
- **File buffering / failed opens:** Host file creation uses ordinary cached Windows I/O (`../rexglue-sdk/src/core/filesystem_win.cpp:206–207`); no `FILE_FLAG_NO_BUFFERING` defect was found. Missing `SURFTEST.sea`, `\\Device\\Image` and shader-cache paths noted in `OPTIMIZATION.md` warrant frequency checks, but there is no new evidence that they are current per-frame loops. Do not assign a streaming hitch to a failure without timestamps/call rates.
- **Thread priority/affinity:** `ignore_thread_priorities=true` and `ignore_thread_affinities=true` (`../rexglue-sdk/src/system/xthread.cpp:40–43`) mean default guest affinities do not pin the workload to six emulated cores. No proven bad default affinity/priority scheduling policy was found; opt-in affinity changes require separate measurements.
- **Guest translation / endianness / dispatch:** Guest RAM translation is inline arithmetic with required byte-swap builtins (`generated/default/BullyRecomp_pch.h:127–137`), not a runtime memory-translation call per load. Normal indirect calls use range-checked dense module table lookup (`:257–278`); only fallback dispatcher resolution reaches a mutex/hash lookup (`../rexglue-sdk/src/system/function_dispatcher.cpp:276–282`). No evidence of frequent fallback or avoidable generic byte-swap misuse was found.
- **Exceptions, setjmp/longjmp and fences:** Generated SEH compile support uses `-fasync-exceptions` (`generated/rexglue.cmake:40–48`), but sampled/searchable generated translation units do not expose active `__try`/`__except` or `ppc_setjmp`/`ppc_longjmp` calls. PCH map helpers do not prove hot-path cost. `eieio` uses a conservative seq_cst fence (`../rexglue-sdk/src/codegen/builders/system.cpp:52–60`); no measured excess-fence problem justifies weakening it.
- **Release/debug build state:** Current cache/rules show Release `-O3 -DNDEBUG`; generated prologues use alignment assumptions and guest profiling zones/counters are conditional. Basic `rex_assert` is disabled by NDEBUG (`../rexglue-sdk/include/rex/assert.h:28–32`). Message-bearing assertions and `REX_ASSERT` can retain guards/logging (`../rexglue-sdk/include/rex/assert.h:60–65`, `../rexglue-sdk/include/rex/logging/assert.h:67–73`), but no hot use/firing was found. No explicit /GS, CFG, stack-protector or preserved-frame-pointer overhead was established; disabling protections is not an evidence-based recommendation.
- **Ordinary-play debug activation:** `StartPerf` returns for an empty path (`src/BullyRecomp_app.h:224–225`), sampling needs its explicit flag, capture needs a directory, RE scanners/watchpoints need explicit flags, and GPU diagnostics are compiled out in the inspected local cache. Autoplay's input hook calls the original import and exits after checking its cached disabled state (`src/autoplay.cpp:223–230`). No normal-play capture/scanner thread or expensive scripted-input path was found. The unconditional swap timestamp hook is the small exception listed above.

## Suggested profiling next steps

1. **Establish two reproducible baselines:** current player INI and controlled 1× app defaults. Use future `tools/perf-run.ps1` / `tools/perf-matrix.ps1` runs with screenshots and the in-process sampling profiler disabled for timing; use external WPR/PIX alongside PresentMon. Record effective cvars, executable and loaded DLL identities, GPU power/clocks/temperature, monitor refresh/VRR state and cache warmth. The primary perf script currently ignores the INI, so explicitly select it for the player baseline. Keep the window visible and cool between A/Bs; rerun a baseline to detect drift.
2. **Add town, mission and interior routes:** deterministic autoplay timelines or controlled saves/routes, with separate first-visit and warmed results. Existing `tools/autoplay/yard-loop.txt` and `shadow-orbit.txt` cover yard/shadow behavior, not these workloads. Scripted inputs alone do not guarantee identical NPC/world state. Store gameplay data locally; no game code/assets need committing.
3. **Trace the completion chain:** guest fence polling, PM4 WAIT_REG_MEM values/yield counts, submission count/reason, occlusion query count, memexport bytes/full-readback fallback, pipeline drain/wait and vblank wake lateness. Align events to exact swap timestamps. Prioritize causes upstream of the guest fence before treating the spin as ordinary computation.
4. **Measure GPU costs:** PIX captures at 1×/2×/3×, ROV timing and diagnostic RTV comparisons. Break out software output merging/overdraw, resolve dispatches, uploads/tile mappings, constant uploads, texture reloads and residency budgets. Compare the first divergent RTV draw/depth/resolve to establish the black-world cause. Keep clean timing sessions separate from screenshots and draw dumps.
5. **Test shader encounters cold and warm:** isolate submission-end blocking, vertex-only translation, translation mutex contention and PSO worker counts. A warm yard that creates no new pipelines cannot validate async behavior in town/interiors. Maintain user-generated host caches outside committed game assets.
6. **Correlate streaming/memory/audio tails:** WPR disk/file, page-fault, allocation and context-switch traces for frames over 50 ms. Inspect main-thread reads, heap scan lengths/commit latency, audio queue/global-lock waits and repeated failed opens by timestamp. Count first: do not optimize speculative work merely because a branch exists.
7. **Then test guest/codegen/build experiments:** pause hint lowering, lock free-entry ordering and spin budgets first; localization, copy hooks, SIMD, ISA targeting, ThinLTO/PGO after compute hotness is established. These changes need future regeneration/builds and correctness checks, but none were performed for this audit.
8. **Soak and use explicit acceptance metrics:** run future 30-minute town/interior/free-roam autoplay plus visual checks. Track exact guest p50/p95/p99, jitter, >20/>50 ms counts, worst frame, fresh displayed-frame latency, per-thread CPU, private commit/working set and GPU residency. The existing soak has 59.41 swaps/s, p99 22.72 ms and worst 53.1 ms (`logs/autoplay/soak-10min/summary.json:21–28`). Its PASS checks crash/hang/frozen/black output (`tools/autoplay-report.py:120–133`), not a strict 1%-low performance threshold; do not equate PASS with every pacing goal being met.

Measurement caution: use `GuestFrameP99Ms` / exact `.frames.csv` values for pacing, not `GuestP99Ms` (the average of polling-derived per-second percentiles in `tools/perf-run.ps1:131`). The logger's polling can divide a multi-swap interval into artificial equal samples (`src/BullyRecomp_app.h:287–291`). Existing summaries contain both forms. Compare matching metrics and configuration before interpreting an improvement.

## Supplementary optimization document: applicability review

Source: `C:/Users/Admin/Downloads/BullyRecomp Performance Optimization.md`, read in full for its text and references. This is a general strategy document spanning several recompilers/platforms; it contains no additional BullyRecomp profile or controlled A/B. Its references include unrelated material (for example reference 15 is a prompt-injection paper, despite being cited for ThinLTO/PGO). Accordingly, its numerical uplift table (`:86–95`) cannot substantiate this project's estimated gains.

| Document recommendation / claim | Result of checking this project |
|---|---|
| Guest register promotion / local state (`:7–10`) | Useful but already covered by issue 17. Existing localization flags should be evaluated before proposing an entirely new SSA recompiler. Benefits need final assembly and gameplay profiles; no evidence supports importing the document's 15–30% CPU estimate. |
| CFG reconstruction, switch recovery and indirect-call caching (`:12–15`) | Native labels/switches and dense dispatch already exist. Sampled `sub_82727E78` (`generated/default/BullyRecomp_recomp.24.cpp:18297–18323`) uses a native switch; `generated/default/BullyRecomp_pch.h:257–278` uses a dense table rather than a general hash search. A polymorphic call-site cache is worth considering only if measured target predictability and indirect-branch misses justify it. No new confirmed issue was added. |
| Native SIMD and startup FP-mode configuration (`:17–20`) | SIMD is already broadly present. Issue 19 now also cites profiled scalar fused/estimate math. New issue 37 covers conservative mode guards and real mode transitions. Hardware CSR helpers already exist, and scalar FPU versus VMX modes differ, so global one-time FTZ/DAZ is not a safe universal replacement. |
| Implement Fastmem first (`:24–37,100`) | Already mapped: `../rexglue-sdk/src/system/xmemory.cpp:140–182,185–201` creates/maps guest virtual and physical views; `generated/default/BullyRecomp_pch.h:125–137` performs inline base/address arithmetic. Windows also has a physical-alias correction at `:119–120`, so it is not literally a single universal load instruction, but there is no missing per-access software page-table walker. No new 20–40% opportunity was established. |
| Offline endian conversion and MOVBE (`:39–42`) | Guest memory must retain the layout expected by PPC code, CPU/GPU sharing and mixed-width accesses. Blindly byte-swapping assets/data offline is incompatible with those contracts and the audit's asset restriction. Current accessors already express swaps through compiler builtins (`PCH:128–137`). MOVBE-capable targeting can be folded into issue 19's ISA/assembly experiment; no final assembly was inspected here, so absence of MOVBE or a single-micro-op speedup is not claimed. |
| Coalesce draws / multi-draw indirect (`:46–49`) | Added Low/Hypothesis issue 38 for per-draw CPU recording only. The backend already batches commands into submissions; the document confuses reducing draw count with reducing queue submissions. Profile mergeable state/order sequences before engineering a new batching layer. Its claimed 30–60% GPU gain is unsupported for this ROV workload. |
| Render directly from persistently mapped host-visible buffers (`:49`) | Not an automatic improvement on a discrete RTX GPU. The existing pooled upload → DEFAULT copy path is intentional and already reviewed. Microsoft recommends profiling direct UPLOAD reads versus DEFAULT copies according to reuse/access patterns; upload-memory placement can trade away GPU bandwidth. See [D3D12 heap-type guidance](https://learn.microsoft.com/en-us/windows/desktop/api/d3d12/ne-d3d12-d3d12_heap_type). GPU_UPLOAD/BAR-backed alternatives are a separate capability-specific experiment, not evidence that the existing copies should be removed. |
| Async shaders / disk PSO cache / offline prewarming (`:51–54`) | Already covered by issues 3, 8, 9 and 34. Host shader/description caching works. Offline discovery cannot guarantee every runtime state combination is known, and caches do not permanently eliminate every hitch. Validate cold routes and preserve cache version/device compatibility; do not commit game shader binaries/assets as part of this audit. |
| RenderWare interception / scalable allocator / texture replacement (`:63–66`) | The document assumes an engine architecture that does not match the local evidence: README identifies Gamebryo's `TexSizeShadow` (`README.md:191`), and runtime logs name `NiStandardMaterial` (`logs/runtime-18.log:149–150`). Use actual guest function/ABI evidence, not presumed RenderWare entry points. Copy hooks and heap contention are already covered. A mimalloc/jemalloc swap would require guest address, layout, lifetime and cross-module compatibility; no allocator replacement or wholesale texture conversion is justified by this document. |
| Add render/streaming/audio worker architecture (`:68–72`) | Rendering and audio already have host workers; only the specific synchronous guest file-read behavior in issue 7 is confirmed. Moving arbitrary guest work to new threads would change ordering and shared-state assumptions. Follow the actual read/completion and audio-lock findings rather than treating the runtime as entirely single-threaded. |
| ThinLTO / PGO / BOLT (`:76–80`) | ThinLTO/PGO remain issue 18 experiments. They cannot automatically optimize through the separately linked prebuilt runtime DLL boundary; SDK changes need compatible rebuild/link architecture. BOLT's current upstream README describes x86-64/AArch64 **ELF** binaries, so it is not an established post-link tool for this Windows PE executable. See [LLVM BOLT supported inputs](https://github.com/llvm/llvm-project/blob/main/bolt/README.md). No BOLT-based Windows gain is assumed. |
| Fixed 30 Hz simulation and frame generation (`:56–59`) | Existing measurements establish approximately 60 guest swaps/s after pacing fixes, not a proven 30 Hz simulation contract. The document supplies no timing trace showing which Bully subsystems are fixed-step or break at current settings. Frame generation adds a presentation feature and GPU work; it does not fix guest fence waits, streaming stalls or input latency. Motion/depth integration and gameplay timing need separate investigation, not a new confirmed performance defect. |
| CPU-first roadmap and quoted overall uplifts (`:86–101`) | Keep the evidence-based order in this report: ROV/resolution, cold pipeline synchronization, readback/submission/streaming, then compute/codegen. Existing GPU busy of ~14–15 ms and substantial guest wait residency do not support the document's categorical assertion that command overhead always outweighs fragment work. None of its percentage ranges is treated as a measured BullyRecomp expectation. |

Additional future checks prompted by this document: include FP-mode guard/transition counters, scalar FMA/estimate assembly and draw/state counts in the town/interior CPU captures. These were source-only follow-ups; no optimization, regeneration, build or executable profiling was performed.
