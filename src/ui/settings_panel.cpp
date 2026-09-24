#include "../config/settings.hpp"
#include "iasiodrv.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <cwchar>
namespace piaoip {
void refresh_rates(HWND window, SettingsDialog& state, unsigned preferred) {
  if (state.selected_device < 0 || state.selected_device >= static_cast<int>(state.devices.size())) return;
  const auto& device = state.devices[state.selected_device];
  BOOL valid_channels=FALSE;
  unsigned channels=std::max(1u,GetDlgItemInt(window,3001,&valid_channels,FALSE));
  if(!device.v2 || !valid_channels) channels=device.channels;
  BOOL valid_bits = FALSE;
  unsigned bits = GetDlgItemInt(window, 3003, &valid_bits, FALSE);
  if (!valid_bits || !channels || !bits) return;
  unsigned max_frames = (1472 - 40) / (channels * (bits / 8));
  unsigned frames = max_frames;
  HWND combo = GetDlgItem(window, 3002);
  SendMessageA(combo, CB_RESETCONTENT, 0, 0);
  LRESULT selection = CB_ERR;
  for (unsigned value : device.rates) {
    if ((state.is_live && value > 192000) ||
        !profile_fits_link(channels, value, bits, frames,
                           device.max_pps, device.link_mbps)) continue;
    char label[32]{};
    std::snprintf(label, sizeof(label), "%u", value);
    LRESULT item = SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    if (value == preferred) selection = item;
  }
  if (selection == CB_ERR) {
    LRESULT count = SendMessageA(combo, CB_GETCOUNT, 0, 0);
    if (count > 0) selection = count - 1;
  }
  if (selection != CB_ERR) SendMessageA(combo, CB_SETCURSEL, selection, 0);
}

void show_device(HWND window, SettingsDialog& state, int index) {
  if (index < 0 || index >= static_cast<int>(state.devices.size())) return;
  state.selected_device = index;
  const auto& device = state.devices[index];
  SetDlgItemTextA(window, 3000, device.ip);
  auto fill = [&](int id, const std::vector<unsigned>& options, unsigned selected) {
    HWND combo = GetDlgItem(window, id);
    SendMessageA(combo, CB_RESETCONTENT, 0, 0);
    LRESULT chosen = CB_ERR;
    for (unsigned value : options) {
      char label[32]{};
      std::snprintf(label, sizeof(label), "%u", value);
      LRESULT item = SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
      if (value == selected) chosen = item;
    }
    if (chosen == CB_ERR && !options.empty()) chosen = 0;
    if (chosen != CB_ERR) SendMessageA(combo, CB_SETCURSEL, chosen, 0);
  };
  std::vector<unsigned> inputs, outputs;
  for(unsigned n=0;n<=device.max_channels;++n) inputs.push_back(n);
  for(unsigned n=0;n<=device.max_outputs;++n) outputs.push_back(n);
  fill(3001,inputs,std::min(state.current.inputs,device.channels));
  fill(3005,outputs,std::min(state.current.outputs,device.outputs));
  fill(3010,{0,16,32,64,128,256,512,1024,2048},state.current.safety);
  fill(3003, device.supported_bits, device.bits);
  fill(3004, device.buffers, state.current.block);
  refresh_rates(window, state, device.rate);
  char status[512]{};
  std::snprintf(status, sizeof(status), "Pi %s: %u ch / %u Hz / %u bit; %u Mb/s; packet %u frames%s",
      device.ip, device.channels, device.rate, device.bits, device.link_mbps, device.frames,
      state.is_live && device.rate > 192000 ? " (choose <=192000 Hz for Live)" : "");
  SetDlgItemTextA(window, 3103, status);
}

void refresh_measured_latency(HWND window, const SettingsDialog& state) {
  if (state.selected_device < 0 ||
      state.selected_device >= static_cast<int>(state.devices.size())) return;
  char reply[512]{};
  sockaddr_in responder{};
  if (!control_request(state.devices[state.selected_device].ip, nullptr,
                       "PIAOIP_MEASURE_STATUS_V1", reply, responder, 100)) {
    SetDlgItemTextA(window, 3105, "Digital roundtrip: Pi did not answer.");
    return;
  }
  unsigned long long count = 0, minimum = 0, maximum = 0;
  unsigned p50 = 0, p95 = 0, p99 = 0, active = 0;
  if (std::sscanf(reply,
      "PIAOIP_MEASURE_V1 count=%llu min_us=%llu p50_us=%u p95_us=%u "
      "p99_us=%u max_us=%llu active=%u",
      &count, &minimum, &p50, &p95, &p99, &maximum, &active) != 7) {
    SetDlgItemTextA(window, 3105, "Digital roundtrip: measurement unavailable.");
    return;
  }
  char label[512]{};
  if (!count)
    std::snprintf(label, sizeof(label), "Digital roundtrip: waiting for input 32 -> output 32.");
  else if(p50>=20000 || p99>=20000)
    std::snprintf(label,sizeof(label),"Measured RTT: min %llu us; max %llu us. Percentiles exceed the 20 ms histogram.",minimum,maximum);
  else
    std::snprintf(label, sizeof(label),
        "Measured Pi -> ASIO -> Pi: median %u us; p99 %u us; max %llu us (%llu packets)",
        p50, p99, maximum, count);
  SetDlgItemTextA(window, 3105, label);
}

INT_PTR CALLBACK settings_dialog_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  constexpr int kFirstEdit = 3000;
  constexpr int kScan = 3100, kDeviceList = 3101, kCheck = 3102,
                kStatus = 3103, kMeasure = 3104, kMeasured = 3105;
  auto* state = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrA(window, GWLP_USERDATA));
  if (message == WM_INITDIALOG) {
    state = reinterpret_cast<SettingsDialog*>(lparam);
    SetWindowLongPtrA(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    SetWindowTextA(window, "Pi AoIP configuration");
    state->background=CreateSolidBrush(RGB(247,249,252));
    auto add = [&](DWORD exstyle, const char* cls, const char* caption, DWORD style,
                   int x,int y,int width,int height,int id) {
      RECT r{x,MulDiv(y,82,100),x+width,MulDiv(y+height,82,100)}; MapDialogRect(window,&r);
      HWND child=CreateWindowExA(exstyle,cls,caption,WS_CHILD|WS_VISIBLE|style,
        r.left,r.top,r.right-r.left,r.bottom-r.top,window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),g_module,nullptr);
      SendMessageA(child,WM_SETFONT,SendMessageA(window,WM_GETFONT,0,0),TRUE);
      SetWindowTheme(child,L"Explorer",nullptr);
      return child;
    };
    HWND title=add(0,"STATIC","Pi AoIP",0,16,10,200,22,3300);
    LOGFONTW font{}; GetObjectW(reinterpret_cast<HFONT>(SendMessageW(window,WM_GETFONT,0,0)),sizeof(font),&font);
    font.lfHeight=font.lfHeight*17/10; font.lfWeight=FW_SEMIBOLD;
    state->heading_font=CreateFontIndirectW(&font);
    SendMessageW(title,WM_SETFONT,reinterpret_cast<WPARAM>(state->heading_font),TRUE);
    add(0,"STATIC","Network audio  /  ASIO",0,16,36,250,12,3301);
    add(0,"STATIC","DEVICE",0,16,59,150,12,3302);
    add(0,"BUTTON","Discover",WS_TABSTOP|BS_PUSHBUTTON,331,54,81,22,kScan);
    add(WS_EX_CLIENTEDGE,"LISTBOX","",WS_TABSTOP|WS_VSCROLL|LBS_NOTIFY,16,80,396,34,kDeviceList);
    add(0,"STATIC","Address",0,16,125,55,13,0);
    add(WS_EX_CLIENTEDGE,"EDIT",state->current.peer,WS_TABSTOP|ES_AUTOHSCROLL,76,120,239,21,3000);
    add(0,"BUTTON","Check link",WS_TABSTOP|BS_PUSHBUTTON,331,119,81,22,kCheck);
    add(0,"STATIC","Discovering wired devices...",0,16,148,396,25,kStatus);
    add(0,"STATIC","AUDIO FORMAT",0,16,178,200,13,3303);
    auto combo=[&](int id,const char* label,int x,int y,int width,const char* value) {
      add(0,"STATIC",label,0,x,y,width,13,0);
      HWND control=add(0,"COMBOBOX","",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,x,y+16,width,150,id);
      SendMessageA(control,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(value));
      SendMessageA(control,CB_SETCURSEL,0,0);
    };
    combo(3001,"Inputs",16,199,86,"32"); combo(3005,"Outputs",114,199,86,"32");
    combo(3002,"Sample rate / Hz",212,199,102,"192000"); combo(3003,"PCM / bits",326,199,86,"32");
    add(0,"STATIC","LATENCY",0,16,246,180,13,3304);
    combo(3004,"ASIO buffer / frames",16,266,184,"32");
    combo(3010,"Network guard / frames",212,266,200,"128");
    add(0,"STATIC","",0,16,310,396,16,3121);
    add(0,"STATIC","STREAM HEALTH",0,16,336,180,13,3305);
    add(0,"STATIC","Open this panel from the running ASIO host for live counters.",0,16,356,396,29,3120);
    add(0,"STATIC","Digital RTT: route input 32 to output 32 (32-bit PCM).",0,16,390,396,28,kMeasured);
    add(0,"BUTTON","Measure RTT",WS_TABSTOP|BS_PUSHBUTTON,16,426,93,24,kMeasure);
    add(0,"BUTTON","Close",WS_TABSTOP|BS_PUSHBUTTON,227,426,81,24,IDCANCEL);
    add(0,"BUTTON","Apply",WS_TABSTOP|BS_DEFPUSHBUTTON,321,426,91,24,IDOK);
    SetTimer(window,2,500,nullptr);
    PostMessageA(window, WM_COMMAND, MAKEWPARAM(kScan, BN_CLICKED), 0);
    return TRUE;
  }
  if(message==WM_CTLCOLORDLG || message==WM_CTLCOLORSTATIC) {
    if(!state) return FALSE;
    auto dc=reinterpret_cast<HDC>(wparam);
    SetBkColor(dc,RGB(247,249,252));
    SetTextColor(dc,GetDlgCtrlID(reinterpret_cast<HWND>(lparam))>=3300 ? RGB(28,77,146) : RGB(35,45,62));
    return reinterpret_cast<INT_PTR>(state->background);
  }
  if(message==WM_TIMER && wparam==2 && state) {
    char label[512]{};
    unsigned rate=GetDlgItemInt(window,3002,nullptr,FALSE),block=GetDlgItemInt(window,3004,nullptr,FALSE);
    unsigned guard=GetDlgItemInt(window,3010,nullptr,FALSE);
    if(rate) {
      std::snprintf(label,sizeof(label),"Block %.3f ms   /   Network guard %.3f ms   /   Digital RTT measured separately",block*1000.0/rate,guard*1000.0/rate);
      SetDlgItemTextA(window,3121,label);
    }
    if(state->driver) {
      aoip::Diagnostics d;
      if(static_cast<IASIO*>(state->driver)->future(aoip::diagnostics_selector,&d)==ASE_SUCCESS) {
        std::snprintf(label,sizeof(label),"RX %llu   |   Missing %llu   |   Late %llu   |   Missed deadlines %llu\r\nTX errors %llu   |   Expired TX %llu   |   Host overruns %llu   |   Callback max %.1f us",
          d.rx_packets,d.missing_frames,d.late_frames,d.deadline_misses,d.tx_errors,d.tx_expired_packets,d.host_overruns,d.max_callback_ns/1000.0);
        SetDlgItemTextA(window,3120,label);
      }
    }
    return TRUE;
  }
  if (message == WM_TIMER && wparam == 1 && state && state->measuring) {
    refresh_measured_latency(window, *state);
    return TRUE;
  }
  if (message == WM_CLOSE) {
    if (state && state->measuring) {
      char reply[512]{};
      sockaddr_in responder{};
      control_request(state->measure_peer, nullptr,
                      "PIAOIP_MEASURE_STOP_V1", reply, responder, 100);
      KillTimer(window, 1);
    }
    EndDialog(window, IDCANCEL);
    return TRUE;
  }
  if(message==WM_APP+1 && state) {
    if(state->scanner.joinable()) state->scanner.join();
    state->scanning=false;
    state->devices=std::move(state->scan_results);
    EnableWindow(GetDlgItem(window,kScan),TRUE);
    EnableWindow(GetDlgItem(window,kCheck),TRUE);
    EnableWindow(GetDlgItem(window,IDOK),TRUE);
    HWND list = GetDlgItem(window, kDeviceList);
    SendMessageA(list, LB_RESETCONTENT, 0, 0);
    for (const auto& device : state->devices) {
      char label[128]{};
      std::snprintf(label, sizeof(label), "%s  |  active %u ch / %u Hz / %u bit",
          device.ip, device.channels, device.rate, device.bits);
      SendMessageA(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    }
    if (state->devices.empty()) {
      state->selected_device = -1;
      SetDlgItemTextA(window, kStatus, "No Pi found. Enter its IPv4 address and click Check link.");
    } else {
      SendMessageA(list, LB_SETCURSEL, 0, 0);
      show_device(window, *state, 0);
    }
    return TRUE;
  }
  if (message != WM_COMMAND) return FALSE;
  if(LOWORD(wparam)==kScan) {
    if(state->scanning) return TRUE;
    state->scanning=true;
    SetDlgItemTextA(window,kStatus,"Searching wired Ethernet...");
    EnableWindow(GetDlgItem(window,kScan),FALSE);
    EnableWindow(GetDlgItem(window,kCheck),FALSE);
    EnableWindow(GetDlgItem(window,IDOK),FALSE);
    state->scanner=std::thread([state,window] {
      SettingsDialog result{}; result.current=state->current;
      try { discover_devices(result); } catch(...) { result.devices.clear(); }
      state->scan_results=std::move(result.devices);
      PostMessageA(window,WM_APP+1,0,0);
    });
    return TRUE;
  }
  if (LOWORD(wparam) == kDeviceList && HIWORD(wparam) == LBN_SELCHANGE) {
    show_device(window, *state, static_cast<int>(SendDlgItemMessageA(window, kDeviceList, LB_GETCURSEL, 0, 0)));
    return TRUE;
  }
  if ((LOWORD(wparam) == 3003 || LOWORD(wparam) == 3001) && HIWORD(wparam) == CBN_SELCHANGE) {
    BOOL valid = FALSE;
    unsigned selected_rate = GetDlgItemInt(window, 3002, &valid, FALSE);
    refresh_rates(window, *state, valid ? selected_rate : 0);
    return TRUE;
  }
  if (LOWORD(wparam) == kCheck) {
    char peer[64]{};
    GetDlgItemTextA(window, kFirstEdit, peer, sizeof(peer));
    SettingsDialog::Device device{};
    if (!query_device(peer,device)) {
      state->selected_device = -1;
      SetDlgItemTextA(window, kStatus, "Pi did not answer on UDP 50022 at this address.");
      return TRUE;
    }
    state->devices.push_back(device);
    show_device(window, *state, static_cast<int>(state->devices.size() - 1));
    return TRUE;
  }
  if (LOWORD(wparam) == kMeasure) {
    if (!state || state->selected_device < 0 ||
        state->selected_device >= static_cast<int>(state->devices.size())) {
      MessageBoxA(window, "Discover the Pi before measuring.", "Pi AoIP", MB_OK | MB_ICONINFORMATION);
      return TRUE;
    }
    const char* ip = state->devices[state->selected_device].ip;
    char reply[512]{};
    sockaddr_in responder{};
    if (state->measuring) {
      control_request(state->measure_peer, nullptr, "PIAOIP_MEASURE_STOP_V1", reply, responder, 100);
      state->measuring = false;
      KillTimer(window, 1);
      SetDlgItemTextA(window, kMeasure, "Measure RTT");
      refresh_measured_latency(window, *state);
    } else if (control_request(ip, nullptr, "PIAOIP_MEASURE_START_V1", reply, responder, 500) &&
               std::strcmp(reply, "PIAOIP_MEASURE_STARTED_V1 channel=32") == 0) {
      state->measuring = true;
      std::snprintf(state->measure_peer, sizeof(state->measure_peer), "%s", ip);
      SetDlgItemTextA(window, kMeasure, "Stop RTT");
      SetTimer(window, 1, 1000, nullptr);
      refresh_measured_latency(window, *state);
    } else {
      MessageBoxA(window, "Measurement requires at least 32 inputs and outputs, 32-bit PCM, and a return from input 32 to output 32.",
                  "Pi AoIP", MB_OK | MB_ICONINFORMATION);
    }
    return TRUE;
  }
  if (LOWORD(wparam) == IDCANCEL) {
    if (state && state->measuring) {
      char reply[512]{};
      sockaddr_in responder{};
      control_request(state->measure_peer, nullptr,
                      "PIAOIP_MEASURE_STOP_V1", reply, responder, 100);
      KillTimer(window, 1);
    }
    EndDialog(window, IDCANCEL);
    return TRUE;
  }
  if (LOWORD(wparam) != IDOK || !state) return FALSE;

  if (state->measuring) {
    char reply[512]{};
    sockaddr_in responder{};
    control_request(state->measure_peer, nullptr,
                    "PIAOIP_MEASURE_STOP_V1", reply, responder, 100);
    state->measuring = false;
    KillTimer(window, 1);
  }

  char peer[64]{};
  GetDlgItemTextA(window, kFirstEdit, peer, sizeof(peer));
  sockaddr_in address{};
  if (inet_pton(AF_INET, peer, &address.sin_addr) != 1) {
    MessageBoxA(window, "Enter a valid Pi IPv4 address.", "Pi AoIP", MB_OK | MB_ICONERROR);
    return TRUE;
  }
  unsigned values[3]{};
  for (int i = 0; i < 3; ++i) {
    BOOL valid = FALSE;
    values[i] = GetDlgItemInt(window, kFirstEdit + 2 + i, &valid, FALSE);
    if (!valid) {
      MessageBoxA(window, "Enter a positive integer in every numeric field.", "Pi AoIP", MB_OK | MB_ICONERROR);
      return TRUE;
    }
  }
  unsigned rate = values[0], bits = values[1], block = values[2];
  if (state->selected_device < 0 ||
      state->selected_device >= static_cast<int>(state->devices.size()) ||
      std::strcmp(state->devices[state->selected_device].ip, peer) != 0) {
    MessageBoxA(window, "Discover the Pi or check its address before saving.",
        "Pi AoIP", MB_OK | MB_ICONERROR);
    return TRUE;
  }
  const auto& device = state->devices[state->selected_device];
  BOOL valid_in=FALSE,valid_out=FALSE,valid_guard=FALSE;
  unsigned inputs=GetDlgItemInt(window,3001,&valid_in,FALSE);
  unsigned outputs=GetDlgItemInt(window,3005,&valid_out,FALSE);
  unsigned guard=GetDlgItemInt(window,3010,&valid_guard,FALSE);
  if(!valid_in || !valid_out || !valid_guard || inputs>device.max_channels || outputs>device.max_outputs ||
      guard>2048 || !aoip::valid_profile({inputs,outputs,rate,bits,block},device.link_mbps)) {
    MessageBoxA(window,"Choose a supported input/output configuration and at least one active direction.","Pi AoIP",MB_OK|MB_ICONERROR); return TRUE;
  }
  unsigned channels=device.v2 ? std::max(1u,inputs) : device.channels;
  unsigned wire_outputs=device.v2 ? outputs : device.outputs;
  auto offered = [](const std::vector<unsigned>& list, unsigned value) {
    return std::find(list.begin(), list.end(), value) != list.end();
  };
  if (!offered(device.rates, rate) || !offered(device.supported_bits, bits) ||
      !offered(device.buffers, block)) {
    MessageBoxA(window, "The selected profile is not advertised by this Pi.",
        "Pi AoIP", MB_OK | MB_ICONERROR);
    return TRUE;
  }
  if (state->is_live && rate > 192000) {
    MessageBoxA(window, "Ableton Live supports up to 192000 Hz. Choose a lower rate.",
        "Pi AoIP", MB_OK | MB_ICONERROR);
    return TRUE;
  }
  if (channels < 1 || channels > 64 || rate < 8000 || rate > 768000 ||
      (bits != 16 && bits != 24 && bits != 32) || block < 16 || block > 2048 ||
      (block & (block - 1)) ||
      uint64_t(channels) * rate * bits >= 1000000000ull) {
    MessageBoxA(window, "Profile is outside the supported limits or exceeds 1 Gbit/s PCM.",
        "Pi AoIP", MB_OK | MB_ICONERROR);
    return TRUE;
  }
  wchar_t temporary[MAX_PATH + 8]{};
  std::swprintf(temporary, MAX_PATH + 8, L"%ls.tmp", state->path);
  char body[512]{};
  int length = std::snprintf(body, sizeof(body),
      "[AoIP]\r\nPeerIp=%s\r\nRate=%u\r\nBits=%u\r\nBufferFrames=%u\r\nInputs=%u\r\nOutputs=%u\r\nSafetyFrames=%u\r\n",
      peer, rate, bits, block, inputs, outputs, guard);
  HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  DWORD written = 0;
  bool saved = file != INVALID_HANDLE_VALUE;
  if (saved) {
    saved = WriteFile(file, body, length, &written, nullptr) && written == static_cast<DWORD>(length);
    saved = CloseHandle(file) && saved;
  }
  if (!saved) {
    if (file != INVALID_HANDLE_VALUE) DeleteFileW(temporary);
    MessageBoxA(window, "Cannot write the Pi AoIP settings file. Check access to the profile folder.", "Pi AoIP", MB_OK | MB_ICONERROR);
    return TRUE;
  }
  unsigned max_frames = (1472 - 40) / (channels * bits / 8);
  unsigned frames = device.v2 ? std::min(max_frames,std::max(1u,std::min(block,rate/8000))) : std::min(device.frames,max_frames);
  if (!profile_fits_link(channels, rate, bits, frames,
                         device.max_pps, device.link_mbps) ||
      !profile_fits_link(wire_outputs,rate,bits,aoip::packet_frames(wire_outputs,bits),device.max_pps,device.link_mbps)) {
    MessageBoxA(window, "This format exceeds the Pi packet or link-speed limit.",
        "Pi AoIP", MB_OK | MB_ICONERROR);
    DeleteFileW(temporary);
    return TRUE;
  }
  char request[128]{}, reply[512]{};
  sockaddr_in responder{};
  std::snprintf(request, sizeof(request),
      device.v2 ? "PIAOIP_SET_PROFILE_V2 channels=%u rate=%u bits=%u frames=%u outputs=%u" :
      "PIAOIP_SET_PROFILE_V1 channels=%u rate=%u bits=%u frames=%u",
      channels, rate, bits, frames, wire_outputs);
  SetDlgItemTextA(window,kStatus,"Applying device profile...");
  const bool acknowledged=control_request(peer,nullptr,request,reply,responder,2000);
  if (acknowledged && (std::strcmp(reply,device.v2 ? "PIAOIP_PROFILE_PENDING_V2" : "PIAOIP_PROFILE_PENDING_V1") != 0 ||
      responder.sin_addr.s_addr != address.sin_addr.s_addr)) {
    DeleteFileW(temporary);
    MessageBoxA(window, "Pi did not accept the selected profile. Windows settings were not changed.",
        "Pi AoIP", MB_OK | MB_ICONERROR);
    return TRUE;
  }
  // A lost UDP acknowledgement does not mean that the device rejected the
  // change. Reconcile the active profile before committing the Windows INI.
  bool active=false;
  for(unsigned attempt=0;attempt<24;++attempt) {
    SettingsDialog::Device applied{};
    if(query_device(peer,applied,200) && applied.channels==channels && applied.rate==rate &&
      applied.bits==bits && applied.outputs==wire_outputs && applied.frames==frames) { active=true; break; }
    Sleep(100);
  }
  if(!active) {
    DeleteFileW(temporary);
    MessageBoxA(window,"Pi has not confirmed the profile. Check the link and retry.","Pi AoIP",MB_OK|MB_ICONERROR); return TRUE;
  }
  if (!MoveFileExW(temporary, state->path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temporary);
    std::snprintf(request, sizeof(request),
        device.v2 ? "PIAOIP_SET_PROFILE_V2 channels=%u rate=%u bits=%u frames=%u outputs=%u" :
        "PIAOIP_SET_PROFILE_V1 channels=%u rate=%u bits=%u frames=%u",
        device.channels, device.rate, device.bits, device.frames, device.outputs);
    control_request(peer, nullptr, request, reply, responder, 500);
    MessageBoxA(window, "Windows profile could not be saved; Pi rollback was requested.",
        "Pi AoIP", MB_OK | MB_ICONERROR);
    return TRUE;
  }
  state->changed = true;
  EndDialog(window, IDOK);
  return TRUE;
}

