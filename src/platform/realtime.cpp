#include "realtime.hpp"
namespace piaoip {
uint64_t now_ns() {
  static const uint64_t hz=[] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return uint64_t(f.QuadPart); }();
  LARGE_INTEGER t; QueryPerformanceCounter(&t); auto v=uint64_t(t.QuadPart);
  return v/hz*1000000000ull+v%hz*1000000000ull/hz;
}
void max_counter(std::atomic<uint64_t>& v,uint64_t x) {
  auto old=v.load(std::memory_order_relaxed);
  while(x>old && !v.compare_exchange_weak(old,x,std::memory_order_relaxed)) {}
}
RealtimeThread::RealtimeThread(std::atomic<uint64_t>& failures) {
  DWORD index=0; mmcss_=AvSetMmThreadCharacteristicsW(L"Pro Audio",&index);
  if(!mmcss_ || !AvSetMmThreadPriority(mmcss_,AVRT_PRIORITY_CRITICAL)) ++failures;
  if(!mmcss_) SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_HIGHEST);
}
RealtimeThread::~RealtimeThread() { if(mmcss_) AvRevertMmThreadCharacteristics(mmcss_); }
DeadlineWaiter::DeadlineWaiter() {
  timer_=CreateWaitableTimerExW(nullptr,nullptr,0x00000002,TIMER_ALL_ACCESS);
  if(!timer_) timer_=CreateWaitableTimerW(nullptr,FALSE,nullptr);
}
DeadlineWaiter::~DeadlineWaiter() { if(timer_) CloseHandle(timer_); }
void DeadlineWaiter::wait(uint64_t deadline,HANDLE stop,HANDLE wake) {
  // Sleep most of a long period; a bounded 40 us spin avoids rounding tiny
  // 16-frame periods to the Windows scheduler's millisecond-scale waits.
  auto now=now_ns();
  if(timer_ && deadline>now+100000) {
    LARGE_INTEGER due; due.QuadPart=-static_cast<LONGLONG>((deadline-now-40000)/100);
    if(SetWaitableTimer(timer_,&due,0,nullptr,nullptr,FALSE)) {
      HANDLE handles[]={stop,timer_,wake};
      auto result=WaitForMultipleObjects(wake ? 3 : 2,handles,FALSE,100);
      if(result==WAIT_OBJECT_0 || result==WAIT_OBJECT_0+2) return;
    }
  }
  while(now_ns()<deadline) { if(WaitForSingleObject(stop,0)==WAIT_OBJECT_0) return; YieldProcessor(); }
}
}
