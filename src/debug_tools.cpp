// Reverse-engineering helpers (off by default).
//
// --bully_find_texture=WxH:format   Every 5 s, scan committed guest memory for Xenos texture
//                                   fetch constants of that size and format (e.g.
//                                   1024x1024:36 for a k_32_FLOAT 2D texture) and log where
//                                   they are. A D3D texture object embeds one, so this finds
//                                   the guest object behind a texture seen in GPU traces.
// --bully_watch_address=0xADDR      Hardware write breakpoint on that guest address in every
//                                   thread. Each hit's host call stack (module-relative
//                                   addresses) is logged; resolve the BullyRecomp.exe ones
//                                   against the linker map to get the guest functions.

#include <rex/cvar.h>
#include <rex/logging.h>

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

REXCVAR_DEFINE_STRING(bully_find_texture, "", "Debug",
                      "Log guest texture fetch constants matching WxH:format (e.g. 1024x1024:36)");
REXCVAR_DEFINE_STRING(bully_find_dwords, "", "Debug",
                      "Log guest addresses holding these big-endian dwords in a "
                      "row, comma-separated hex (e.g. 44800000,44800000)");
REXCVAR_DEFINE_STRING(bully_watch_address, "", "Debug",
                      "Hardware write breakpoint on a guest address; logs the call stacks");

namespace bully {
namespace {

uint8_t* g_membase = nullptr;

// --- Fetch-constant scan -----------------------------------------------------------------

void ScanForTexture(uint32_t width, uint32_t height, uint32_t format,
                    std::unordered_set<uint32_t>& reported) {
  const uint32_t size_dword = (width - 1) | ((height - 1) << 13);
  // Guest virtual heaps and the physical views; skip anything not committed on the host.
  for (uint64_t guest = 0x00010000; guest < 0xFFFF0000ull;) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(g_membase + guest, &info, sizeof(info))) break;
    const uint64_t region_end =
        uint64_t(reinterpret_cast<uint8_t*>(info.BaseAddress) - g_membase) + info.RegionSize;
    if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
      const uint8_t* p = g_membase + guest;
      const uint8_t* end = g_membase + region_end;
      for (; p + 12 <= end; p += 4) {
        const uint32_t d2 = _byteswap_ulong(*reinterpret_cast<const uint32_t*>(p + 8));
        if ((d2 & 0x03FFFFFF) != size_dword) continue;
        const uint32_t d1 = _byteswap_ulong(*reinterpret_cast<const uint32_t*>(p + 4));
        const uint32_t d0 = _byteswap_ulong(*reinterpret_cast<const uint32_t*>(p));
        if ((d1 & 0x3F) != format || (d0 & 3) != 2) continue;  // type 2 = texture
        const uint32_t address = uint32_t(p - g_membase);
        if (reported.insert(address).second) {
          REXLOG_WARN("find_texture: fetch constant at guest {:08X}: {:08X} {:08X} {:08X} "
                      "(texture data at {:08X})",
                      address, d0, d1, d2, (d1 >> 12) << 12);
        }
      }
    }
    guest = std::max<uint64_t>(region_end, guest + 0x1000);
  }
}

void ScanForDwords(const std::vector<uint32_t>& pattern, std::unordered_set<uint32_t>& reported) {
  // All guest memory; matches in the physical views (0xA0000000+) are usually GPU command
  // buffer copies.
  for (uint64_t guest = 0x00010000; guest < 0xFFFF0000ull;) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(g_membase + guest, &info, sizeof(info))) break;
    const uint64_t region_end =
        uint64_t(reinterpret_cast<uint8_t*>(info.BaseAddress) - g_membase) + info.RegionSize;
    if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
      const uint8_t* end = g_membase + region_end;
      for (const uint8_t* p = g_membase + guest; p + 4 * pattern.size() <= end; p += 4) {
        bool match = true;
        for (size_t i = 0; i < pattern.size() && match; ++i) {
          match = _byteswap_ulong(reinterpret_cast<const uint32_t*>(p)[i]) == pattern[i];
        }
        if (match && reported.size() < 256 && reported.insert(uint32_t(p - g_membase)).second) {
          REXLOG_WARN("find_dwords: match at guest {:08X}", uint32_t(p - g_membase));
        }
      }
    }
    guest = std::max<uint64_t>(region_end, guest + 0x1000);
  }
}

// --- Hardware write breakpoint -----------------------------------------------------------

