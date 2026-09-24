#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include "iasiodrv.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include "aoip/diagnostics.hpp"

namespace {
constexpr CLSID kClsid = {0xa24d50b2, 0x9111, 0x4a6b, {0x9c, 0x29, 0xa0, 0x1d, 0x61, 0x7b, 0xc8, 0x30}};
std::atomic<long> g_callbacks{0}, g_nonzero{0};
std::atomic<long> g_last_nonzero_callback{0}, g_time_errors{0};
std::atomic<long> g_reset_requests{0};
std::atomic<bool> g_panel_rtt_ok{false};
ASIOBufferInfo* g_buffers = nullptr;
long g_channels = 0, g_outputs = 0, g_bits = 0, g_frames = 0;
bool g_echo = false;
bool g_no_ready=false, g_time_info=false;
IASIO* g_driver = nullptr;
unsigned g_stall_ms = 0;
HWND own_panel() {
  HWND found=nullptr;
  EnumWindows([](HWND window,LPARAM parameter)->BOOL {
    DWORD process=0; GetWindowThreadProcessId(window,&process);
    if(process!=GetCurrentProcessId()) return TRUE;
    char title[128]{}; GetWindowTextA(window,title,sizeof(title));
    if(std::strcmp(title,"Pi AoIP configuration")) return TRUE;
    *reinterpret_cast<HWND*>(parameter)=window; return FALSE;
  },reinterpret_cast<LPARAM>(&found));
  return found;
}
bool save_panel(HWND window) {
  RECT r{}; GetWindowRect(window,&r);
  const int width=r.right-r.left,height=r.bottom-r.top;
  HDC screen=GetDC(window),memory=CreateCompatibleDC(screen);
  HBITMAP bitmap=CreateCompatibleBitmap(screen,width,height);
  HGDIOBJ old=SelectObject(memory,bitmap);
  bool ok=PrintWindow(window,memory,2)!=FALSE;
  BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth=width; info.bmiHeader.biHeight=-height;
  info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
  std::vector<unsigned char> pixels(size_t(width)*height*4);
  SelectObject(memory,old);
  ok=GetDIBits(memory,bitmap,0,height,pixels.data(),&info,DIB_RGB_COLORS)!=0 && ok;
  BITMAPFILEHEADER file{}; file.bfType=0x4d42; file.bfOffBits=sizeof(file)+sizeof(info.bmiHeader);
  file.bfSize=file.bfOffBits+DWORD(pixels.size());
  FILE* output=std::fopen("aoip/build/panel.bmp","wb");
  if(output) { std::fwrite(&file,sizeof(file),1,output); std::fwrite(&info.bmiHeader,sizeof(info.bmiHeader),1,output);
    std::fwrite(pixels.data(),pixels.size(),1,output); std::fclose(output); } else ok=false;
  DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(window,screen);
  return ok;
}
struct OwnedWindow { HWND owner, found; };
BOOL CALLBACK find_owned_window(HWND window, LPARAM parameter) {
  auto* target = reinterpret_cast<OwnedWindow*>(parameter);
  char cls[32]{};
  GetClassNameA(window, cls, sizeof(cls));
  if (GetWindow(window, GW_OWNER) == target->owner && std::strcmp(cls, "#32770") == 0) {
    target->found = window;
    return FALSE;
  }
  return TRUE;
}
void buffer_switch(long half, ASIOBool) {
  ++g_callbacks;
  if (g_stall_ms && g_callbacks == 1000) Sleep(g_stall_ms);
  bool audible=false;
  for (long ch = 0; ch < g_channels; ++ch) {
    auto* bytes = static_cast<unsigned char*>(g_buffers[ch].buffers[half]);
    int n = g_bits == 16 ? 2 : 4;
    for (int i = 0; i < n; ++i) if (bytes[i]) { ++g_nonzero; audible=true; break; }
  }
  for (long ch = 0; ch < g_outputs; ++ch) {
    if (g_echo && ch < g_channels) {
      const void* input = g_buffers[ch].buffers[half];
      void* output = g_buffers[g_channels + ch].buffers[half];
      std::memcpy(output, input, static_cast<size_t>(g_frames) * (g_bits == 16 ? 2 : 4));
      continue;
    }
    if (g_bits == 16) {
      auto* output = static_cast<int16_t*>(g_buffers[g_channels + ch].buffers[half]);
      for (long f = 0; f < g_frames; ++f) output[f] = static_cast<int16_t>((ch + 1) * 1000);
    } else {
      auto* output = static_cast<int32_t*>(g_buffers[g_channels + ch].buffers[half]);
      for (long f = 0; f < g_frames; ++f) output[f] = static_cast<int32_t>((ch + 1) * 100000);
    }
  }
  if(audible) g_last_nonzero_callback=g_callbacks.load();
  if (g_driver && !g_no_ready) g_driver->outputReady();
}
void rate_changed(ASIOSampleRate) {}
long asio_message(long selector, long, void*, double*) {
  if (selector == kAsioResetRequest) { ++g_reset_requests; return 1; }
  if (selector == kAsioSupportsTimeInfo) return g_time_info ? 1 : 0;
  return 0;
}
ASIOTime* buffer_switch_time(ASIOTime* time, long half, ASIOBool direct) {
  static uint64_t previous_position=0,previous_time=0;
  const auto& t=time->timeInfo;
  uint64_t position=(uint64_t(t.samplePosition.hi)<<32)|t.samplePosition.lo;
  uint64_t stamp=(uint64_t(t.systemTime.hi)<<32)|t.systemTime.lo;
  if(!(t.flags&kSamplePositionValid) || !(t.flags&kSystemTimeValid) ||
     (g_callbacks && (position<=previous_position || stamp<=previous_time))) ++g_time_errors;
  previous_position=position; previous_time=stamp;
  buffer_switch(half,direct); return time;
}
}

