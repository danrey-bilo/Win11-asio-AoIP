#pragma once
#include <windows.h>
#include <avrt.h>
#include <atomic>
#include <cstdint>
namespace piaoip {
uint64_t now_ns();
void max_counter(std::atomic<uint64_t>& value, uint64_t candidate);
class RealtimeThread {
  HANDLE mmcss_=nullptr;
public:
  explicit RealtimeThread(std::atomic<uint64_t>& failures);
  ~RealtimeThread();
};
class DeadlineWaiter {
  HANDLE timer_=nullptr;
public:
  DeadlineWaiter();
  ~DeadlineWaiter();
  void wait(uint64_t deadline, HANDLE stop_event, HANDLE wake_event=nullptr);
};
}
