#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include "iasiodrv.h"

namespace {
constexpr CLSID kClsid = {0xa24d50b2, 0x9111, 0x4a6b,
    {0x9c, 0x29, 0xa0, 0x1d, 0x61, 0x7b, 0xc8, 0x30}};
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, char*, int) {
  HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(initialized)) {
    MessageBoxA(nullptr, "COM initialization failed.", "Pi AoIP", MB_OK | MB_ICONERROR);
    return 1;
  }
  IASIO* driver = nullptr;
  HRESULT result = CoCreateInstance(kClsid, nullptr, CLSCTX_INPROC_SERVER,
      kClsid, reinterpret_cast<void**>(&driver));
  if (FAILED(result)) {
    MessageBoxA(nullptr, "Pi AoIP ASIO is not installed. Install or repair the PiAoIP MSI package.",
        "Pi AoIP", MB_OK | MB_ICONERROR);
    CoUninitialize();
    return 1;
  }
  ASIOError panel = driver->controlPanel();
  driver->Release();
  CoUninitialize();
  return panel == ASE_OK ? 0 : 1;
}
