  // RX, ASIO processing and TX each own their queues/cursors. No socket call
  // or unbounded historical catch-up runs inside a host callback.
  struct Counters {
#define AOIP_COUNTERS(X) \
    X(rx_packets) X(invalid_packets) X(rx_queue_overflows) X(late_frames) \
    X(missing_frames) X(resyncs) X(callbacks) X(deadline_misses) X(skipped_frames) \
    X(max_callback_ns) X(max_wake_late_ns) X(max_rx_gap_ns) X(tx_packets) \
    X(tx_queue_overflows) X(tx_expired_packets) X(tx_errors) X(mmcss_failures) X(last_tx_error) X(host_overruns) X(expired_output_frames) X(tx_retries)
#define DECLARE(name) std::atomic<uint64_t> name{0};
    AOIP_COUNTERS(DECLARE)
#undef DECLARE
    void reset() {
#define RESET(name) name=0;
      AOIP_COUNTERS(RESET)
#undef RESET
    }
    void snapshot(aoip::Diagnostics& out) const {
#define COPY(name) out.name=name.load(std::memory_order_relaxed);
      AOIP_COUNTERS(COPY)
#undef COPY
    }
#undef AOIP_COUNTERS
  } stats_;

  void receive_loop() {
    RealtimeThread realtime(stats_.mmcss_failures);
    aoip::Packet packet;
    uint64_t previous=0;
    HANDLE events[]={stop_event_,socket_event_};
    while(running_.load(std::memory_order_relaxed)) {
      bool received=false;
      for(unsigned batch=0; batch<256 && running_; ++batch) {
        int n=recv(socket_,reinterpret_cast<char*>(packet.data),sizeof(packet.data),0);
        if(n==SOCKET_ERROR) {
          int error=WSAGetLastError();
          if(error!=WSAEWOULDBLOCK && error!=WSAECONNRESET) ++stats_.invalid_packets;
          break;
        }
        packet.time_ns=now_ns(); packet.size=unsigned(n);
        if(!aoip::validate_packet(packet.data,packet.size,1,cfg_.channels,cfg_.rate,cfg_.bits)) {
          ++stats_.invalid_packets; continue;
        }
        if(previous) max_counter(stats_.max_rx_gap_ns,packet.time_ns-previous);
        previous=packet.time_ns;
        ++stats_.rx_packets;
        if(!rx_queue_->push(packet)) ++stats_.rx_queue_overflows;
        received=true;
      }
      if(received) { SetEvent(rx_event_); continue; }
      if(WaitForMultipleObjects(2,events,FALSE,100)==WAIT_OBJECT_0) break;
      WSANETWORKEVENTS network{};
      WSAEnumNetworkEvents(socket_,socket_event_,&network);
    }
  }

  void send_loop() {
    RealtimeThread realtime(stats_.mmcss_failures);
    DeadlineWaiter waiter;
    aoip::Packet packet;
    HANDLE events[]={stop_event_,tx_event_};
    const uint64_t expiry=std::max<uint64_t>(500000,uint64_t(block_+cfg_.safety)*1000000000/cfg_.rate);
    while(running_.load(std::memory_order_relaxed)) {
      if(!tx_queue_->pop(packet)) { WaitForMultipleObjects(2,events,FALSE,100); continue; }
      if(now_ns()>packet.time_ns+expiry) { ++stats_.tx_expired_packets; continue; }
      waiter.wait(packet.time_ns,stop_event_);
      if(!running_) break;
      if(now_ns()>packet.time_ns+expiry) { ++stats_.tx_expired_packets; continue; }
      while(running_) {
        if(now_ns()>packet.time_ns+expiry) { ++stats_.tx_expired_packets; break; }
        int n=send(socket_,reinterpret_cast<const char*>(packet.data),int(packet.size),0);
        if(n==int(packet.size)) { ++stats_.tx_packets; break; }
        const int error=WSAGetLastError();
        if(n==SOCKET_ERROR && error==WSAEWOULDBLOCK) {
          // Transient backpressure is retried only until the original deadline.
          // The audio thread remains independent and the retry is observable.
          ++stats_.tx_retries;
          waiter.wait(now_ns()+25000,stop_event_);
          continue;
        }
        ++stats_.tx_errors; stats_.last_tx_error=error; break;
      }
    }
  }

  unsigned output_packet_frames() const {
    // Small channel counts must not wait milliseconds to fill the MTU.
    // Aim for <=125 us of audio, also bounded by one host block and the MTU.
    return std::min(aoip::packet_frames(wire_outputs_,cfg_.bits),
      std::min(unsigned(block_),std::max(1u,cfg_.rate/8000)));
  }

  void enqueue_output(int half,uint64_t now) {
    if(!cfg_.outputs) return;
    const unsigned stride=wire_outputs_*(cfg_.bits/8);
    const unsigned packet_frames=output_packet_frames();
    const unsigned asio_bytes=cfg_.bits==16 ? 2 : 4;
    for(long f=0; f<block_; ++f) {
      if(!partial_frames_) {
        partial_.time_ns=now+uint64_t(f)*1000000000/cfg_.rate;
        aoip::write_header(partial_.data,2,tx_seq_++,tx_frame_,wire_outputs_,cfg_.rate,cfg_.bits,packet_frames);
        if(source_v2_) { aoip::put16(partial_.data+4,2); aoip::put16(partial_.data+34,tx_epoch_); }
      }
      auto* target=partial_.data+aoip::header_bytes+size_t(partial_frames_)*stride;
      std::memset(target,0,stride);
      for(auto& channel:buffers_) if(!channel.input)
        std::memcpy(target+channel.channel*(cfg_.bits/8),channel.halves[half].get()+size_t(f)*asio_bytes,cfg_.bits/8);
      ++tx_frame_;
      if(++partial_frames_==packet_frames) {
        partial_.size=aoip::header_bytes+packet_frames*stride;
        aoip::put32(partial_.data+36,aoip::packet_crc(partial_.data,partial_.size));
        if(!tx_queue_->push(partial_)) ++stats_.tx_queue_overflows;
        else SetEvent(tx_event_);
        partial_frames_=0;
      }
    }
  }

  uint64_t allowed_lateness() const {
    // Recover short scheduling stalls using the explicitly selected guard.
    // Never replay an unbounded backlog when the host actually hangs.
    return std::max<uint64_t>(500000,
      uint64_t(std::max(2u*unsigned(block_),cfg_.safety))*1000000000/cfg_.rate);
  }

  void deliver(uint64_t scheduled) {
    // If the host has no outputReady handshake, use the normal ASIO one-cycle
    // output pipeline. Never send a half immediately after requesting deferred work.
    if(pending_half_>=0) { enqueue_output(pending_half_,now_ns()); pending_half_=-1; }
    unsigned missing=timeline_.read(input_scratch_.data(),unsigned(block_));
    stats_.missing_frames+=missing;
    for(auto& channel:buffers_) {
      if(!channel.input) continue;
      for(long f=0; f<block_; ++f) {
        int32_t value=input_scratch_[size_t(f)*cfg_.channels+channel.channel];
        if(cfg_.bits==16) reinterpret_cast<int16_t*>(channel.halves[half_].get())[f]=int16_t(value);
        else reinterpret_cast<int32_t*>(channel.halves[half_].get())[f]=value;
      }
    }
    timestamp_=scheduled;
    position_=stream_position_;
    output_ready_=false;
    const uint64_t before=now_ns();
    if(time_info_) {
      ASIOTime time{}; const auto pos=position_.load();
      time.timeInfo.samplePosition.lo=uint32_t(pos); time.timeInfo.samplePosition.hi=uint32_t(pos>>32);
      time.timeInfo.systemTime.lo=uint32_t(scheduled); time.timeInfo.systemTime.hi=uint32_t(scheduled>>32);
      time.timeInfo.sampleRate=cfg_.rate; time.timeInfo.speed=1.0;
      time.timeInfo.flags=kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
      callbacks_->bufferSwitchTimeInfo(&time,half_,ASIOTrue);
    } else callbacks_->bufferSwitch(half_,ASIOTrue);
    const auto finished=now_ns();
    max_counter(stats_.max_callback_ns,finished-before);
    const auto period=uint64_t(block_)*1000000000/cfg_.rate;
    if(finished-before>period) ++stats_.host_overruns;
    ++stats_.callbacks;
    if(finished>scheduled+allowed_lateness()) {
      stats_.expired_output_frames+=block_; tx_frame_+=block_;
      partial_frames_=0; pending_half_=-1;
    } else if(output_ready_.load(std::memory_order_acquire)) enqueue_output(half_,finished);
    else pending_half_=half_;
    stream_position_+=block_;
    half_^=1;
  }

  void audio_loop() {
    audio_thread_id_=GetCurrentThreadId();
    RealtimeThread realtime(stats_.mmcss_failures);
    DeadlineWaiter waiter;
    aoip::Packet packet;
    const double nominal=double(block_)*1000000000.0/cfg_.rate;
    double deadline=double(now_ns())+nominal;
    uint64_t last_packet_ns=0;
    bool locked=false;
    uint16_t receive_epoch=0;
    while(running_.load(std::memory_order_relaxed)) {
      // Bound work even if a faulty sender floods the socket. A queue holds
      // <=1024 packets; old frames are discarded using the source timeline.
      for(unsigned batch=0; batch<1024 && rx_queue_->pop(packet); ++batch) {
        const auto first=aoip::get64(packet.data+16);
        const bool v2=aoip::get16(packet.data+4)==2;
        const uint16_t epoch=aoip::get16(packet.data+34);
        const bool restart=locked && (v2 ? epoch!=receive_epoch :
          (first<timeline_.cursor && timeline_.cursor-first>cfg_.rate/4));
        const bool reacquire=locked && last_packet_ns && packet.time_ns-last_packet_ns>250000000;
        const bool far_ahead=locked && first>timeline_.cursor && first-timeline_.cursor>cfg_.rate/4;
        if(!locked || restart || reacquire || far_ahead) {
          if(locked) ++stats_.resyncs;
          timeline_.reset(first); locked=true;
          receive_epoch=epoch;
          // One block plus an explicit, small guard; host buffering is independent
          // of the MTU-sized wire packets.
          deadline=double(packet.time_ns)+double(block_+cfg_.safety)*1000000000.0/cfg_.rate;
        }
        last_packet_ns=packet.time_ns;
        stats_.late_frames+=timeline_.insert(packet.data);
      }
      if(!locked) {
        HANDLE events[]={stop_event_,rx_event_};
        WaitForMultipleObjects(2,events,FALSE,20);
        continue;
      }
      const auto now=now_ns();
      if(double(now)<deadline) { waiter.wait(uint64_t(deadline),stop_event_,rx_event_); continue; }
      const uint64_t late=now-uint64_t(deadline);
      max_counter(stats_.max_wake_late_ns,late);
      if(late>=allowed_lateness()) {
        const uint64_t skipped=uint64_t(double(late)/nominal);
        ++stats_.deadline_misses;
        stats_.skipped_frames+=skipped*block_;
        timeline_.cursor+=skipped*block_;
        stream_position_+=skipped*block_;
        tx_frame_+=skipped*block_;
        partial_frames_=0;
        pending_half_=-1; // expired host audio must not be returned after recovery
        deadline+=double(skipped)*nominal;
      }
      const int64_t queued=int64_t(timeline_.latest)-int64_t(timeline_.cursor);
      const int64_t target=block_+cfg_.safety;
      if(queued>target+std::max<int64_t>({2*block_,cfg_.rate/1000,cfg_.safety})) {
        const auto skip=uint64_t((queued-target)/block_)*block_;
        timeline_.cursor+=skip; stats_.skipped_frames+=skip; ++stats_.resyncs;
      }
      // Bounded clock discipline follows the Pi's average sample cadence.
      // +/-2000 ppm, with half-second correction, cannot chase a packet burst.
      const double error=double(int64_t(timeline_.latest)-int64_t(timeline_.cursor)-target);
      const double correction=std::clamp(error/(cfg_.rate*0.5),-0.002,0.002);
      deliver(uint64_t(deadline));
      deadline+=nominal*(1.0-correction);
    }
    audio_thread_id_=0;
  }

  std::atomic<ULONG> refs_{1};
  Config cfg_;
  SOCKET socket_=INVALID_SOCKET;
  WSAEVENT socket_event_=WSA_INVALID_EVENT;
  HANDLE stop_event_=nullptr,rx_event_=nullptr,tx_event_=nullptr;
  bool wsa_=false, time_info_=false;
  char error_[128]{};
  std::atomic<bool> running_{false}, output_ready_{false};
  std::atomic<DWORD> audio_thread_id_{0};
  std::thread receiver_,worker_,sender_;
  ASIOCallbacks* callbacks_=nullptr;
  std::vector<ChannelBuffer> buffers_;
  std::unique_ptr<aoip::SpscQueue<aoip::Packet,1024>> rx_queue_,tx_queue_;
  aoip::Timeline timeline_;
  std::vector<int32_t> input_scratch_;
  std::atomic<uint64_t> position_{0},timestamp_{0};
  uint64_t tx_frame_=0;
  uint64_t stream_position_=0;
  uint32_t tx_seq_=0;
  uint16_t tx_epoch_=1;
  unsigned wire_outputs_=0,source_frames_=1,partial_frames_=0;
  bool source_v2_=false;
  aoip::Packet partial_;
  long block_=32;
  int half_=0,pending_half_=-1;
