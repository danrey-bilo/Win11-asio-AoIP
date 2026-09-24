#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <iphlpapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "aoip/profile.hpp"
#include "aoip/protocol.hpp"
#include "aoip/diagnostics.hpp"
#include "../platform/realtime.hpp"
namespace piaoip {
inline constexpr CLSID kClsid = {0xa24d50b2, 0x9111, 0x4a6b, {0x9c, 0x29, 0xa0, 0x1d, 0x61, 0x7b, 0xc8, 0x30}};
inline constexpr uint16_t kControlPort=50022;
extern HMODULE g_module;
extern std::atomic<long> g_objects;
struct Config {
  char peer[64] = ""; // Empty: discover a single peer on wired IPv4 networks.
  uint16_t port = 50021;
  uint16_t peer_port = 50020;
  uint32_t rate = 192000;
  uint16_t channels = 8;
  uint16_t bits = 32;
  long block = 64;
  unsigned inputs = 65535, outputs = 65535;
  unsigned safety = 512;
};

struct SettingsDialog {
  Config current;
  wchar_t path[MAX_PATH]{};
  bool is_live = false;
  bool changed = false;
  bool measuring = false;
  void* driver = nullptr;
  HFONT heading_font = nullptr;
  HBRUSH background = nullptr;
  char measure_peer[64]{};
  struct Device {
    char ip[64]{};
    unsigned channels = 0, rate = 0, bits = 0, frames = 0, port = 0;
    unsigned max_channels = 0, outputs = 0, max_outputs = 0;
    bool v2 = false;
    unsigned max_pps = 0;
    unsigned link_mbps = 0;
    std::vector<unsigned> rates, supported_bits, buffers;
  };
  std::vector<Device> devices;
  std::vector<Device> scan_results;
  std::thread scanner;
  bool scanning = false;
  int selected_device = -1;
};


void read_config(Config& c);
bool profile_path(wchar_t (&path)[MAX_PATH]);
bool control_request(const char*, const char*, const char*, char (&)[512], sockaddr_in&, DWORD);
bool parse_device(const char*, const sockaddr_in&, SettingsDialog::Device&);
bool query_device(const char*, SettingsDialog::Device&, DWORD timeout=500);
void discover_devices(SettingsDialog&);
bool profile_fits_link(unsigned,unsigned,unsigned,unsigned,unsigned,unsigned);
bool show_settings_dialog(const Config&, void* driver=nullptr);
}
