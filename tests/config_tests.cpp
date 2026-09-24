#include "../src/config/settings.hpp"
#include <cwchar>
#include <iostream>
#include <stdexcept>
namespace piaoip { HMODULE g_module = GetModuleHandleW(nullptr); }
static void require(bool condition, const char* error) {
  if (!condition) throw std::runtime_error(error);
}
int main() try {
  wchar_t temporary[MAX_PATH]{};
  require(GetTempPathW(MAX_PATH, temporary) != 0, "temporary folder unavailable");
  const auto folder = std::wstring(temporary) + L"PiAoIP-config-" + std::to_wstring(GetCurrentProcessId()) + L"-\u041f\u0440\u043e\u0444\u0438\u043b\u044c";
  const auto file = folder + L"\\profile.ini";
  require(SetEnvironmentVariableW(L"PIAOIP_CONFIG_PATH", file.c_str()), "override failed");
  wchar_t resolved[MAX_PATH]{};
  require(piaoip::profile_path(resolved) && file == resolved, "Unicode profile path mismatch");
  auto write = [&](const wchar_t* key, const wchar_t* value) {
    require(WritePrivateProfileStringW(L"AoIP", key, value, resolved), "INI write failed");
  };
  write(L"PeerIp", L"10.31.4.22"); write(L"Inputs", L"0"); write(L"Outputs", L"64");
  write(L"Rate", L"96000"); write(L"Bits", L"24"); write(L"BufferFrames", L"128");
  write(L"SafetyFrames", L"256");
  piaoip::Config config{}; piaoip::read_config(config);
  require(!std::strcmp(config.peer, "10.31.4.22") && config.inputs == 0 && config.outputs == 64 &&
          config.rate == 96000 && config.bits == 24 && config.block == 128 && config.safety == 256,
          "Profile read / independent channels failed");
  write(L"Rate", L"384000"); write(L"Bits", L"7"); write(L"BufferFrames", L"3");
  write(L"Inputs", L"-1"); write(L"Outputs", L"999"); write(L"SafetyFrames", L"-1");
  // Start from known valid values so legacy HKCU defaults cannot affect the test.
  piaoip::Config checked = config; piaoip::read_config(checked);
  require(aoip::valid_rate(checked.rate) && aoip::valid_bits(checked.bits) && aoip::valid_buffer(checked.block) &&
          checked.inputs == 0 && checked.outputs == 64 && checked.safety == 256,
          "Invalid INI values escaped validation");
  DeleteFileW(file.c_str()); RemoveDirectoryW(folder.c_str());
  std::cout << "Windows config tests passed: Unicode paths, explicit profile, independent I/O, invalid values\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n'; return 1;
}
