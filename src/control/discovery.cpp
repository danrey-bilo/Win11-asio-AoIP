#include "../config/settings.hpp"
namespace piaoip {
bool control_request(const char* target_ip, const char* source_ip, const char* request,
                     char (&reply)[512], sockaddr_in& responder, DWORD timeout_ms) {
  SOCKET fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd == INVALID_SOCKET) return false;
  DWORD timeout = timeout_ms;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
  BOOL broadcast = TRUE;
  setsockopt(fd, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = 0;
  bool okay = !source_ip || inet_pton(AF_INET, source_ip, &local.sin_addr) == 1;
  if (okay) okay = bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != SOCKET_ERROR;
  sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_port = htons(kControlPort);
  if (okay) okay = inet_pton(AF_INET, target_ip, &remote.sin_addr) == 1;
  if (okay) okay = sendto(fd, request, static_cast<int>(std::strlen(request)), 0,
      reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) != SOCKET_ERROR;
  if (okay) {
    int length = sizeof(responder);
    int n = recvfrom(fd, reply, sizeof(reply) - 1, 0,
        reinterpret_cast<sockaddr*>(&responder), &length);
    okay = n > 0 && responder.sin_port == htons(kControlPort) &&
      ((!source_ip && responder.sin_addr.s_addr == remote.sin_addr.s_addr) || source_ip);
    if (okay) reply[n] = 0;
  }
  closesocket(fd);
  return okay;
}

bool parse_csv(const char* input, std::vector<unsigned>& values) {
  values.clear();
  while (*input) {
    char* end = nullptr;
    unsigned long value = std::strtoul(input, &end, 10);
    if (end == input || !value || value > 768000 || (*end && *end != ',')) return false;
    values.push_back(static_cast<unsigned>(value));
    if (!*end) break;
    input = end + 1;
    if (!*input) return false;
  }
  return !values.empty();
}

bool profile_fits_link(unsigned channels, unsigned rate, unsigned bits,
                       unsigned frames, unsigned max_pps, unsigned link_mbps) {
  return aoip::fits_link(channels, rate, bits, frames, link_mbps, max_pps);
}

bool parse_device(const char* reply, const sockaddr_in& address, SettingsDialog::Device& device) {
  int consumed = 0;
  char rates[128]{}, bits[32]{}, buffers[64]{};
  device.v2 = std::strncmp(reply, "PIAOIP_DEVICE_V2 ", 16) == 0;
  char copy[512]{};
  std::snprintf(copy, sizeof(copy), "%s", reply);
  if (device.v2) copy[15] = '1';
  bool valid = std::sscanf(copy,
      "PIAOIP_DEVICE_V1 channels=%u rate=%u bits=%u frames=%u audio_port=%u "
      "max_channels=%u rates=%127s bits_supported=%31s max_pps=%u "
      "link_mbps=%u buffers=%63s%n",
      &device.channels, &device.rate, &device.bits, &device.frames, &device.port,
      &device.max_channels, rates, bits, &device.max_pps,
      &device.link_mbps, buffers, &consumed) == 11 &&
      device.max_channels >= 1 && device.max_channels <= 64 &&
      device.channels >= 1 && device.channels <= device.max_channels &&
      device.frames >= 1 && device.frames <= 256 && device.port == 50020 &&
      device.max_pps >= 1000 && device.max_pps <= 100000 &&
      device.link_mbps >= 10 && device.link_mbps <= 100000 &&
      parse_csv(rates, device.rates) && parse_csv(bits, device.supported_bits) &&
      parse_csv(buffers, device.buffers);
  if (device.v2) {
    int end = 0;
    valid = valid && std::sscanf(reply + consumed, " outputs=%u max_outputs=%u%n",
      &device.outputs, &device.max_outputs, &end) == 2 && consumed + end == int(std::strlen(reply)) &&
      device.max_outputs <= 64 && device.outputs <= device.max_outputs;
  } else {
    valid = valid && consumed == int(std::strlen(reply));
    device.outputs = device.max_outputs = device.channels;
  }
  valid = valid && aoip::valid_rate(device.rate) && aoip::valid_bits(device.bits) &&
    device.frames <= aoip::packet_frames(device.channels, device.bits);
  for (auto buffer : device.buffers) valid = valid && aoip::valid_buffer(buffer);
  for (auto bit : device.supported_bits) valid = valid && aoip::valid_bits(bit);
  if (valid) valid = inet_ntop(AF_INET, &address.sin_addr, device.ip, sizeof(device.ip)) != nullptr;
  return valid;
}

bool query_device(const char* ip, SettingsDialog::Device& device, DWORD timeout) {
  char reply[512]{}; sockaddr_in responder{};
  if (control_request(ip, nullptr, "PIAOIP_DISCOVER_V2", reply, responder, timeout) &&
      parse_device(reply, responder, device)) return true;
  return control_request(ip, nullptr, "PIAOIP_DISCOVER_V1", reply, responder, timeout) &&
      parse_device(reply, responder, device);
}

void discover_devices(SettingsDialog& state) {
  state.devices.clear();
  ULONG bytes = 15000;
  std::vector<uint8_t> buffer(bytes);
  DWORD error = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
      GAA_FLAG_SKIP_DNS_SERVER, nullptr,
      reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &bytes);
  if (error == ERROR_BUFFER_OVERFLOW) {
    buffer.resize(bytes);
    error = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
        GAA_FLAG_SKIP_DNS_SERVER, nullptr,
        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &bytes);
  }
  if (error != NO_ERROR) return;
  for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
       adapter; adapter = adapter->Next) {
    if (adapter->IfType != IF_TYPE_ETHERNET_CSMACD || adapter->OperStatus != IfOperStatusUp) continue;
    for (auto* address = adapter->FirstUnicastAddress; address; address = address->Next) {
      if (!address->Address.lpSockaddr || address->Address.lpSockaddr->sa_family != AF_INET ||
          address->OnLinkPrefixLength < 1 || address->OnLinkPrefixLength > 30) continue;
      auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address->Address.lpSockaddr);
      uint32_t mask = 0xffffffffu << (32 - address->OnLinkPrefixLength);
      in_addr directed{};
      directed.s_addr = htonl((ntohl(ipv4->sin_addr.s_addr) & mask) | ~mask);
      char local_ip[INET_ADDRSTRLEN]{}, target_ip[INET_ADDRSTRLEN]{};
      if (!inet_ntop(AF_INET, &ipv4->sin_addr, local_ip, sizeof(local_ip)) ||
          !inet_ntop(AF_INET, &directed, target_ip, sizeof(target_ip))) continue;
      char reply[512]{};
      sockaddr_in responder{};
      if (!control_request(target_ip, local_ip, "PIAOIP_DISCOVER_V1", reply, responder, 300)) continue;
      SettingsDialog::Device device{};
      if (!parse_device(reply, responder, device)) continue;
      SettingsDialog::Device upgraded{};
      if (query_device(device.ip, upgraded, 100)) device = upgraded;
      bool duplicate = false;
      for (const auto& previous : state.devices)
        if (std::strcmp(previous.ip, device.ip) == 0) duplicate = true;
      if (!duplicate) state.devices.push_back(device);
    }
  }
}

}
