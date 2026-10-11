// Optional sampling profiler for the guest main thread (--bully_profile).
//
// Suspends the thread named by --bully_profile_thread (default "Main XThread") ~500 times a second, unwinds its host stack
// with the PE .pdata tables, and counts self and inclusive samples per function. Results
// go to <perf log>.profile.csv as module-relative function start addresses; resolve them
// against out/build/local-win-release/BullyRecomp.map with tools/resolve-profile.py.
#pragma once

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <stop_token>
#include <string>
#include <thread>
#include <tlhelp32.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bully {

inline HANDLE FindThreadByName(const wchar_t* prefix) {
  HANDLE found = nullptr;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return nullptr;
  THREADENTRY32 entry{sizeof(entry)};
  for (BOOL ok = Thread32First(snapshot, &entry); ok && !found; ok = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != GetCurrentProcessId()) continue;
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                               FALSE, entry.th32ThreadID);
    if (!thread) continue;
    PWSTR description = nullptr;
    if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
      if (wcsncmp(description, prefix, wcslen(prefix)) == 0) found = thread;
      LocalFree(description);
    }
    if (found != thread) CloseHandle(thread);
  }
  CloseHandle(snapshot);
  return found;
}

inline void RunProfiler(std::stop_token stop, std::string output_path, std::wstring thread_prefix) {
  struct Counts { uint64_t self = 0, inclusive = 0; };
  std::unordered_map<uint64_t, Counts> functions;
  // Frames above the first BullyRecomp.exe frame (runtime DLL, ntdll, ...), keyed by a short
  // "module+rva<module+rva" chain, so time inside imports is not hidden in the exe caller.
  std::unordered_map<std::string, uint64_t> external;
  std::unordered_map<uint64_t, std::string> module_names;
  auto module_name = [&](uint64_t base) -> const std::string& {
    auto [it, inserted] = module_names.try_emplace(base);
    if (inserted) {
      wchar_t path[MAX_PATH];
      const DWORD length = GetModuleFileNameW(reinterpret_cast<HMODULE>(base), path, MAX_PATH);
      const wchar_t* name = path;
      for (DWORD i = 0; i < length; ++i) if (path[i] == L'\\' ||path[i] == L'/') name = path + i + 1;
      for (; *name && name < path + length; ++name) it->second += char(*name < 128 ? *name : '?');
    }
    return it->second;
  };
  const uint64_t module_base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
  HANDLE thread = nullptr;
  uint64_t total = 0;
  auto last_write = std::chrono::steady_clock::now();
  auto write = [&] {
    std::ofstream out(output_path, std::ios::trunc);
    out << "rva,self,inclusive,total=" << total << "\n";
    for (const auto& [rva, counts] : functions) out << std::hex << rva << std::dec << ',' << counts.self << ',' << counts.inclusive << '\n';
    std::ofstream ext(output_path + ".ext.csv", std::ios::trunc);
    ext << "chain,samples,total=" << total << "\n";
    for (const auto& [chain, count] : external) ext << chain << ',' << count << '\n';
  };
  while (!stop.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (!thread) {
      thread = FindThreadByName(thread_prefix.c_str());
      continue;
    }
    if (SuspendThread(thread) == DWORD(-1)) break;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    std::vector<uint64_t> stack;
    std::string chain;
    int chain_frames = 0;
    if (GetThreadContext(thread, &context)) {
      for (int depth = 0; depth < 48 && context.Rip; ++depth) {
        DWORD64 image_base = 0;
        auto* entry = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (!entry) break;
        if (image_base == module_base) {
          stack.push_back(entry->BeginAddress);
        } else if (stack.empty() && chain_frames < 6) {
          char frame_text[96];
          snprintf(frame_text, sizeof(frame_text), "%s%s+%lx", chain_frames ? "<" : "",
                   module_name(image_base).c_str(), static_cast<unsigned long>(entry->BeginAddress));
          chain += frame_text;
          ++chain_frames;
        }
        PVOID handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, entry, &context, &handler_data,
                         &frame, nullptr);
      }
    }
    ResumeThread(thread);
    if (stack.empty() && chain.empty()) continue;
    ++total;
    if (!chain.empty()) ++external[chain];
    if (!stack.empty()) ++functions[stack.front()].self;
    std::unordered_set<uint64_t> seen;
    for (uint64_t rva : stack) if (seen.insert(rva).second) ++functions[rva].inclusive;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_write > std::chrono::seconds(5)) {
      write();
      last_write = now;
    }
  }
  write();
  if (thread) CloseHandle(thread);
}

}  // namespace bully
