// BullyRecomp - ReXGlue Recompiled Project

#include "generated/default/BullyRecomp_init.h"

#include "BullyRecomp_app.h"

std::atomic<uint64_t> g_bully_swap_count{0};
int64_t g_bully_swap_qpc[kBullySwapRing];
// Called from generated code (config/perf-hooks.toml) at the game's VdSwap call.
// Records the QPC time of each swap so the perf logger can write exact frame intervals.
void BullyFrameSwapHook() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const uint64_t index = g_bully_swap_count.load(std::memory_order_relaxed);
  g_bully_swap_qpc[index % kBullySwapRing] = now.QuadPart;
  g_bully_swap_count.store(index + 1, std::memory_order_release);
}

REXCVAR_DEFINE_STRING(bully_capture_dir, "", "Debug",
                     "Optional directory for periodic guest-frame captures");
REXCVAR_DEFINE_INT32(bully_capture_interval_s, 3, "Debug",
                    "Seconds between guest-frame captures (--bully_capture_dir)");
REXCVAR_DEFINE_INT32(bully_capture_max, 120, "Debug", "Maximum number of guest-frame captures");
REXCVAR_DEFINE_STRING(bully_perf_log, "", "Debug",
                     "Optional CSV path for per-second frame-time and CPU statistics");
REXCVAR_DEFINE_BOOL(bully_profile, false, "Debug",
                    "Sample the guest main thread (needs --bully_perf_log); writes <log>.profile.csv");
REXCVAR_DEFINE_STRING(bully_profile_thread, "Main XThread", "Debug",
                     "Name prefix of the thread --bully_profile samples");
REXCVAR_DEFINE_BOOL(bully_gpu_diagnostics, false, "Debug", "Enable optional GPU draw diagnostics");

REX_DEFINE_APP(BullyRecomp, BullyrecompApp::Create)
