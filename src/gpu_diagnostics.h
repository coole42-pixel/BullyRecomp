#pragma once
#include <rex/system/interfaces/graphics.h>
#include <memory>
#include <filesystem>
std::unique_ptr<rex::system::IGraphicsSystem> CreateDiagnosticGraphics(
    const std::filesystem::path& directory);
