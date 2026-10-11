#include "audio_compat.h"
#include <rex/audio/audio_driver.h>
#include <rex/runtime.h>
#include <rex/ppc.h>

bool BullyAudioSystem::SubmitFrameIfActive(size_t index, uint32_t samples) {
  auto lock = global_critical_region_.Acquire();
  if (index >= kMaximumClientCount || !clients_[index].in_use || !clients_[index].driver) {
    return false;
  }
  clients_[index].driver->SubmitFrame(samples);
  return true;
}

// Strong host implementation of this imported kernel call. The generated
// mapping and direct calls both use this symbol; no generated source is edited.
extern "C" void __imp__XAudioSubmitRenderDriverFrame(PPCContext& ctx, uint8_t*) {
  using rex::X_RESULT;
  const uint32_t handle = ctx.r3.u32;
  auto* runtime = rex::Runtime::instance();
  auto* audio = runtime ? dynamic_cast<BullyAudioSystem*>(runtime->audio_system()) : nullptr;
  if ((handle & 0xFFFF0000) != 0x41550000 || !audio) {
    ctx.r3.u64 = X_ERROR_INVALID_HANDLE;
    return;
  }
  // A frame from an already-unregistered client is obsolete. Dropping it
  // preserves the kernel call's success behavior without touching freed state.
  audio->SubmitFrameIfActive(handle & 0xFFFF, ctx.r4.u32);
  ctx.r3.u64 = X_ERROR_SUCCESS;
}
