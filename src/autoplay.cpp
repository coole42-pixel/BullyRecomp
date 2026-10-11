// Automated gameplay input (--bully_autoplay).
//
// Overrides the XamInputGetState import the generated code calls, and merges a scripted
// gamepad state for controller 0 into whatever the real input system reports. Because the
// state is written straight into guest memory, it works without window focus and without
// injecting host keystrokes, so the PC stays usable while a test session runs.
//
// Modes:
//   --bully_autoplay=wander      Boot phase (taps A to get through the title menu, loading and
//                                intro), then seeded random wandering: walk/run in changing
//                                directions, camera sweeps, sprint taps and the odd jump/action.
//                                Never presses Start or Back, which would open menus.
//   --bully_autoplay=<file>      Timeline script, one step per line. Steps after a line
//                                reading "loop" repeat forever; without it the whole script
//                                repeats:
//                                  <ms> [A B X Y LB RB LT RT START BACK UP DOWN LEFT RIGHT LS RS]
//                                       [L=x,y] [R=x,y]       (stick values -1..1)
//                                e.g. "2000 L=0,1 R=0.5,0"  = walk forward, turn camera right
//                                Lines starting with '#' are comments.

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

REXCVAR_DEFINE_STRING(bully_autoplay, "", "Debug",
                      "Automated gameplay input: 'wander' or a timeline script path");
REXCVAR_DEFINE_INT32(bully_autoplay_seed, 1, "Debug", "Seed for --bully_autoplay=wander");
REXCVAR_DEFINE_INT32(bully_autoplay_boot_s, 45, "Debug",
                     "Seconds of menu/intro confirmation (A taps) before wandering");

namespace {

// XINPUT_GAMEPAD button bits.
enum : uint16_t {
  kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008,
  kStart = 0x0010, kBack = 0x0020, kLeftThumb = 0x0040, kRightThumb = 0x0080,
  kLeftShoulder = 0x0100, kRightShoulder = 0x0200, kA = 0x1000, kB = 0x2000, kX = 0x4000,
  kY = 0x8000,
};

struct PadState {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

struct Step {
  uint32_t duration_ms;
  PadState pad;
};

int16_t Axis(double v) { return int16_t(std::clamp(v, -1.0, 1.0) * 32767.0); }

// Deterministic 64-bit mix (splitmix64) so the same seed replays the same session.
uint64_t Mix(uint64_t x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}
double Unit(uint64_t seed, uint64_t a, uint64_t b) {  // [0, 1)
  return double(Mix(seed ^ Mix(a * 0x100000001B3ull + b)) >> 11) * 0x1.0p-53;
}

PadState Wander(double t_s) {
  PadState pad;
  const double boot_s = std::max(0, REXCVAR_GET(bully_autoplay_boot_s));
  if (t_s < boot_s) {
    // 120 ms A tap every 1.5 s: confirms the title menu and advances/skips intro screens.
    if (std::fmod(t_s, 1.5) < 0.12) pad.buttons |= kA;
    return pad;
  }
  const uint64_t seed = uint64_t(REXCVAR_GET(bully_autoplay_seed));
  const double t = t_s - boot_s;
  // 2.5 s segments, each with its own direction and camera motion.
  const uint64_t segment = uint64_t(t / 2.5);
  const double in_segment = t - double(segment) * 2.5;
  // Mostly forward with turns; one segment in four picks any direction, which gets Jimmy
  // off walls and fences he would otherwise keep running into. Occasionally stop briefly.
  const double turn = Unit(seed, segment, 6) < 0.25 ? 6.2832 : 2.4;  // full circle or +-70 deg
  const double heading = (Unit(seed, segment, 1) - 0.5) * turn;
  const bool idle = Unit(seed, segment, 2) < 0.08;
  if (!idle) {
    pad.lx = Axis(std::sin(heading));
    pad.ly = Axis(std::cos(heading));
  }
  // Camera: sweep for the first second of some segments.
  const double cam = Unit(seed, segment, 3);
  if (cam < 0.6 && in_segment < 1.0) pad.rx = Axis(cam < 0.3 ? -0.8 : 0.8);
  // Sprint: tap A a few times in some segments (Bully sprints on repeated A taps).
  if (Unit(seed, segment, 4) < 0.35 && std::fmod(in_segment, 0.4) < 0.1) pad.buttons |= kA;
  // Rare single taps of other face buttons (jump/action) early in a segment.
  const double extra = Unit(seed, segment, 5);
  if (in_segment > 1.2 && in_segment < 1.32) {
    if (extra < 0.10) pad.buttons |= kX;
    else if (extra < 0.15) pad.buttons |= kY;
  }
  return pad;
}

class Timeline {
 public:
  bool Load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const auto first = line.find_first_not_of(" \t");
      if (first == std::string::npos || line[first] == '#') continue;
      if (line.compare(first, 4, "loop") == 0) {
        loop_start_ms_ = total_ms_;
        continue;
      }
      std::istringstream words(line);
      Step step{};
      if (!(words >> step.duration_ms) || !step.duration_ms) continue;
      std::string word;
      while (words >> word) {
        for (auto& c : word) c = char(toupper(uint8_t(c)));
        double x = 0, y = 0;
        if ((word.rfind("L=", 0) == 0 || word.rfind("R=", 0) == 0) &&
            sscanf_s(word.c_str() + 2, "%lf,%lf", &x, &y) == 2) {
          (word[0] == 'L' ? step.pad.lx : step.pad.rx) = Axis(x);
          (word[0] == 'L' ? step.pad.ly : step.pad.ry) = Axis(y);
        } else if (word == "LT") {
          step.pad.left_trigger = 255;
        } else if (word == "RT") {
          step.pad.right_trigger = 255;
        } else {
          static const std::pair<const char*, uint16_t> kNames[] = {
              {"A", kA}, {"B", kB}, {"X", kX}, {"Y", kY}, {"LB", kLeftShoulder},
              {"RB", kRightShoulder}, {"START", kStart}, {"BACK", kBack}, {"UP", kDpadUp},
              {"DOWN", kDpadDown}, {"LEFT", kDpadLeft}, {"RIGHT", kDpadRight},
              {"LS", kLeftThumb}, {"RS", kRightThumb}};
          for (const auto& [name, bit] : kNames) {
            if (word == name) step.pad.buttons |= bit;
          }
        }
      }
      total_ms_ += step.duration_ms;
      steps_.push_back(step);
    }
    return !steps_.empty();
  }

