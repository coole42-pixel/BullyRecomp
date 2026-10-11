#include "gpu_diagnostics.h"
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/graphics_system.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <fstream>
#include <array>
#include <windows.h>

namespace {
class DiagnosticProcessor : public rex::graphics::d3d12::D3D12CommandProcessor {
 public:
  DiagnosticProcessor(rex::graphics::d3d12::D3D12GraphicsSystem* graphics,
                      rex::system::KernelState* kernel,
                      std::filesystem::path directory)
      : D3D12CommandProcessor(graphics, kernel), directory_(std::move(directory)) {}

 protected:
  bool IssueDraw(rex::graphics::xenos::PrimitiveType primitive, uint32_t count,
                 IndexBufferInfo* indices, bool explicit_mode) override {
    const auto frame = GetCurrentFrame();
    if (last_frame_ != frame) {
      last_frame_ = frame;
      std::ifstream request(directory_ / "capture-draws.txt");
      std::string token((std::istreambuf_iterator<char>(request)), {});
      if (!token.empty() && token != last_request_) {
        last_request_ = token;
        remaining_ = 96;
      }
    }
    const bool capture = remaining_ != 0;
    const auto id = next_id_;
    if (capture) {
      --remaining_;
      ++next_id_;
      const auto prefix = "draw-" + std::to_string(id);
      std::ofstream registers(directory_ / (prefix + "-registers.bin"), std::ios::binary);
      registers.write(reinterpret_cast<const char*>(register_file_->values),
                      sizeof(register_file_->values));
      for (unsigned slot = 0; slot < 96; ++slot) {
        const auto fetch = register_file_->GetVertexFetch(slot);
        if (unsigned(fetch.type) != 3 || !fetch.size) continue;
        const auto address = uint32_t(fetch.address << 2);
        const auto length = std::min<size_t>(4096, size_t(fetch.size) * 4);
        if (uint64_t(address) + length > 0x20000000) continue;
        std::array<char, 4096> bytes{};
        SIZE_T copied = 0;
        if (ReadProcessMemory(GetCurrentProcess(), memory_->TranslatePhysical(address),
                              bytes.data(), length, &copied) && copied) {
          std::ofstream buffer(directory_ / (prefix + "-vertex-" + std::to_string(slot) + ".bin"),
                               std::ios::binary);
          buffer.write(bytes.data(), copied);
        }
      }
    }
    const bool result = D3D12CommandProcessor::IssueDraw(primitive, count, indices, explicit_mode);
    if (capture) {
      std::ofstream summary(directory_ / "draws.csv", std::ios::app);
      summary << id << ',' << frame << ',' << unsigned(primitive) << ',' << count << ','
              << std::hex << (active_vertex_shader() ? active_vertex_shader()->ucode_data_hash() : 0)
              << ',' << (active_pixel_shader() ? active_pixel_shader()->ucode_data_hash() : 0)
              << std::dec << ',' << result << '\n';
    }
    return result;
  }

 private:
  std::filesystem::path directory_;
  uint64_t last_frame_ = UINT64_MAX;
  std::string last_request_;
  unsigned remaining_ = 0;
  unsigned next_id_ = 0;
};

class DiagnosticGraphics : public rex::graphics::d3d12::D3D12GraphicsSystem {
 public:
  explicit DiagnosticGraphics(std::filesystem::path directory) : directory_(std::move(directory)) {}
 protected:
  std::unique_ptr<rex::graphics::CommandProcessor> CreateCommandProcessor() override {
    return std::make_unique<DiagnosticProcessor>(this, kernel_state_, directory_);
  }
 private:
  std::filesystem::path directory_;
};
}

std::unique_ptr<rex::system::IGraphicsSystem> CreateDiagnosticGraphics(
    const std::filesystem::path& directory) {
  std::filesystem::create_directories(directory);
  return std::make_unique<DiagnosticGraphics>(directory);
}
