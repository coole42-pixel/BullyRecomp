#pragma once
#include <rex/audio/sdl/sdl_audio_system.h>

// The SDK's SubmitFrame asserts a driver exists, but a queued callback may
// finish after UnregisterClient has cleared that slot. Check under the same
// critical region used by registration and teardown before dereferencing it.
class BullyAudioSystem : public rex::audio::sdl::SDLAudioSystem {
 public:
  using SDLAudioSystem::SDLAudioSystem;
  bool SubmitFrameIfActive(size_t index, uint32_t samples);
};