bool show_settings_dialog(const Config& config, void* driver) {
  SettingsDialog state{}; state.current=config; state.driver=driver;
  if (!profile_path(state.path)) {
    MessageBoxA(nullptr, "Cannot find the ASIO profile path.", "Pi AoIP", MB_OK | MB_ICONERROR);
    return false;
  }
  char host[MAX_PATH]{};
  if (GetModuleFileNameA(nullptr, host, sizeof(host)))
    state.is_live = std::strstr(host, "Ableton Live") != nullptr;
  // A real dialog font fixes default system-font sizing and keeps layout in
  // dialog units at the host window's DPI. All controls retain keyboard access.
  alignas(DWORD) uint8_t storage[256]{};
  auto* dialog=reinterpret_cast<DLGTEMPLATE*>(storage);
  dialog->style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_SETFONT;
  dialog->cx=428; dialog->cy=381;
  auto* extra=reinterpret_cast<WORD*>(storage+sizeof(DLGTEMPLATE));
  *extra++=0; *extra++=0; *extra++=0; *extra++=10;
  const wchar_t face[]=L"Segoe UI";
  std::memcpy(extra,face,sizeof(face));
  ACTCTXW context{}; context.cbSize=sizeof(context);
  context.dwFlags=ACTCTX_FLAG_HMODULE_VALID|ACTCTX_FLAG_RESOURCE_NAME_VALID;
  context.hModule=g_module; context.lpResourceName=MAKEINTRESOURCEW(2);
  HANDLE activation=CreateActCtxW(&context); ULONG_PTR cookie=0;
  if(activation!=INVALID_HANDLE_VALUE) ActivateActCtx(activation,&cookie);
  INITCOMMONCONTROLSEX common{sizeof(common),ICC_STANDARD_CLASSES}; InitCommonControlsEx(&common);
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    if(cookie) DeactivateActCtx(0,cookie);
    if(activation!=INVALID_HANDLE_VALUE) ReleaseActCtx(activation);
    return false;
  }
  DialogBoxIndirectParamA(g_module, dialog, GetActiveWindow(), settings_dialog_proc,
      reinterpret_cast<LPARAM>(&state));
  if(state.scanner.joinable()) state.scanner.join();
  WSACleanup();
  if(state.heading_font) DeleteObject(state.heading_font);
  if(state.background) DeleteObject(state.background);
  if(cookie) DeactivateActCtx(0,cookie);
  if(activation!=INVALID_HANDLE_VALUE) ReleaseActCtx(activation);
  return state.changed;
}

}
