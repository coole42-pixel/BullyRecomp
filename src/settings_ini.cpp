// BullyRecomp.ini support and the 4K resolution-scale cap.
//
// The INI is translated into the SDK's own config loader (rex::cvar::LoadConfig), so its values
// get exactly the "config file" priority: below command-line flags and REX_* environment
// variables, above built-in defaults, and settings registered later (by the GPU plugin) are
// still applied when they appear. Keys are setting (cvar) names; [sections] only group them.

#include "settings_ini.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

REXCVAR_DEFINE_STRING(bully_ini, "", "Settings",
                      "Settings INI path (empty = BullyRecomp.ini next to the executable, "
                      "'none' = do not load one)");

namespace bully {
namespace {

constexpr int kMaxScale = 4;
constexpr int kMaxOutputWidth = 3840;
constexpr int kMaxOutputHeight = 2160;

constexpr const char* kDefaultIni = R"(; BullyRecomp settings
; Command-line flags (as in play.cmd) override values here. Each key is a setting name;
; [sections] are only for grouping. Lines starting with ; or # are comments.
; Any other setting can be added too, e.g. keybind_a = Space (see the SDK's flag list).

[Graphics]
; Internal render resolution: 1 (1280x720), 2 (2560x1440), 3 (3840x2160), 4, or max.
; Capped at 3840x2160 (4K), so 4 and max both give 3x for this 720p game.
; Each step costs a lot of GPU time (2x = 4 times the pixels).
resolution_scale = 1
; Upscaling/sharpening when drawing to the window: bilinear, cas, fsr, fsr2, fsr3
present_effect = bilinear
; Post-process anti-aliasing on the game image: none, fxaa, fxaa_extreme
swap_post_effect = none
; Anisotropic filtering: -1 = game setting, 0 = off, 1 = 1x, 2 = 2x, 3 = 4x, 4 = 8x, 5 = 16x
anisotropic_override = 3
fullscreen = true
; Window size when not fullscreen (0 = default)
window_width = 0
window_height = 0
; Let G-Sync/FreeSync displays follow the game's frame rate
d3d12_allow_variable_refresh_rate_and_tearing = true

[Performance]
; Compile new shaders in the background (brief pop-in) instead of freezing the game
async_shader_compilation = true
; Emulated console vblank. false = smooth 60 fps; true caps busy scenes at 30 fps.
vsync = false
; Microseconds to spin on a busy game lock before sleeping (0 = runtime default)
bully_cs_spin_us = 20

[Input]
; Keyboard and mouse as a controller (WASD move, arrows camera, Space = A, ...)
mnk_mode = true
; Use the mouse for the camera
mnk_mouse = false
mnk_sensitivity = 1.0

[Audio]
audio_mute = false

[Logging]
; trace, debug, info, warn, error, critical, off
log_level = warn
)";

std::string Trim(std::string s) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

std::string TomlString(const std::string& value) {
  std::string out = "\"";
  for (char c : value) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

int MaxScaleFor4K() {
  const int width = std::max(1, rex::cvar::Query<int32_t>("video_mode_width"));
  const int height = std::max(1, rex::cvar::Query<int32_t>("video_mode_height"));
  return std::clamp(std::min(kMaxOutputWidth / width, kMaxOutputHeight / height), 1, kMaxScale);
}

}  // namespace

std::vector<std::string> LoadSettingsIni(const std::filesystem::path& exe_dir) {
  std::vector<std::string> keys;
  const std::string setting = REXCVAR_GET(bully_ini);
  if (ToLower(setting) == "none") return keys;
  const std::filesystem::path path =
      setting.empty() ? exe_dir / "BullyRecomp.ini" : std::filesystem::path(setting);

  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    std::ofstream out(path);
    out << kDefaultIni;
    if (!out) return keys;
  }
  std::ifstream in(path);
  if (!in) return keys;

  std::ostringstream toml;
  std::string line;
  int line_number = 0;
  while (std::getline(in, line)) {
    ++line_number;
    // Strip comments (; or # outside a quoted value is good enough for these settings).
    const auto comment = line.find_first_of(";#");
    if (comment != std::string::npos) line.resize(comment);
    line = Trim(line);
    if (line.empty() || line.front() == '[') continue;
    const auto equals = line.find('=');
    if (equals == std::string::npos) continue;
    std::string key = ToLower(Trim(line.substr(0, equals)));
    std::string value = Trim(line.substr(equals + 1));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.size() - 2);
    }
    if (key.empty()) continue;
    // "max" is the largest scale that still fits in 4K (applied by ApplyResolutionScaleCap).
    if (key == "resolution_scale" && ToLower(value) == "max") value = std::to_string(kMaxScale);
    toml << key << " = " << TomlString(value) << '\n';
    keys.push_back(key);
  }

  // LoadConfig only reads files, so hand it the translated settings through a temp file.
  wchar_t temp_dir[MAX_PATH];
  GetTempPathW(MAX_PATH, temp_dir);
  const auto temp = std::filesystem::path(temp_dir) /
                    ("BullyRecomp-ini-" + std::to_string(GetCurrentProcessId()) + ".toml");
  {
    std::ofstream out(temp, std::ios::trunc);
    out << toml.str();
  }
  rex::cvar::LoadConfig(temp);
  std::filesystem::remove(temp, error);
  return keys;
}

void ReportUnknownIniKeys(const std::vector<std::string>& keys) {
  for (const auto& key : keys) {
    if (!rex::cvar::GetFlagInfo(key)) {
      REXLOG_WARN("BullyRecomp.ini: unknown setting '{}' ignored", key);
    }
  }
}

void ApplyResolutionScaleCap() {
  // Same precedence as the SDK: a non-default resolution_scale sets both axes unless an
  // axis was set explicitly.
  const int shared = rex::cvar::Query<int32_t>("resolution_scale");
  const bool use_shared = rex::cvar::HasNonDefaultValue("resolution_scale");
  const int x = use_shared && !rex::cvar::HasNonDefaultValue("draw_resolution_scale_x")
                    ? shared
                    : rex::cvar::Query<int32_t>("draw_resolution_scale_x");
  const int y = use_shared && !rex::cvar::HasNonDefaultValue("draw_resolution_scale_y")
                    ? shared
                    : rex::cvar::Query<int32_t>("draw_resolution_scale_y");
  const int cap = MaxScaleFor4K();
  const int capped_x = std::clamp(x, 1, cap);
  const int capped_y = std::clamp(y, 1, cap);
  const int width = rex::cvar::Query<int32_t>("video_mode_width");
  const int height = rex::cvar::Query<int32_t>("video_mode_height");
  if (capped_x != x || capped_y != y) {
    REXLOG_WARN("Resolution scale {}x{} would render {}x{}; capped to {}x{} ({}x{}, max 4K)", x, y,
                width * x, height * y, capped_x, capped_y, width * capped_x, height * capped_y);
    rex::cvar::SetFlagByName("draw_resolution_scale_x", std::to_string(capped_x));
    rex::cvar::SetFlagByName("draw_resolution_scale_y", std::to_string(capped_y));
    rex::cvar::SetFlagByName("resolution_scale", std::to_string(std::max(capped_x, capped_y)));
  } else if (x > 1 || y > 1) {
    REXLOG_INFO("Resolution scale {}x{}: rendering at {}x{}", x, y, width * x, height * y);
  }
}

}  // namespace bully
