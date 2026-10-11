// Loads the supplied XEX for offline inspection; never launches guest code.
#include <rex/runtime.h>
#include <rex/kernel/init.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

int main(int argc, char** argv) {
  if (argc != 6) {
    std::fprintf(stderr, "Usage: inspect_xex GAME_ROOT XEX START_HEX SIZE_HEX OUTPUT\n");
    return 2;
  }
  rex::Runtime runtime(std::filesystem::absolute(argv[1]));
  rex::RuntimeConfig config;
  config.tool_mode = true;
  config.kernel_init = rex::kernel::InitializeKernel;
  auto status = runtime.Setup(std::move(config));
  if (status) {
    std::fprintf(stderr, "Setup failed: %08X\n", status);
    return 1;
  }
  status = runtime.LoadXexImage(argv[2]);
  if (status) {
    std::fprintf(stderr, "Load failed: %08X\n", status);
    return 1;
  }
  const auto address = std::strtoul(argv[3], nullptr, 16);
  const auto size = std::strtoul(argv[4], nullptr, 16);
  if (address < 0x82000000 || size == 0 || address + size > 0x83160000) {
    std::fprintf(stderr, "Range outside this game's image\n");
    return 2;
  }
  std::ofstream output(argv[5], std::ios::binary);
  output.write(reinterpret_cast<const char*>(runtime.virtual_membase() + address), size);
  if (!output) return 1;
  std::printf("Wrote %lu bytes at %08lX\n", size, address);
}
