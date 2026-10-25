#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <process.h>
#include <string>
#include <windows.h>

static std::wstring findCompiler() {
  if (const auto explicitPath = _wgetenv(L"IMGBP_GLSLC")) {
    if (!std::filesystem::is_regular_file(explicitPath))
      throw std::runtime_error("IMGBP_GLSLC does not point to a file");
    return explicitPath;
  }
  if (const auto sdk = _wgetenv(L"VULKAN_SDK")) {
    auto candidate = std::filesystem::path(sdk) / L"Bin/glslc.exe";
    if (std::filesystem::is_regular_file(candidate)) return candidate.wstring();
  }
  wchar_t found[32768];
  const auto count = SearchPathW(nullptr, L"glslc.exe", nullptr, 32768, found, nullptr);
  if (count && count < 32768) return found;
  if (std::filesystem::is_regular_file(GLSLC_PATH)) return GLSLC_PATH;
  throw std::runtime_error("glslc not found. Install Vulkan SDK or set IMGBP_GLSLC.");
}
int wmain(int argc, wchar_t** argv) {
  try {
    if (argc != 3) {
      std::cerr << "Usage: imgbp-shaderc source.comp output.spv\n";
      return 1;
    }
    const std::filesystem::path source = argv[1], output = argv[2];
    if (!std::filesystem::is_regular_file(source))
      throw std::runtime_error("Source not found");
    std::filesystem::create_directories(output.has_parent_path() ? output.parent_path() : L".");
    const auto compiler = findCompiler();
    const wchar_t* args[] = {compiler.c_str(), L"--target-env=vulkan1.1",
                            L"-fshader-stage=compute", L"-O", argv[1], L"-o", argv[2], nullptr};
    const auto status = _wspawnv(_P_WAIT, compiler.c_str(), args);
    if (status != 0) throw std::runtime_error("glslc failed: " + std::to_string(status));
    std::ifstream file(output, std::ios::binary);
    uint32_t magic = 0;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (!file || magic != 0x07230203) throw std::runtime_error("Invalid SPIR-V output");
    std::cout << "SPIR-V ready\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 2;
  }
}
