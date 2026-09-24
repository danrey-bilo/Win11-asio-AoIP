#include "settings.hpp"
#include <shlobj.h>
#include <cwchar>
namespace piaoip {
namespace {
bool copy_path(const std::wstring& source, wchar_t (&path)[MAX_PATH]) {
  if (source.empty() || source.size() >= MAX_PATH) return false;
  std::wmemcpy(path, source.c_str(), source.size() + 1);
  return true;
}
bool beside_module(wchar_t (&path)[MAX_PATH]) {
  DWORD count = GetModuleFileNameW(g_module, path, MAX_PATH);
  if (!count || count >= MAX_PATH) return false;
  std::wstring value(path);
  auto slash = value.find_last_of(L"\\/");
  return slash != std::wstring::npos &&
      copy_path(value.substr(0, slash + 1) + L"PiAoipAsio.ini", path);
}
bool is_file(const wchar_t* path) {
  DWORD attributes = GetFileAttributesW(path);
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}
bool ensure_parent(const wchar_t* path) {
  std::wstring value(path);
  auto slash = value.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return false;
  auto parent = value.substr(0, slash);
  int result = SHCreateDirectoryExW(nullptr, parent.c_str(), nullptr);
  return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS || result == ERROR_FILE_EXISTS;
}
void read_ini(const wchar_t* path, Config& c) {
  wchar_t peer[64]{};
  MultiByteToWideChar(CP_UTF8, 0, c.peer, -1, peer, 64);
  GetPrivateProfileStringW(L"AoIP", L"PeerIp", peer, peer, 64, path);
  if (!WideCharToMultiByte(CP_UTF8, 0, peer, -1, c.peer, sizeof(c.peer), nullptr, nullptr))
    c.peer[0] = 0;
  auto number = [&](const wchar_t* name, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(L"AoIP", name, fallback, path));
  };
  int rate = number(L"Rate", c.rate), bits = number(L"Bits", c.bits);
  int block = number(L"BufferFrames", c.block), guard = number(L"SafetyFrames", c.safety);
  if (rate > 0 && aoip::valid_rate(rate)) c.rate = rate;
  if (bits > 0 && aoip::valid_bits(bits)) c.bits = static_cast<uint16_t>(bits);
  if (block > 0 && aoip::valid_buffer(block)) c.block = block;
  if (guard >= 0 && guard <= 2048) c.safety = guard;
  int inputs = number(L"Inputs", c.inputs), outputs = number(L"Outputs", c.outputs);
  if (inputs >= 0 && inputs <= 64) c.inputs = static_cast<uint16_t>(inputs);
  if (outputs >= 0 && outputs <= 64) c.outputs = static_cast<uint16_t>(outputs);
}
void read_legacy_registry(Config& c) {
  constexpr auto key = L"Software\\PiAoIP\\ASIO";
  wchar_t peer[64]{};
  DWORD size = sizeof(peer);
  if (RegGetValueW(HKEY_CURRENT_USER, key, L"PeerIp", RRF_RT_REG_SZ,
                   nullptr, peer, &size) == ERROR_SUCCESS)
    WideCharToMultiByte(CP_UTF8, 0, peer, -1, c.peer, sizeof(c.peer), nullptr, nullptr);
  auto number = [&](const wchar_t* name, DWORD fallback) {
    DWORD value = fallback, bytes = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD,
                        nullptr, &value, &bytes) == ERROR_SUCCESS ? value : fallback;
  };
  auto rate = number(L"Rate", c.rate), bits = number(L"Bits", c.bits);
  auto block = number(L"BufferFrames", c.block);
  if (aoip::valid_rate(rate)) c.rate = rate;
  if (aoip::valid_bits(bits)) c.bits = static_cast<uint16_t>(bits);
  if (aoip::valid_buffer(block)) c.block = block;
}
void read_previous_install(Config& c) {
  wchar_t path[MAX_PATH]{};
  DWORD bytes = sizeof(path);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\PiAoIP", L"LegacyDriverPath",
                   RRF_RT_REG_SZ, nullptr, path, &bytes) != ERROR_SUCCESS) return;
  std::wstring value(path);
  auto slash = value.find_last_of(L"\\/");
  if (slash != std::wstring::npos &&
      copy_path(value.substr(0, slash + 1) + L"PiAoipAsio.ini", path) && is_file(path))
    read_ini(path, c);
}
}

bool profile_path(wchar_t (&path)[MAX_PATH]) {
  wchar_t override_path[MAX_PATH]{};
  DWORD count = GetEnvironmentVariableW(L"PIAOIP_CONFIG_PATH", override_path, MAX_PATH);
  if (count) {
    if (count >= MAX_PATH) return false;
    count = GetFullPathNameW(override_path, MAX_PATH, path, nullptr);
    return count && count < MAX_PATH && ensure_parent(path);
  }
  // Explicit portable profiles remain isolated from installed per-user settings.
  if (beside_module(path) && is_file(path)) return true;
  PWSTR folder = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &folder))) return false;
  std::wstring value = std::wstring(folder) + L"\\PiAoIP\\PiAoipAsio.ini";
  CoTaskMemFree(folder);
  return copy_path(value, path) && ensure_parent(path);
}

void read_config(Config& c) {
  read_legacy_registry(c);
  wchar_t path[MAX_PATH]{};
  if (!profile_path(path)) return;
  // First MSI launch can import a previous portable installation, but writes
  // always use the new user's own profile. Explicit test overrides never import.
  if (!is_file(path) && !GetEnvironmentVariableW(L"PIAOIP_CONFIG_PATH", nullptr, 0))
    read_previous_install(c);
  read_ini(path, c);
}
}