struct StackRecord {
  DWORD thread_id;
  int count;
  uint64_t frames[24];
  char modules[24];  // 'E' = BullyRecomp.exe, 'R' = rexruntime.dll, '?' = other
};
constexpr int kMaxRecords = 32;
StackRecord g_records[kMaxRecords];
std::atomic<int> g_record_count{0};
uint64_t g_watch_host = 0;
uint64_t g_exe_base = 0, g_runtime_base = 0;

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  CONTEXT* ctx = info->ContextRecord;
  if (!(ctx->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
  ctx->Dr6 = 0;
  const int index = g_record_count.fetch_add(1);
  if (index < kMaxRecords) {
    StackRecord& r = g_records[index];
    r.thread_id = GetCurrentThreadId();
    CONTEXT unwind = *ctx;
    r.count = 0;
    for (int depth = 0; depth < 24 && unwind.Rip; ++depth) {
      DWORD64 image_base = 0;
      auto* entry = RtlLookupFunctionEntry(unwind.Rip, &image_base, nullptr);
      r.frames[r.count] = unwind.Rip - image_base;
      r.modules[r.count] = image_base == g_exe_base ? 'E' : image_base == g_runtime_base ? 'R' : '?';
      ++r.count;
      if (!entry) break;
      PVOID handler_data = nullptr;
      DWORD64 frame = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, unwind.Rip, entry, &unwind, &handler_data,
                       &frame, nullptr);
    }
  } else {
    // Enough samples: disable the breakpoint on this thread.
    ctx->Dr7 &= ~uint64_t(3);
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}

void ArmThreads(std::unordered_set<DWORD>& armed) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 entry{sizeof(entry)};
  for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != GetCurrentProcessId() ||
        entry.th32ThreadID == GetCurrentThreadId() || armed.count(entry.th32ThreadID)) {
      continue;
    }
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                               FALSE, entry.th32ThreadID);
    if (!thread) continue;
    if (SuspendThread(thread) != DWORD(-1)) {
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (GetThreadContext(thread, &ctx)) {
        ctx.Dr0 = g_watch_host;
        // L0 enable, RW0 = 01 (write), LEN0 = 11 (4 bytes).
        ctx.Dr7 = (ctx.Dr7 & ~0xF0003ull) | 1 | (1ull << 16) | (3ull << 18);
        if (SetThreadContext(thread, &ctx)) armed.insert(entry.th32ThreadID);
      }
      ResumeThread(thread);
    }
    CloseHandle(thread);
  }
  CloseHandle(snapshot);
}

}  // namespace

void StartDebugTools(uint8_t* membase) {
  g_membase = membase;
  const std::string find = REXCVAR_GET(bully_find_texture);
  const std::string watch = REXCVAR_GET(bully_watch_address);
  if (!find.empty()) {
    uint32_t w = 0, h = 0, f = 0;
    if (sscanf_s(find.c_str(), "%ux%u:%u", &w, &h, &f) == 3 && w && h) {
      std::thread([w, h, f] {
        std::unordered_set<uint32_t> reported;
        for (;;) {
          std::this_thread::sleep_for(std::chrono::seconds(5));
          ScanForTexture(w, h, f, reported);
        }
      }).detach();
    }
  }
  const std::string dwords = REXCVAR_GET(bully_find_dwords);
  if (!dwords.empty()) {
    std::vector<uint32_t> pattern;
    size_t start = 0;
    while (start < dwords.size()) {
      size_t comma = dwords.find(',', start);
      if (comma == std::string::npos) comma = dwords.size();
      pattern.push_back(uint32_t(std::stoul(dwords.substr(start, comma - start), nullptr, 16)));
      start = comma + 1;
    }
    std::thread([pattern] {
      std::unordered_set<uint32_t> reported;
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        ScanForDwords(pattern, reported);
      }
    }).detach();
  }
  if (!watch.empty()) {
    const uint32_t guest = uint32_t(std::stoul(watch, nullptr, 16));
    g_watch_host = reinterpret_cast<uint64_t>(membase + guest);
    g_exe_base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    g_runtime_base = reinterpret_cast<uint64_t>(GetModuleHandleW(L"rexruntime.dll"));
    AddVectoredExceptionHandler(1, WatchHandler);
    REXLOG_WARN("watch_address: write breakpoint on guest {:08X}", guest);
    std::thread([] {
      std::unordered_set<DWORD> armed;
      int printed = 0;
      for (;;) {
        ArmThreads(armed);
        const int available = std::min(g_record_count.load(), kMaxRecords);
        for (; printed < available; ++printed) {
          const StackRecord& r = g_records[printed];
          std::string text;
          for (int i = 0; i < r.count; ++i) {
            char frame[32];
            snprintf(frame, sizeof(frame), " %c:%llx", r.modules[i],
                     static_cast<unsigned long long>(r.frames[i]));
            text += frame;
          }
          REXLOG_WARN("watch_address hit {} (thread {}):{}", printed, r.thread_id, text);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
    }).detach();
  }
}

}  // namespace bully
