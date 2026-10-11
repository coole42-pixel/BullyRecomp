// Adaptive spinning for guest critical sections (OPTIMIZATION.md, Phase 4).
//
// The game's heap (malloc/free at sub_823BED50 / sub_823BF300) is guarded by an
// RTL_CRITICAL_SECTION with no spin count. The runtime's RtlEnterCriticalSection then
// falls straight into a host kernel wait on any contention, and the main thread spent
// ~18% of its time blocked there in the busy tutorial yard. Lock holds are short, so a
// brief spin almost always acquires the lock without a kernel round trip.
//
// This replaces the import thunk the generated code calls. It uses the runtime's exact
// lock protocol (lock_count CAS -1 -> 0 at +0x10, big-endian recursion count at +0x14 and
// owner at +0x18), and anything it does not handle (recursion, a lock still held after the
// spin) is passed to the original runtime implementation unchanged.

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/xthread.h>

#include <intrin.h>
#include <windows.h>

REXCVAR_DEFINE_INT32(bully_cs_spin_us, 20, "Performance",
                     "Microseconds to spin on a contended guest critical section before "
                     "blocking in the kernel (0 = runtime default behaviour)");

namespace {

PPCFunc* OriginalEnterCriticalSection() {
  static PPCFunc* original = reinterpret_cast<PPCFunc*>(
      GetProcAddress(GetModuleHandleW(L"rexruntime.dll"), "__imp__RtlEnterCriticalSection"));
  return original;
}

uint64_t SpinBudgetTicks() {
  // TSC ticks per microsecond, measured once against QPC.
  static const uint64_t ticks_per_us = [] {
    LARGE_INTEGER frequency, start, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    const uint64_t tsc_start = __rdtsc();
    do QueryPerformanceCounter(&now);
    while (now.QuadPart - start.QuadPart < frequency.QuadPart / 200);  // 5 ms
    const double us = 1e6 * double(now.QuadPart - start.QuadPart) / frequency.QuadPart;
    return uint64_t(double(__rdtsc() - tsc_start) / us);
  }();
  return ticks_per_us * uint64_t(REXCVAR_GET(bully_cs_spin_us));
}

}  // namespace

extern "C" REX_FUNC(__imp__RtlEnterCriticalSection) {
  PPCFunc* original = OriginalEnterCriticalSection();
  const int32_t spin_us = REXCVAR_GET(bully_cs_spin_us);
  if (spin_us > 0) {
    const uint32_t address = ctx.r3.u32;
    uint8_t* cs = base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
    auto* lock_count = reinterpret_cast<volatile long*>(cs + 0x10);
    auto* recursion = reinterpret_cast<volatile uint32_t*>(cs + 0x14);
    auto* owner = reinterpret_cast<volatile uint32_t*>(cs + 0x18);
    const uint32_t self = rex::system::XThread::GetCurrentThread()->guest_object();
    if (_byteswap_ulong(*owner) != self) {
      // -1 (free) and 0 (held, no waiters) are the same in either byte order.
      const uint64_t deadline = __rdtsc() + SpinBudgetTicks();
      do {
        if (*lock_count == -1 && _InterlockedCompareExchange(lock_count, 0, -1) == -1) {
          *owner = _byteswap_ulong(self);
          *recursion = _byteswap_ulong(1);
          return;
        }
        _mm_pause();
      } while (__rdtsc() < deadline);
    }
  }
  original(ctx, base);
}
