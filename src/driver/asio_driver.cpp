#include "../config/settings.hpp"
#include "iasiodrv.h"
#include "aoip/timeline.hpp"
#include "aoip/spsc_queue.hpp"
namespace piaoip {
HMODULE g_module=nullptr;
std::atomic<long> g_objects{0};
struct ChannelBuffer {
  long channel = 0;
  bool input = true;
  std::unique_ptr<uint8_t[]> halves[2];
};

class Driver final : public IASIO {
public:
  Driver() { ++g_objects; read_config(cfg_); block_ = cfg_.block; }
  ~Driver() {
    stop();
    disposeBuffers();
    if (socket_ != INVALID_SOCKET) closesocket(socket_);
    if (socket_event_ != WSA_INVALID_EVENT) WSACloseEvent(socket_event_);
    if (stop_event_) CloseHandle(stop_event_);
    if (rx_event_) CloseHandle(rx_event_);
    if (tx_event_) CloseHandle(tx_event_);
    if (wsa_) WSACleanup();
    --g_objects;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
    if (!value) return E_POINTER;
    *value = nullptr;
    if (iid == IID_IUnknown || iid == kClsid) { *value = static_cast<IASIO*>(this); AddRef(); return S_OK; }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override { ULONG left = --refs_; if (!left) delete this; return left; }
  ASIOBool init(void*) override try {
    if (socket_ != INVALID_SOCKET) {
      stop();
      closesocket(socket_);
      socket_ = INVALID_SOCKET;
      if (wsa_) { WSACleanup(); wsa_ = false; }
      timeline_.prepare(1);
      buffers_.clear();
      callbacks_ = nullptr;
    }
    if(wsa_) { WSACleanup(); wsa_=false; }
    cfg_ = Config{};
    read_config(cfg_);
    block_ = cfg_.block;
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return ASIOFalse;
    wsa_ = true;
    // The source owns the channel count. Never use a stale Windows setting.
    SettingsDialog::Device source{};
    if (!cfg_.peer[0] || std::strcmp(cfg_.peer, "auto") == 0) {
      SettingsDialog discovery{};
      discover_devices(discovery);
      if (discovery.devices.size() != 1) {
        std::snprintf(error_, sizeof(error_), discovery.devices.empty() ?
          "No wired Pi found; open Pi AoIP settings" : "Multiple Pi devices; select one in settings");
        return ASIOFalse;
      }
      std::snprintf(cfg_.peer, sizeof(cfg_.peer), "%s", discovery.devices.front().ip);
    }
    if (!query_device(cfg_.peer, source)) {
      std::snprintf(error_, sizeof(error_), "Pi discovery failed; check wired link");
      return ASIOFalse;
    }
    cfg_.channels = static_cast<uint16_t>(source.channels);
    cfg_.inputs = std::min(cfg_.inputs, source.channels);
    cfg_.outputs = std::min(cfg_.outputs, source.outputs);
    if (!cfg_.inputs && !cfg_.outputs) { std::strcpy(error_, "Enable at least one input or output"); return ASIOFalse; }
    wire_outputs_=source.outputs; source_frames_=source.frames; source_v2_=source.v2;
    cfg_.rate = source.rate;
    cfg_.bits = static_cast<uint16_t>(source.bits);
    if (std::find(source.buffers.begin(), source.buffers.end(),
                  static_cast<unsigned>(cfg_.block)) == source.buffers.end()) {
      cfg_.block = static_cast<long>(source.buffers.front());
    }
    block_ = cfg_.block;
    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) return ASIOFalse;
    int receive_buffer = 256 * 1024;
    setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receive_buffer), sizeof(receive_buffer));
    DWORD timeout = 200;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    BOOL no_connreset = FALSE;
    DWORD returned = 0;
    WSAIoctl(socket_, 0x9800000c, &no_connreset, sizeof(no_connreset), nullptr, 0, &returned, nullptr, nullptr);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(cfg_.port);
    local.sin_addr.s_addr = INADDR_ANY;
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(cfg_.peer_port);
    if (inet_pton(AF_INET, cfg_.peer, &remote.sin_addr) != 1 ||
        bind(socket_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR ||
        connect(socket_, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == SOCKET_ERROR) {
      std::snprintf(error_, sizeof(error_), "UDP bind/connect failed: %d", WSAGetLastError());
      closesocket(socket_); socket_ = INVALID_SOCKET;
      return ASIOFalse;
    }
    if (socket_event_ == WSA_INVALID_EVENT) socket_event_=WSACreateEvent();
    if (!stop_event_) stop_event_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!rx_event_) rx_event_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if (!tx_event_) tx_event_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if (!stop_event_ || !rx_event_ || !tx_event_ || socket_event_==WSA_INVALID_EVENT ||
        WSAEventSelect(socket_,socket_event_,FD_READ | FD_CLOSE)) return ASIOFalse;
    int send_buffer=64*1024;
    setsockopt(socket_,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<const char*>(&send_buffer),sizeof(send_buffer));
    timeline_.prepare(cfg_.channels);
    input_scratch_.assign(2048*cfg_.channels,0);
    rx_queue_=std::make_unique<aoip::SpscQueue<aoip::Packet,1024>>();
    tx_queue_=std::make_unique<aoip::SpscQueue<aoip::Packet,1024>>();
    return ASIOTrue;
  } catch(const std::bad_alloc&) { std::strcpy(error_,"Audio memory allocation failed"); return ASIOFalse; }
  void getDriverName(char* name) override { if (name) std::strcpy(name, "Pi AoIP"); }
  long getDriverVersion() override { return 2; }
  void getErrorMessage(char* text) override { if (text) std::strcpy(text, error_); }
  ASIOError start() override {
    if (socket_ == INVALID_SOCKET || !callbacks_ || running_) return ASE_InvalidMode;
    char request[160]{}, reply[512]{};
    std::snprintf(request,sizeof(request),source_v2_ ?
      "PIAOIP_SUBSCRIBE_V2 port=%u channels=%u rate=%u bits=%u outputs=%u" :
      "PIAOIP_SUBSCRIBE_V1 port=%u channels=%u rate=%u bits=%u",
      cfg_.port,cfg_.channels,cfg_.rate,cfg_.bits,wire_outputs_);
    sockaddr_in responder{};
    if (!control_request(cfg_.peer,nullptr,request,reply,responder,500) ||
        std::strcmp(reply,source_v2_ ? "PIAOIP_SUBSCRIBED_V2" : "PIAOIP_SUBSCRIBED_V1")) {
      std::strcpy(error_,"Pi rejected stream subscription"); return ASE_HWMalfunction;
    }
    std::array<char,aoip::packet_bytes> stale{};
    for (unsigned i=0;i<4096;++i) if(recv(socket_,stale.data(),int(stale.size()),0)==SOCKET_ERROR) break;
    rx_queue_->reset(); tx_queue_->reset(); timeline_.reset(0); stats_.reset();
    position_=0; stream_position_=0; timestamp_=now_ns(); tx_frame_=0; tx_seq_=0; half_=0;
    tx_epoch_=uint16_t((now_ns()>>8)%65535+1);
    partial_frames_=0; pending_half_=-1;
    ResetEvent(stop_event_); ResetEvent(rx_event_); ResetEvent(tx_event_);
    running_=true;
    try {
      receiver_=std::thread([this]{receive_loop();});
      sender_=std::thread([this]{send_loop();});
      worker_=std::thread([this]{audio_loop();});
    } catch (...) { stop(); return ASE_NoMemory; }
    return ASE_OK;
  }
  ASIOError stop() override {
    if (audio_thread_id_ && audio_thread_id_==GetCurrentThreadId()) return ASE_InvalidMode;
    running_=false;
    if(stop_event_) SetEvent(stop_event_);
    if(worker_.joinable()) worker_.join();
    if(receiver_.joinable()) receiver_.join();
    if(sender_.joinable()) sender_.join();
    return ASE_OK;
  }
  ASIOError getChannels(long* inputs, long* outputs) override {
    if (!inputs || !outputs) return ASE_InvalidParameter;
    *inputs = cfg_.inputs; *outputs = cfg_.outputs; return ASE_OK;
  }
  ASIOError getLatencies(long* input, long* output) override {
    if (!input || !output) return ASE_InvalidParameter;
    *input = cfg_.inputs ? block_ + cfg_.safety + source_frames_ : 0;
    *output = cfg_.outputs ? block_ + output_packet_frames() : 0;
    return ASE_OK;
  }
  ASIOError getBufferSize(long* min, long* max, long* preferred, long* granularity) override {
    if (!min || !max || !preferred || !granularity) return ASE_InvalidParameter;
    *min = 16; *max = 2048; *preferred = cfg_.block; *granularity = -1; return ASE_OK;
  }
  ASIOError canSampleRate(ASIOSampleRate rate) override {
    // The Pi sender is the clock master. A host rate change without a matching
    // sender restart would otherwise leave the device open but silent.
    return rate == cfg_.rate ? ASE_OK : ASE_NoClock;
  }
  ASIOError getSampleRate(ASIOSampleRate* rate) override {
    if (!rate) return ASE_InvalidParameter;
    *rate = cfg_.rate; return ASE_OK;
  }
  ASIOError setSampleRate(ASIOSampleRate rate) override {
    if (running_ || canSampleRate(rate) != ASE_OK) return ASE_NoClock;
    return ASE_OK;
  }
  ASIOError getClockSources(ASIOClockSource* clocks, long* count) override {
    if (!count || *count < 1 || !clocks) return ASE_InvalidParameter;
    *count = 1;
    std::memset(clocks, 0, sizeof(*clocks));
    clocks[0].index = 0; clocks[0].associatedChannel = -1;
    clocks[0].isCurrentSource = ASIOTrue;
    std::strcpy(clocks[0].name, "Pi network PCM");
    return ASE_OK;
  }
  ASIOError setClockSource(long index) override { return index == 0 ? ASE_OK : ASE_InvalidParameter; }
  ASIOError getSamplePosition(ASIOSamples* pos, ASIOTimeStamp* stamp) override {
    if (!pos || !stamp) return ASE_InvalidParameter;
    uint64_t frame = position_.load();
    pos->lo = static_cast<unsigned long>(frame);
    pos->hi = static_cast<unsigned long>(frame >> 32);
    uint64_t ns = timestamp_.load();
    stamp->lo = static_cast<unsigned long>(ns);
    stamp->hi = static_cast<unsigned long>(ns >> 32);
    return ASE_OK;
  }
  ASIOError getChannelInfo(ASIOChannelInfo* info) override {
    if (!info || info->channel < 0 || unsigned(info->channel) >= (info->isInput ? cfg_.inputs : cfg_.outputs)) return ASE_InvalidParameter;
    info->isActive = ASIOFalse;
    for (auto& item : buffers_)
      if (item.channel == info->channel && item.input == (info->isInput != 0)) info->isActive = ASIOTrue;
    info->channelGroup = 0;
    info->type = cfg_.bits == 16 ? ASIOSTInt16LSB : cfg_.bits == 24 ? ASIOSTInt32LSB24 : ASIOSTInt32LSB;
    std::snprintf(info->name, sizeof(info->name), "Pi %s %ld", info->isInput ? "input" : "output", info->channel + 1);
    return ASE_OK;
  }
  ASIOError createBuffers(ASIOBufferInfo* info, long count, long frames, ASIOCallbacks* callbacks) override try {
    if (running_ || !info || !callbacks || count < 1 || unsigned(count) > cfg_.inputs + cfg_.outputs ||
        frames < 16 || frames > 2048 || (frames & (frames - 1)) || !buffers_.empty() || !callbacks->bufferSwitch) return ASE_InvalidParameter;
    for (long i = 0; i < count; ++i) {
      if (info[i].channelNum < 0 || unsigned(info[i].channelNum) >= (info[i].isInput ? cfg_.inputs : cfg_.outputs)) return ASE_InvalidParameter;
      for (long j = 0; j < i; ++j)
        if (info[j].channelNum == info[i].channelNum && info[j].isInput == info[i].isInput)
          return ASE_InvalidParameter;
    }
    block_ = frames;
    callbacks_ = callbacks;
    time_info_ = callbacks->bufferSwitchTimeInfo && callbacks->asioMessage &&
      callbacks->asioMessage(kAsioSupportsTimeInfo,0,nullptr,nullptr)==1;
    int asio_bytes = cfg_.bits == 16 ? 2 : 4;
    for (long i = 0; i < count; ++i) {
      ChannelBuffer item;
      item.channel = info[i].channelNum;
      item.input = info[i].isInput != 0;
      for (int half = 0; half < 2; ++half) {
        item.halves[half] = std::make_unique<uint8_t[]>(size_t(frames) * asio_bytes);
        std::memset(item.halves[half].get(), 0, size_t(frames) * asio_bytes);
      }
      buffers_.push_back(std::move(item));
      info[i].buffers[0] = buffers_.back().halves[0].get();
      info[i].buffers[1] = buffers_.back().halves[1].get();
    }
    return ASE_OK;
  } catch(const std::bad_alloc&) {
    buffers_.clear(); callbacks_=nullptr;
    for(long i=0;i<count;++i) info[i].buffers[0]=info[i].buffers[1]=nullptr;
    return ASE_NoMemory;
  }
  ASIOError disposeBuffers() override {
    stop();
    buffers_.clear(); callbacks_ = nullptr; return ASE_OK;
  }
  ASIOError controlPanel() override {
    if (show_settings_dialog(cfg_, this) && callbacks_ && callbacks_->asioMessage) {
      auto reset = callbacks_->asioMessage;
      AddRef();
      long accepted = reset(kAsioResetRequest, 0, nullptr, nullptr);
      Release();
      if (accepted != 1) {
        MessageBoxA(nullptr, "The ASIO host did not accept automatic device reset.",
            "Pi AoIP", MB_OK | MB_ICONWARNING);
      }
    }
    return ASE_OK;
  }
  ASIOError future(long selector, void* data) override {
    if (selector==aoip::diagnostics_selector && data) {
      auto* out=static_cast<aoip::Diagnostics*>(data);
      if(out->size!=sizeof(*out) || out->version!=1) return ASE_InvalidParameter;
      stats_.snapshot(*out); return ASE_SUCCESS;
    }
    if(selector==kAsioCanTimeInfo) return ASE_SUCCESS;
    return ASE_NotPresent;
  }
  ASIOError outputReady() override { output_ready_.store(true,std::memory_order_release); return ASE_OK; }

private:
#include "engine.inc"
};

class Factory final : public IClassFactory {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid == IID_IUnknown || iid == IID_IClassFactory) { *out = this; AddRef(); return S_OK; }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override { auto left = --refs_; if (!left) delete this; return left; }
  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** out) override {
    if (outer) return CLASS_E_NOAGGREGATION;
    auto* driver = new Driver();
    HRESULT result = driver->QueryInterface(iid, out);
    driver->Release();
    return result;
  }
  HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override { g_objects += lock ? 1 : -1; return S_OK; }
private:
  std::atomic<ULONG> refs_{1};
};
}

using namespace piaoip;
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) { g_module = instance; DisableThreadLibraryCalls(instance); }
  return TRUE;
}
extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID iid, void** out) {
  if (clsid != kClsid) return CLASS_E_CLASSNOTAVAILABLE;
  auto* factory = new Factory();
  HRESULT result = factory->QueryInterface(iid, out);
  factory->Release();
  return result;
}
extern "C" HRESULT __stdcall DllCanUnloadNow() {
  return g_objects == 0 ? S_OK : S_FALSE;
}
