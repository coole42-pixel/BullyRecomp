// Execute the regenerated functions on the original XEX's data mappings,
// without starting guest threads or graphics/audio backends.
#include "generated/default/BullyRecomp_init.h"
#include <rex/runtime.h>
#include <rex/kernel/init.h>
#include <cstdio>
#include <filesystem>
#include <unordered_set>
#include "src/audio_compat.h"
#include <rex/audio/audio_driver.h>

// Generated code calls the perf hook from config/perf-hooks.toml; the game app defines it.
void BullyFrameSwapHook() {}

class CountingAudioDriver : public rex::audio::AudioDriver {
 public:
  using AudioDriver::AudioDriver;
  void SubmitFrame(uint32_t samples) override { ++count; last_samples = samples; }
  unsigned count = 0;
  uint32_t last_samples = 0;
};

class TestAudioSystem : public BullyAudioSystem {
 public:
  using BullyAudioSystem::BullyAudioSystem;
  void SetTestDriver(rex::audio::AudioDriver* driver) {
    clients_[0].driver = driver;
    clients_[0].in_use = driver != nullptr;
  }
};

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  rex::Runtime runtime(std::filesystem::absolute(argv[1]));
  rex::RuntimeConfig config;
  config.tool_mode = true;
  config.kernel_init = rex::kernel::InitializeKernel;
  if (runtime.Setup(PPCImageConfig, std::move(config)) ||
      runtime.LoadXexImage("game:/Default.xex")) return 1;
  auto* base = runtime.virtual_membase();
  int failures = 0;
  auto check = [&](bool condition, const char* name) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
  };
  {
    TestAudioSystem audio(runtime.function_dispatcher());
    CountingAudioDriver driver(runtime.memory());
    check(!audio.SubmitFrameIfActive(0, 0) && !audio.SubmitFrameIfActive(8, 0),
          "late/invalid audio submission does not dereference a null driver");
    audio.SetTestDriver(&driver);
    check(audio.SubmitFrameIfActive(0, 0x40007080) && driver.count == 1 &&
          driver.last_samples == 0x40007080, "active audio frames reach the original driver");
    audio.SetTestDriver(nullptr);
    check(!audio.SubmitFrameIfActive(0, 0x40007080) && driver.count == 1,
          "unregistered audio client drops its obsolete frame");
  }
  std::unordered_set<uint32_t> registered;
  for (auto* mapping = PPCFuncMappings; mapping->host; ++mapping) {
    registered.insert(static_cast<uint32_t>(mapping->guest));
  }
  unsigned missingInitializers = 0;
  for (uint32_t slot = 0x82BF0014; slot < 0x82BF2B30; slot += 4) {
    const auto address = REX_LOAD_U32(slot);
    if (address && !registered.contains(address)) {
      std::printf("Missing initializer %08X at %08X\n", address, slot);
      ++missingInitializers;
    }
  }
  check(missingInitializers == 0, "all 2748 distinct original initializer pointers are registered");

  // Vtable entry 0x8269AA10 must include its initial bounds check; starting
  // only at the indexed-load block 0x8269AA24 accepts invalid indices.
  REX_STORE_U32(0x82E20018, 4);
  REX_STORE_U32(0x82E20008, 0x11223344);
  REX_STORE_U32(0x82E20014, 0x55667788);
  auto shaderElement = [&](uint32_t index) {
    PPCContext ctx{};
    ctx.r3.u64 = 0x82E20000;
    ctx.r4.u64 = index;
    sub_8269AA10(ctx, base);
    return ctx.r3.u32;
  };
  check(shaderElement(0) == 0x11223344 && shaderElement(3) == 0x55667788,
        "shader vtable method reads valid indices");
  check(shaderElement(4) == 0 && shaderElement(0xFFFFFFFF) == 0,
        "shader vtable method rejects out-of-range indices");
  REX_STORE_U32(0x82E20518, 0x11223344);
  PPCContext getter{};
  getter.r3.u64 = 0x82E20000;
  getter.lr = 0x12345678;
  sub_82775328(getter, base);
  check(getter.r3.u32 == 0x11223344 && getter.lr == 0x12345678,
        "0x82775328 returns the object field and preserves LR");

  REX_STORE_U32(0x82E20600, 0x82E20700);
  REX_STORE_U32(0x82E20604, 0x82E20740);
  REX_STORE_U32(0x82E20720, 0x82E20800);
  REX_STORE_U32(0x82E20760, 0x82E20820);
  REX_STORE_U32(0x82E20800, 0x82E20900);
  REX_STORE_U32(0x82E20804, 0);
  REX_STORE_U32(0x82E20820, 0x82E20920);
  REX_STORE_U32(0x82E20824, 0);
  auto compareLists = [&](uint16_t first, uint16_t second) {
    REX_STORE_U32(0x82E20900, uint32_t(first) << 13);
    REX_STORE_U32(0x82E20920, uint32_t(second) << 13);
    PPCContext ctx{};
    ctx.r3.u64 = 0x82E20600;
    ctx.r4.u64 = 0x82E20604;
    sub_82A70EE0(ctx, base);
    return ctx.r3.s32;
  };
  check(compareLists(2, 3) == 1 && compareLists(3, 2) == -1 &&
        compareLists(3, 3) == 0, "runtime comparison callback orders list maxima");

  PPCContext audio{};
  audio.r4.u64 = 0x82E20A00;
  sub_82876340(audio, base);
  check(REX_LOAD_U32(0x82E20A00) == 0, "null audio interface returns null");
  audio.r3.u64 = 0x82E20000;
  sub_82876340(audio, base);
  check(REX_LOAD_U32(0x82E20A00) == 0x82E20008,
        "audio interface returns the adjusted pointer");
  audio.r3.u64 = 0x82E20000;
  audio.r4.u64 = 0;
  REX_STORE_U8(0x82E2003C, 0);
  sub_82879DB8(audio, base);
  check(audio.r3.u32 == 0x80070057, "audio dispatch rejects invalid channel");
  audio.r3.u64 = 0x82E20000;
  audio.r4.u64 = 0;
  sub_82879E00(audio, base);
  check(audio.r3.u32 == 0x80070057, "audio getter rejects invalid channel");

  REX_STORE_U32(0x82E20B04, 0);
  REX_STORE_U32(0x82E20B88, 0x82E20C00);
  auto interfacePredicate = [&](uint32_t value) {
    REX_STORE_U32(0x82E21F14, value); // array[0x4C5]
    PPCContext ctx{};
    ctx.r3.u64 = 0x82E20B00;
    ctx.r4.u64 = 0x82E20B80;
    sub_823D38F8(ctx, base);
    return ctx.r3.u32;
  };
  check(interfacePredicate(0) == 0 && interfacePredicate(0x12345678) == 1,
        "menu transition predicate returns the original zero/nonzero result");
  auto reset = [&] {
    REX_STORE_U32(0x82E3E35C, 1);
    REX_STORE_U16(0x82E3E358, 41);
    REX_STORE_U32(0x82E3E34C, 1);
    REX_STORE_U16(0x82E3E348, 42);
    REX_STORE_U32(0x82E3E344, 1);
    REX_STORE_U16(0x82E3E340, 60);
    REX_STORE_U32(0x82E3E33C, 1);
    REX_STORE_U16(0x82E3E338, 61);
    REX_STORE_U16(0x82E22500, 100);
  };
  auto invoke = [&](uint16_t id) {
    PPCContext ctx{};
    ctx.r4.u64 = id;
    ctx.lr = 0x12345678;
    sub_821E0810(ctx, base);
    check(ctx.lr == 0x12345678, "leaf/tail branches preserve LR");
    return ctx.r3.u32;
  };

  reset();
  check(invoke(41) == 1, "primary match returns true");
  reset();
  check(invoke(42) == 1, "0x821E08AC backward branch returns true");
  reset();
  check(invoke(43) == 0, "unmatched IDs tail-call fallback and return false");
  reset();
  check(invoke(60) == 1, "fallback match returns true");
  reset();
  REX_STORE_U32(0x82E3E35C, 0);
  check(invoke(100) == 1 && REX_LOAD_U16(0x82E3E358) == 100 &&
        REX_LOAD_U16(0x82E22500) == 101, "primary lazy initialization");
  reset();
  REX_STORE_U32(0x82E3E35C, 0);
  REX_STORE_U32(0x82E3E34C, 0);
  check(invoke(101) == 1 && REX_LOAD_U16(0x82E3E348) == 101 &&
        REX_LOAD_U16(0x82E22500) == 102, "secondary initialization and backward branch");
  reset();
  REX_STORE_U16(0x82E3E358, 0xFFFF);
  check(invoke(0xFFFF) == 1, "signed 16-bit ID comparison");

  // Unsupported selector reaches the original callee's E_INVALIDARG return
  // without dereferencing an object. r11 records its incoming this pointer.
  PPCContext thunk{};
  thunk.r3.u64 = 0x82E20000;
  thunk.r4.u64 = 0xFF;
  thunk.lr = 0x12345678;
  sub_82885D20(thunk, base);
  check(thunk.r11.u32 == 0x82E20004 && thunk.r3.u32 == 0x80070057 &&
        thunk.lr == 0x12345678, "0x82885D20 adds four and tail-calls original callee");
  thunk = {};
  thunk.r3.u64 = 0x82E20000;
  thunk.r4.u64 = 0xFF;
  thunk.lr = 0x12345678;
  sub_82886100(thunk, base);
  check(thunk.r11.u32 == 0x82E20000 && thunk.r3.u32 == 0x80070057 &&
        thunk.lr == 0x12345678, "0x82886104 reaches repaired thunk with net zero adjustment");
  return failures ? 1 : 0;
}
