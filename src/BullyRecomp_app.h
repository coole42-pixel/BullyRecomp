// BullyRecomp - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/presenter.h>
#include <fstream>
#include <thread>
#include <sstream>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <vector>
#include <unordered_map>
#include <windows.h>
#include <mmsystem.h>
#include <psapi.h>
#include <tlhelp32.h>
#include "audio_compat.h"
#include "bully_profiler.h"
#include "settings_ini.h"

namespace bully {
void StartDebugTools(uint8_t* membase);
}
#ifdef BULLYRECOMP_GPU_DIAGNOSTICS
#include "gpu_diagnostics.h"
#endif

extern std::atomic<uint64_t> g_bully_swap_count;
// QPC timestamps of recent guest swaps, indexed by swap count modulo the ring size.
inline constexpr uint64_t kBullySwapRing = 1024;
extern int64_t g_bully_swap_qpc[kBullySwapRing];
REXCVAR_DECLARE(std::string, bully_capture_dir);
REXCVAR_DECLARE(int32_t, bully_capture_interval_s);
REXCVAR_DECLARE(int32_t, bully_capture_max);
REXCVAR_DECLARE(bool, bully_gpu_diagnostics);
REXCVAR_DECLARE(std::string, bully_perf_log);
REXCVAR_DECLARE(bool, bully_profile);
REXCVAR_DECLARE(std::string, bully_profile_thread);

class BullyrecompApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<BullyrecompApp>(new BullyrecompApp(ctx, "BullyRecomp",
        PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
    // The default D3D12 render-target path leaves the 3D scene black (only 2D/HUD
    // draws appear); the ROV path renders it correctly. Respect explicit user choices.
    // The flag is registered by the GPU plugin, so load the plugin before setting it.
    if (!config.graphics && !config.gpu_plugin.empty()) {
      config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin);
    }
    if (config.graphics && rex::cvar::GetFlagByName("render_target_path_d3d12").empty()) {
      rex::cvar::SetFlagByName("render_target_path_d3d12", "rov");
    }
    // The scale flags are registered by the GPU plugin, so cap them after it has loaded and
    // before the GPU starts (they only take effect at startup).
    if (config.graphics) {
      bully::ApplyResolutionScaleCap();
    }
    // Measured defaults (OPTIMIZATION.md, Phase 1): submitting on every primary buffer end
    // keeps the guest main thread spinning on GPU fences at ~41 swaps/s; submitting lazily
    // reaches ~60. Only applied when the user has not set the flag themselves.
    if (config.graphics) {
      SetDefaultFlag("d3d12_submit_on_primary_buffer_end", "false");
      SetDefaultFlag("d3d12_pipeline_creation_threads", "8");
      // Synchronize to the display without enabling the guest vblank/sleep path,
      // which drops busy scenes to 30 fps (OPTIMIZATION.md, Phase 4).
      // Requires patches/rexglue-sdk-host-vsync.patch in the local runtime.
      SetDefaultFlag("d3d12_vsync", "true");
      SetDefaultFlag("d3d12_frame_pacing", "true");
      SetDefaultFlag("d3d12_exclusive_fullscreen", "true");
      SetDefaultFlag("d3d12_vsync_frame_rate", "60");
      SetDefaultFlag("d3d12_allow_variable_refresh_rate_and_tearing", "false");
      SetDefaultFlag("fullscreen_exclusive", "true");
      SetDefaultFlag("fullscreen_refresh_rate", "120");
      SetDefaultFlag("vsync", "false");
      // A new pipeline otherwise blocks the command processor while it compiles; seen as
      // 0.5-6 s freezes on first visits. Async skips those draws for a few frames instead.
      SetDefaultFlag("async_shader_compilation", "true");
    }
    config.audio_factory =[](rex::runtime::FunctionDispatcher* dispatcher) {
      return std::make_unique<BullyAudioSystem>(dispatcher);
    };
#ifdef BULLYRECOMP_GPU_DIAGNOSTICS
    if (REXCVAR_GET(bully_gpu_diagnostics) && !REXCVAR_GET(bully_capture_dir).empty()) {
      config.graphics = CreateDiagnosticGraphics(REXCVAR_GET(bully_capture_dir));
      config.gpu_plugin.clear();
    }
#endif
  }

  ~BullyrecompApp() override { StopCapture(); StopPerf(); }

  void OnPostSetup() override {
    bully::ReportUnknownIniKeys(ini_keys_);
    for (const char* flag : {"async_shader_compilation", "render_target_path_d3d12",
                            "occlusion_query_enable", "readback_resolve",
                            "readback_memexport", "gpu_allow_invalid_fetch_constants", "vsync",
                            "d3d12_vsync",
                            "d3d12_frame_pacing", "fullscreen_exclusive", "fullscreen_refresh_rate",
                            "d3d12_exclusive_fullscreen", "d3d12_vsync_frame_rate",
                            "bully_fps_limit",
                            "bully_motion_blur",
                            "d3d12_submit_on_primary_buffer_end", "bully_cs_spin_us",
                            "resolution_scale", "draw_resolution_scale_x",
                            "draw_resolution_scale_y", "mnk_mode", "log_level"}) {
      REXLOG_INFO("GPU setting {} = {}", flag, rex::cvar::GetFlagByName(flag));
    }
  }

  void OnPostLaunchModule(rex::system::XThread*) override {
    bully::StartDebugTools(runtime()->virtual_membase());
    StartPerf();
    const auto directory = REXCVAR_GET(bully_capture_dir);
    auto* graphics = runtime()->graphics_system();
    auto* presenter = graphics ? graphics->presenter() : nullptr;
    if (directory.empty() || !presenter) return;
    StopCapture();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
      REXLOG_WARN("Cannot create guest capture directory: {}", error.message());
      return;
    }
    capture_thread_ = std::jthread([presenter, directory](std::stop_token stop) {
      std::string last_overrides;
      const unsigned max_frames = unsigned(std::max(1, REXCVAR_GET(bully_capture_max)));
      const unsigned ticks = unsigned(std::max(1, REXCVAR_GET(bully_capture_interval_s))) * 10;
      for (unsigned frame = 0; frame < max_frames && !stop.stop_requested(); ++frame) {
        for (unsigned tick = 0; tick < ticks && !stop.stop_requested(); ++tick) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (stop.stop_requested()) break;
        // Diagnostic-only bridge to the SDK's supported hot-reload GPU flags.
        // Only active with --bully_capture_dir, and accepts no host commands.
        std::ifstream override_file(std::filesystem::path(directory) / "gpu-overrides.txt");
        if (override_file) {
          std::string text((std::istreambuf_iterator<char>(override_file)), {});
          if (text != last_overrides) {
            last_overrides = text;
            std::istringstream lines(text);
            std::string line;
            while (std::getline(lines, line)) {
              if (!line.empty() && line.back() == '\r') line.pop_back();
              const auto equals = line.find('=');
              if (equals == std::string::npos) continue;
              const auto name = line.substr(0, equals);
              if (name != "async_shader_compilation" && name != "occlusion_query_enable" &&
                  name != "query_occlusion_fake_sample_count" && name != "readback_resolve" &&
                  name != "readback_memexport_fast" && name != "gpu_allow_invalid_fetch_constants") {
                continue;
              }
              const auto value = line.substr(equals + 1);
              const bool accepted = rex::cvar::SetFlagByName(name, value);
              REXLOG_INFO("Diagnostic GPU override {} = {} (accepted={})", name, value, accepted);
            }
          }
        }
        rex::ui::RawImage image;
        if (!presenter->CaptureGuestOutput(image) || !image.width || !image.height ||
            image.stride < size_t(image.width) * 4 ||
            image.data.size() < (image.height - 1) * image.stride + size_t(image.width) * 4) {
          continue;
        }
        const auto path = std::filesystem::path(directory) /
            ("frame-" + std::to_string(frame) + ".ppm");
        std::ofstream output(path, std::ios::binary);
        output << "P6\n" << image.width << ' ' << image.height << "\n255\n";
        for (uint32_t y = 0; y < image.height; ++y) {
          const auto* row = image.data.data() + y * image.stride;
          for (uint32_t x = 0; x < image.width; ++x) {
            output.write(reinterpret_cast<const char*>(row + x * 4), 3);
          }
        }
        if (output) REXLOG_DEBUG("Captured guest frame: {}", path.string());
      }
    });
  }

  void OnShutdown() override { StopCapture(); StopPerf(); }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}

  // Runs just before the SDK reads its own config, early enough for every setting.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    ini_keys_ = bully::LoadSettingsIni(paths.config_path.parent_path());
  }

 private:
  // Sets a flag only if nothing (config file, environment, command line) has set it.
  static void SetDefaultFlag(const char* name, const char* value) {
    for (const auto& entry : rex::cvar::GetRegistry()) {
      if (entry.name == name) {
        if (entry.source == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
        return;
      }
    }
  }
  void StopCapture() {
    if (capture_thread_.joinable()) {
      capture_thread_.request_stop();
      capture_thread_.join();
    }
  }
  // Frame-time logger: polls the guest VdSwap counter (BullyFrameSwapHook) and writes one
  // CSV row per second (fps, frame-time percentiles, process CPU use, memory) to
  // --bully_perf_log. Off unless that cvar is set.
  void StartPerf() {
    const std::string path = REXCVAR_GET(bully_perf_log);
    if (path.empty()) return;
    StopPerf();
    if (REXCVAR_GET(bully_profile)) {
      const std::string name = REXCVAR_GET(bully_profile_thread);
      profile_thread_ = std::jthread(bully::RunProfiler, path + ".profile.csv",
                                     std::wstring(name.begin(), name.end()));
    }
    perf_thread_ = std::jthread([path](std::stop_token stop) {
      std::error_code error;
      std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
      std::ofstream out(path, std::ios::trunc);
      std::ofstream threads_out(path + ".threads.csv", std::ios::trunc);
      threads_out << "t_s,tid,cpu_pct,name\n";
      // Every swap's exact interval (from the QPC ring filled by BullyFrameSwapHook).
      std::ofstream frames_out(path + ".frames.csv", std::ios::trunc);
      frames_out << "t_s,frame_ms\n";
      LARGE_INTEGER qpc_frequency, qpc_start;
      QueryPerformanceFrequency(&qpc_frequency);
      QueryPerformanceCounter(&qpc_start);
      uint64_t drained = g_bully_swap_count.load(std::memory_order_acquire);
      int64_t previous_qpc = 0;
      std::unordered_map<DWORD, double> thread_cpu;
      out << "t_s,frames,fps,avg_ms,p95_ms,p99_ms,max_ms,cpu_cores,ws_mb\n";
      timeBeginPeriod(1);
      using Clock = std::chrono::steady_clock;
      const auto start = Clock::now();
      auto window_start = start;
      auto last_swap = start;
      uint64_t last_counter = 0;
      bool have_counter = false;
      std::vector<double> frame_ms;
      auto process_time = [] {
        FILETIME created, exited, kernel, user;
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        ULARGE_INTEGER k{{kernel.dwLowDateTime, kernel.dwHighDateTime}};
        ULARGE_INTEGER u{{user.dwLowDateTime, user.dwHighDateTime}};
        return double(k.QuadPart + u.QuadPart) * 100e-9;
      };
      double last_cpu = process_time();
      while (!stop.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto now = Clock::now();
        const uint64_t counter = g_bully_swap_count.load(std::memory_order_acquire);
        if (counter - drained > kBullySwapRing) {
          drained = counter - kBullySwapRing;
          previous_qpc = 0;
        }
        for (; drained < counter; ++drained) {
          const int64_t qpc = g_bully_swap_qpc[drained % kBullySwapRing];
          if (previous_qpc) {
            char fline[64];
            snprintf(fline, sizeof(fline), "%.4f,%.3f\n",
                     double(qpc - qpc_start.QuadPart) / qpc_frequency.QuadPart,
                     1000.0 * double(qpc - previous_qpc) / qpc_frequency.QuadPart);
            frames_out << fline;
          }
          previous_qpc = qpc;
        }
        if (!have_counter) {
          last_counter = counter;
          have_counter = true;
          last_swap = now;
        } else if (counter != last_counter) {
          // One sample per observed change; a change of N means N swaps in this poll.
          const double ms = std::chrono::duration<double, std::milli>(now - last_swap).count();
          const uint64_t swaps = counter - last_counter;
          for (uint64_t i = 0; i < swaps && i < 8; ++i) frame_ms.push_back(ms / swaps);
          last_counter = counter;
          last_swap = now;
        }
        const double window = std::chrono::duration<double>(now - window_start).count();
        if (window < 1.0) continue;
        std::sort(frame_ms.begin(), frame_ms.end());
        auto percentile = [&](double q) {
          return frame_ms.empty() ? 0.0
              : frame_ms[std::min(frame_ms.size() - 1, size_t(q * frame_ms.size()))];
        };
        double sum = 0;
        for (double v : frame_ms) sum += v;
        const double cpu = process_time();
        PROCESS_MEMORY_COUNTERS memory{};
        GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory));
        char line[256];
        snprintf(line, sizeof(line), "%.1f,%zu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.0f\n",
                 std::chrono::duration<double>(now - start).count(), frame_ms.size(),
                 frame_ms.size() / window, frame_ms.empty() ? 0.0 : sum / frame_ms.size(),
                 percentile(0.95), percentile(0.99), frame_ms.empty() ? 0.0 : frame_ms.back(),
                 (cpu - last_cpu) / window, memory.WorkingSetSize / 1048576.0);
        out << line;
        frames_out.flush();
        out.flush();
        // Per-thread CPU use (percent of one core) for the busiest threads this second.
        {
          struct ThreadUse { DWORD id; double percent; std::string name; };
          std::vector<ThreadUse> uses;
          HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
          if (snapshot != INVALID_HANDLE_VALUE) {
            THREADENTRY32 entry{sizeof(entry)};
            for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
              if (entry.th32OwnerProcessID != GetCurrentProcessId()) continue;
              HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
              if (!thread) continue;
              FILETIME c, e, k, u;
              if (GetThreadTimes(thread, &c, &e, &k, &u)) {
                ULARGE_INTEGER kk{{k.dwLowDateTime, k.dwHighDateTime}};
                ULARGE_INTEGER uu{{u.dwLowDateTime, u.dwHighDateTime}};
                const double total = double(kk.QuadPart + uu.QuadPart) * 100e-9;
                double& previous = thread_cpu[entry.th32ThreadID];
                const double delta = total - previous;
                previous = total;
                if (delta > 0.02 * window) {
                  PWSTR description = nullptr;
                  std::string name;
                  if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
                    for (const wchar_t* w = description; *w; ++w) name += char(*w < 128 ? *w : '?');
                    LocalFree(description);
                  }
                  std::replace(name.begin(), name.end(), ',', ' ');
                  uses.push_back({entry.th32ThreadID, 100.0 * delta / window, name});
                }
              }
              CloseHandle(thread);
            }
            CloseHandle(snapshot);
          }
          std::sort(uses.begin(), uses.end(),
                    [](const ThreadUse& a, const ThreadUse& b) { return a.percent > b.percent; });
          const double now_s = std::chrono::duration<double>(now - start).count();
          for (size_t i = 0; i < uses.size() && i < 6; ++i) {
            char tline[320];
            snprintf(tline, sizeof(tline), "%.1f,%lu,%.1f,%s\n", now_s, uses[i].id, uses[i].percent,
                     uses[i].name.c_str());
            threads_out << tline;
          }
          threads_out.flush();
        }
        frame_ms.clear();
        last_cpu = cpu;
        window_start = now;
      }
      timeEndPeriod(1);
    });
  }
  void StopPerf() {
    if (profile_thread_.joinable()) {
      profile_thread_.request_stop();
      profile_thread_.join();
    }
    if (perf_thread_.joinable()) {
      perf_thread_.request_stop();
      perf_thread_.join();
    }
  }
  std::jthread perf_thread_;
  std::jthread profile_thread_;
  std::jthread capture_thread_;
  std::vector<std::string> ini_keys_;
};