  PadState At(double t_s) const {
    uint64_t t = uint64_t(t_s * 1000.0);
    if (t >= total_ms_) {
      const uint64_t start = loop_start_ms_ < total_ms_ ? loop_start_ms_ : 0;
      t = start + (t - start) % (total_ms_ - start);
    }
    for (const auto& step : steps_) {
      if (t < step.duration_ms) return step.pad;
      t -= step.duration_ms;
    }
    return {};
  }

 private:
  std::vector<Step> steps_;
  uint64_t total_ms_ = 0;
  uint64_t loop_start_ms_ = 0;
};

struct Autoplay {
  bool enabled = false;
  bool wander = false;
  Timeline timeline;
  LARGE_INTEGER start{}, frequency{};
};

const Autoplay& GetAutoplay() {
  static const Autoplay autoplay = [] {
    Autoplay a;
    const std::string mode = REXCVAR_GET(bully_autoplay);
    if (mode.empty()) return a;
    if (mode == "wander") {
      a.wander = true;
    } else if (!a.timeline.Load(mode)) {
      REXLOG_ERROR("Autoplay: cannot read timeline script '{}'", mode);
      return a;
    }
    a.enabled = true;
    QueryPerformanceFrequency(&a.frequency);
    QueryPerformanceCounter(&a.start);
    REXLOG_INFO("Autoplay: {} (seed {}, boot {} s)", mode, REXCVAR_GET(bully_autoplay_seed),
                REXCVAR_GET(bully_autoplay_boot_s));
    return a;
  }();
  return autoplay;
}

PPCFunc* OriginalXamInputGetState() {
  static PPCFunc* original = reinterpret_cast<PPCFunc*>(
      GetProcAddress(GetModuleHandleW(L"rexruntime.dll"), "__imp__XamInputGetState"));
  return original;
}

constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kFlagGamepad = 0x00000001;
constexpr uint32_t kFlagAnyUser = 0x40000000;

uint16_t LoadBe16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
void StoreBe16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }

}  // namespace

extern "C" REX_FUNC(__imp__XamInputGetState) {
  const uint32_t user_index = ctx.r3.u32;
  const uint32_t flags = ctx.r4.u32;
  const uint32_t state_address = ctx.r5.u32;
  OriginalXamInputGetState()(ctx, base);

  const Autoplay& autoplay = GetAutoplay();
  if (!autoplay.enabled || !state_address) return;
  if ((flags & 0xFF) && !(flags & kFlagGamepad)) return;
  if ((user_index & 0xFF) != 0 && (user_index & 0xFF) != 0xFF && !(flags & kFlagAnyUser)) return;

  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const double t_s = double(now.QuadPart - autoplay.start.QuadPart) / autoplay.frequency.QuadPart;
  const PadState pad = autoplay.wander ? Wander(t_s) : autoplay.timeline.At(t_s);

  // X_INPUT_STATE (big-endian): u32 packet, u16 buttons, u8 lt, u8 rt, s16 lx, ly, rx, ry.
  uint8_t* state = base + state_address;
  const bool had_device = ctx.r3.u32 == kErrorSuccess;
  if (!had_device) std::memset(state, 0, 16);
  static std::atomic<uint32_t> packet{1};
  const uint32_t packet_number = packet.fetch_add(1, std::memory_order_relaxed);
  state[0] = uint8_t(packet_number >> 24);
  state[1] = uint8_t(packet_number >> 16);
  state[2] = uint8_t(packet_number >> 8);
  state[3] = uint8_t(packet_number);
  // Merge: scripted buttons/triggers add to real input; scripted sticks win when non-zero,
  // so a person can still take over.
  StoreBe16(state + 4, uint16_t(LoadBe16(state + 4) | pad.buttons));
  state[6] = std::max(state[6], pad.left_trigger);
  state[7] = std::max(state[7], pad.right_trigger);
  const int16_t sticks[4] = {pad.lx, pad.ly, pad.rx, pad.ry};
  for (int i = 0; i < 4; ++i) {
    if (sticks[i]) StoreBe16(state + 8 + i * 2, uint16_t(sticks[i]));
  }
  ctx.r3.u64 = kErrorSuccess;
}