int main(int argc, char** argv) {
  if (argc < 3 || argc > 5) { std::fprintf(stderr, "Usage: smoke_host DLL|registered SECONDS|echoSECONDS|panel [BUFFER] [STALL_MS]\n"); return 2; }
  if (argc > 4) g_stall_ms=unsigned(std::atoi(argv[4]));
  g_no_ready=std::strstr(argv[2],"deferred")!=nullptr;
  g_time_info=std::strstr(argv[2],"time")!=nullptr;
  g_echo = std::strncmp(argv[2], "echo", 4) == 0 ||
           std::strcmp(argv[2], "panelrtt") == 0;
  HMODULE dll = nullptr;
  IASIO* driver = nullptr;
  HRESULT result = S_OK;
  if (!std::strcmp(argv[1], "registered")) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    result = CoCreateInstance(kClsid, nullptr, CLSCTX_INPROC_SERVER, kClsid, reinterpret_cast<void**>(&driver));
  } else {
    dll = LoadLibraryA(argv[1]);
    if (!dll) { std::fprintf(stderr, "LoadLibrary failed %lu\n", GetLastError()); return 1; }
    using GetClass = HRESULT (__stdcall*)(REFCLSID, REFIID, void**);
    auto get_class = reinterpret_cast<GetClass>(GetProcAddress(dll, "DllGetClassObject"));
    if (!get_class) { std::fprintf(stderr, "DllGetClassObject missing\n"); return 1; }
    IClassFactory* factory = nullptr;
    result = get_class(kClsid, IID_IClassFactory, reinterpret_cast<void**>(&factory));
    if (FAILED(result)) { std::fprintf(stderr, "factory error %08lx\n", result); return 1; }
    result = factory->CreateInstance(nullptr, kClsid, reinterpret_cast<void**>(&driver));
    factory->Release();
  }
  if (FAILED(result)) { std::fprintf(stderr, "driver error %08lx\n", result); return 1; }
  std::fprintf(stderr, "driver created\n");
  if (!driver->init(nullptr)) { char msg[128]{}; driver->getErrorMessage(msg); std::fprintf(stderr, "init: %s\n", msg); driver->Release(); return 1; }
  std::fprintf(stderr, "driver initialized\n");
  if (!std::strcmp(argv[2], "panel") || !std::strcmp(argv[2], "paneltest") || !std::strcmp(argv[2], "panelpreview") ||
      !std::strcmp(argv[2], "paneltest-save") || !std::strcmp(argv[2], "paneltest-restore")) {
    std::fprintf(stderr, "opening panel\n");
    std::thread observer;
    bool restore = !std::strcmp(argv[2], "paneltest-restore");
    bool save = restore || !std::strcmp(argv[2], "paneltest-save");
    bool preview=!std::strcmp(argv[2],"panelpreview");
    if (save || preview || !std::strcmp(argv[2], "paneltest")) observer = std::thread([save, restore, preview] {
      for (int i = 0; i < 100; ++i) {
        HWND window = own_panel();
        if (window) {
          Sleep(1200);
          char status[256]{};
          GetDlgItemTextA(window, 3103, status, sizeof(status));
          std::fprintf(stderr, "PANEL found status=%s\n", status);
          char channel_class[32]{};
          GetClassNameA(GetDlgItem(window, 3001), channel_class, sizeof(channel_class));
          const LRESULT rate192 = SendDlgItemMessageA(window, 3002, CB_FINDSTRINGEXACT,
              static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>("192000"));
          const LRESULT buffer_count = SendDlgItemMessageA(window, 3004, CB_GETCOUNT, 0, 0);
          std::fprintf(stderr, "PANEL channels_control=%s output_control=%s rate192=%s buffer_options=%ld\n",
              channel_class, GetDlgItem(window, 3005) ? "present" : "absent",
              rate192 == CB_ERR ? "absent" : "present", static_cast<long>(buffer_count));
          if(preview) std::fprintf(stderr,"PANEL screenshot=%s\n",save_panel(window) ? "aoip/build/panel.bmp" : "FAILED");
          if (save) {
            auto select = [&](int id, const char* value) {
              LRESULT index = SendDlgItemMessageA(window, id, CB_FINDSTRINGEXACT,
                  static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(value));
              if (index != CB_ERR) SendDlgItemMessageA(window, id, CB_SETCURSEL, index, 0);
              std::fprintf(stderr, "PANEL select %d=%s index=%ld\n", id, value, static_cast<long>(index));
            };
            select(3002, restore ? "192000" : "96000");
            select(3003, restore ? "32" : "24");
            select(3004, restore ? "128" : "64");
            PostMessageA(window, WM_COMMAND, IDOK, 0);
            for (int j = 0; j < 50; ++j) {
              OwnedWindow owned{window, nullptr};
              EnumWindows(find_owned_window, reinterpret_cast<LPARAM>(&owned));
              if (owned.found) {
                char title[128]{};
                GetWindowTextA(owned.found, title, sizeof(title));
                std::fprintf(stderr, "PANEL message=%s\n", title);
                HWND okay = GetDlgItem(owned.found, IDOK);
                if (okay) SendMessageA(okay, BM_CLICK, 0, 0);
                break;
              }
              Sleep(100);
            }
            Sleep(200);
            PostMessageA(window, WM_COMMAND, IDCANCEL, 0);
          } else SendMessageA(window, WM_COMMAND, IDCANCEL, 0);
          return;
        }
        Sleep(100);
      }
      std::fprintf(stderr, "PANEL not found after 10 s\n");
    });
    ASIOError status = driver->controlPanel();
    if (observer.joinable()) observer.join();
    std::printf("PANEL result=%ld\n", static_cast<long>(status));
    driver->Release();
    if (dll) FreeLibrary(dll);
    else CoUninitialize();
    return status == ASE_OK ? 0 : 1;
  }
  driver->getChannels(&g_channels, &g_outputs);
  long min = 0, max = 0, preferred = 0, step = 0;
  driver->getBufferSize(&min, &max, &preferred, &step);
  if (argc > 3) preferred=std::atoi(argv[3]);
  ASIOSampleRate rate = 0;
  driver->getSampleRate(&rate);
  ASIOChannelInfo ci{};
  ci.isInput = g_channels ? ASIOTrue : ASIOFalse;
  ci.channel = 0;
  driver->getChannelInfo(&ci);
  g_bits = ci.type == ASIOSTInt16LSB ? 16 : ci.type == ASIOSTInt32LSB24 ? 24 : 32;
  g_frames = preferred;
  std::vector<ASIOBufferInfo> buffers(g_channels + g_outputs);
  for (long i = 0; i < g_channels; ++i) { buffers[i].isInput = ASIOTrue; buffers[i].channelNum = i; }
  for (long i = 0; i < g_outputs; ++i) { buffers[g_channels + i].isInput = ASIOFalse; buffers[g_channels + i].channelNum = i; }
  ASIOCallbacks callbacks{buffer_switch, rate_changed, asio_message, buffer_switch_time};
  if (driver->createBuffers(buffers.data(), static_cast<long>(buffers.size()), preferred, &callbacks) != ASE_OK) {
    std::fprintf(stderr, "createBuffers failed\n"); driver->Release(); return 1;
  }
  g_buffers = buffers.data();
  g_driver = driver;
  std::printf("ASIO channels=%ld outputs=%ld rate=%.0f bits=%ld buffer=%ld\n", g_channels, g_outputs, rate, g_bits, preferred);
  if (driver->start() != ASE_OK) { std::fprintf(stderr, "start failed\n"); driver->Release(); return 1; }
  if (!std::strcmp(argv[2], "panelrtt")) {
    std::thread observer([] {
      for (int attempt = 0; attempt < 100; ++attempt) {
        HWND window = own_panel();
        if (window) {
          HWND measure = GetDlgItem(window, 3104);
          if (measure) SendMessageA(measure, BM_CLICK, 0, 0);
          Sleep(4000);
          char label[256]{};
          GetDlgItemTextA(window, 3105, label, sizeof(label));
          std::fprintf(stderr, "PANEL measured=%s\n", label);
          g_panel_rtt_ok = std::strstr(label, "median") != nullptr;
          if (measure) SendMessageA(measure, BM_CLICK, 0, 0);
          SendMessageA(window, WM_COMMAND, IDCANCEL, 0);
          return;
        }
        Sleep(100);
      }
    });
    ASIOError panel = driver->controlPanel();
    observer.join();
    driver->stop(); driver->disposeBuffers(); driver->Release();
    if (dll) FreeLibrary(dll); else CoUninitialize();
    return panel == ASE_OK && g_panel_rtt_ok ? 0 : 1;
  }
  if (!std::strcmp(argv[2], "resettest")) {
    std::thread observer([] {
      for (int i = 0; i < 100; ++i) {
        HWND window = own_panel();
        if (window) {
          for (int j = 0; j < 30; ++j) {
            char status[256]{};
            GetDlgItemTextA(window, 3103, status, sizeof(status));
            if (std::strstr(status, "active:")) break;
            Sleep(100);
          }
          LRESULT index = SendDlgItemMessageA(window, 3004, CB_FINDSTRINGEXACT,
              static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>("128"));
          if (index != CB_ERR) SendDlgItemMessageA(window, 3004, CB_SETCURSEL, index, 0);
          PostMessageA(window, WM_COMMAND, IDOK, 0);
          return;
        }
        Sleep(100);
      }
    });
    ASIOError panel = driver->controlPanel();
    observer.join();
    driver->stop();
    driver->disposeBuffers();
    if (!driver->init(nullptr)) { std::fprintf(stderr, "reinit failed\n"); driver->Release(); return 1; }
    long after = 0;
    driver->getBufferSize(&min, &max, &after, &step);
    std::printf("RESET panel=%ld requests=%ld buffer_before=%ld buffer_after=%ld\n",
        static_cast<long>(panel), g_reset_requests.load(), preferred, after);
    driver->Release();
    if (dll) FreeLibrary(dll);
    else CoUninitialize();
    return panel == ASE_OK && g_reset_requests == 1 && after == 128 ? 0 : 1;
  }
  Sleep(static_cast<DWORD>(std::atoi(argv[2] + (g_echo ? 4 : 0))) * 1000);
  driver->stop();
  aoip::Diagnostics diagnostics;
  if (driver->future(aoip::diagnostics_selector, &diagnostics) == ASE_SUCCESS) {
    std::printf("DIAG rx=%llu invalid=%llu rx_overflow=%llu late_frames=%llu missing_frames=%llu resync=%llu "
      "deadline_misses=%llu skipped_frames=%llu callback_max_us=%.1f wake_max_us=%.1f rx_gap_max_us=%.1f "
      "tx=%llu tx_overflow=%llu tx_expired=%llu tx_errors=%llu mmcss_failures=%llu last_tx_error=%llu host_overruns=%llu expired_output_frames=%llu tx_retries=%llu\n",
      diagnostics.rx_packets,diagnostics.invalid_packets,diagnostics.rx_queue_overflows,diagnostics.late_frames,
      diagnostics.missing_frames,diagnostics.resyncs,diagnostics.deadline_misses,diagnostics.skipped_frames,
      diagnostics.max_callback_ns/1000.,diagnostics.max_wake_late_ns/1000.,diagnostics.max_rx_gap_ns/1000.,
      diagnostics.tx_packets,diagnostics.tx_queue_overflows,diagnostics.tx_expired_packets,diagnostics.tx_errors,
      diagnostics.mmcss_failures,diagnostics.last_tx_error,diagnostics.host_overruns,diagnostics.expired_output_frames,diagnostics.tx_retries);
  }
  std::printf("FINAL callbacks=%ld nonzero_channel_blocks=%ld frames=%ld last_nonzero_callback=%ld time_errors=%ld\n",
              g_callbacks.load(), g_nonzero.load(), g_callbacks.load() * preferred,g_last_nonzero_callback.load(),g_time_errors.load());
  driver->disposeBuffers();
  driver->Release();
  if (dll) FreeLibrary(dll);
  else CoUninitialize();
  return g_callbacks > 0 && !g_time_errors && (g_nonzero > 0 || (!g_channels && g_outputs)) ? 0 : 1;
}
