// Guest motion-blur control, applied by config/perf-hooks.toml before the
// game's accumulation pass converts its integer strength to gMotionBlurStrength.
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>

#include <atomic>
#include <array>
#include <bit>
#include <cstring>

REXCVAR_DEFINE_BOOL(bully_motion_blur, true, "Graphics",
                    "Enable the game's motion blur (false removes accumulated motion trails)");
REXCVAR_DEFINE_BOOL(bully_blur_diagnostics, false, "Debug",
                    "Log game blur state changes for troubleshooting");

void BullyMotionBlurHook(PPCRegister& strength) {
  const bool enabled = REXCVAR_GET(bully_motion_blur);
  const int requested = strength.s32;
  if (!enabled) strength.s64 = 0;
  static std::atomic<bool> logged{false}, logged_nonzero{false};
  if (!logged.exchange(true) || (requested != 0 && !logged_nonzero.exchange(true))) {
    REXLOG_INFO("Bully motion blur: enabled={}, requested strength={}, applied strength={}",
                enabled, requested, strength.s32);
  }
}

void BullyMotionBlurStateHook() {
  if (!REXCVAR_GET(bully_blur_diagnostics)) return;
  auto* runtime = rex::Runtime::instance();
  if (!runtime) return;
  const uint8_t* base = runtime->virtual_membase();
  // Globals read by sub_8230F1F0 in this supported XEX; diagnostic reads only.
  const uint32_t addresses[] = {0x82EF73E8, 0x82CF147C, 0x82EF73FC, 0x82EF7400, 0x82EF741C};
  std::array<uint32_t, 5> state;
  for (size_t i = 0; i < state.size(); ++i) {
    std::memcpy(&state[i], base + addresses[i], 4);
    state[i] = std::byteswap(state[i]);
  }
  static std::array<uint32_t, 5> previous = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
  if (state != previous) {
    REXLOG_INFO("Bully blur state: normal={}, strength={}, special={}, special_strength={}, pulse={}",
                state[0], state[1], state[2], std::bit_cast<float>(state[3]), state[4]);
    previous = state;
  }
}
