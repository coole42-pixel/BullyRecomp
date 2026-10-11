// BullyRecomp.ini: user settings file (see settings_ini.cpp).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace bully {

// Resolves the INI path from --bully_ini (empty = next to the executable, "none" = disabled),
// writes a commented default file if it does not exist, and applies it with config-file
// priority: command-line flags and REX_* environment variables still win. Returns the keys
// it applied so they can be checked once every module has registered its settings.
std::vector<std::string> LoadSettingsIni(const std::filesystem::path& exe_dir);

// Warns about INI keys that do not name a known setting (call after setup).
void ReportUnknownIniKeys(const std::vector<std::string>& keys);

// Caps the draw resolution scale at 4x and so that the scaled guest output fits in
// 3840x2160 (e.g. 3x for the game's 1280x720), adjusting the flags before the GPU starts.
void ApplyResolutionScaleCap();

}  // namespace bully
