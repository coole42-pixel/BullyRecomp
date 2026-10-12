// BullyRecomp - ReXGlue Recompiled Project

#include "generated/default/BullyRecomp_init.h"

#include "BullyRecomp_app.h"

std::atomic<uint64_t> g_bully_swap_count{0};
int64_t g_bully_swap_qpc[kBullySwapRing];
REXCVAR_DEFINE_INT32(bully_fps_limit, 60, "Performance",
                    "Guest frame production limit in FPS (0 = unlimited)")
    .range(0, 240);

namespace {

// Pace the producer rather than relying on UI presentation to throttle the game.
// Keep a fixed clock so small wake-up delays don't accumulate into repeated frames.
// A large hitch starts a new interval instead of emitting a burst of catch-up frames.
struct FramePacer {
  FramePacer() {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    ticks_per_second = frequency.QuadPart;
    timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                  TIMER_MODIFY_STATE | SYNCHRONIZE);
  }
  ~FramePacer() { if (timer) CloseHandle(timer); }

  int64_t Wait(int fps) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (fps != last_fps) next_frame = 0;
    const int64_t period = fps > 0 ? (ticks_per_second + fps / 2) / fps : 0;
    if (period && next_frame) {
      const int64_t deadline = next_frame;
      const int64_t spin_ticks = ticks_per_second / 5000;  // last 0.2 ms
      while (now.QuadPart < deadline) {
        const int64_t remaining = deadline - now.QuadPart;
        if (timer && remaining > spin_ticks) {
          LARGE_INTEGER due;
          due.QuadPart = -std::max<int64_t>(1,
              (remaining - spin_ticks) * 10000000 / ticks_per_second);
          if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, 100);
          } else {
            Sleep(1);
          }
        } else if (!timer && remaining > ticks_per_second / 500) {
          Sleep(1);
        } else {
          YieldProcessor();
        }
        QueryPerformanceCounter(&now);
      }
    }
    if (!period) {
      next_frame = 0;
    } else if (!next_frame || now.QuadPart - next_frame > period / 4) {
      next_frame = now.QuadPart + period;
    } else {
      next_frame += period;
    }
    last_fps = fps;
    return now.QuadPart;
  }

  HANDLE timer = nullptr;
  int64_t ticks_per_second = 0;
  int64_t next_frame = 0;
  int last_fps = 0;
};

}  // namespace

// Called from generated code (config/perf-hooks.toml) at the game's VdSwap call.
// Limits frame production and records exact intervals for the optional perf logger.
void BullyFrameSwapHook() {
  thread_local FramePacer pacer;
  const int64_t now = pacer.Wait(REXCVAR_GET(bully_fps_limit));
  const uint64_t index = g_bully_swap_count.load(std::memory_order_relaxed);
  g_bully_swap_qpc[index % kBullySwapRing] = now;
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
