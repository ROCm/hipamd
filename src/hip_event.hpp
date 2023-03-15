/* Copyright (c) 2015 - 2021 Advanced Micro Devices, Inc.

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE. */

#ifndef HIP_EVENT_H
#define HIP_EVENT_H

#include "hip_internal.hpp"
#include "thread/monitor.hpp"

// Internal structure for stream callback handler
class StreamCallback {
protected:
  void* userData_;
 public:
  StreamCallback(void* userData)
      : userData_(userData) {}

  virtual void CL_CALLBACK callback() = 0;
};

class StreamAddCallback : public StreamCallback {
  hipStreamCallback_t callBack_;
  hipStream_t stream_;
public:
  StreamAddCallback(hipStream_t stream, hipStreamCallback_t callback, void* userData)
      : StreamCallback(userData) {
    stream_ = stream;
    callBack_ = callback;
  }

  void CL_CALLBACK callback() {
    hipError_t status = hipSuccess;
    callBack_(stream_, status, userData_);
  }
};

class LaunchHostFuncCallback : public StreamCallback {
  hipHostFn_t callBack_;
 public:
  LaunchHostFuncCallback(hipHostFn_t callback, void* userData)
      : StreamCallback(userData) {
    callBack_ = callback;
  }

  void CL_CALLBACK callback() { callBack_(userData_); }
};

void CL_CALLBACK ihipStreamCallback(cl_event event, cl_int command_exec_status, void* user_data);

namespace hip {

#define IPC_SIGNALS_PER_EVENT 32
typedef struct ihipIpcEventShmem_s {
  std::atomic<int> owners;
  std::atomic<int> owners_device_id;
  std::atomic<int> owners_process_id;
  std::atomic<int> read_index;
  std::atomic<int> write_index;
  uint32_t signal[IPC_SIGNALS_PER_EVENT];
} ihipIpcEventShmem_t;

class EventMarker : public amd::Marker {
 public:
  EventMarker(amd::HostQueue& stream, bool disableFlush, bool markerTs = false,
              int32_t scope = amd::Device::kCacheStateInvalid)
      : amd::Marker(stream, disableFlush) {
    profilingInfo_.enabled_ = true;
    profilingInfo_.callback_ = nullptr;
    profilingInfo_.marker_ts_ = markerTs;
    profilingInfo_.clear();
    setEventScope(scope);
  }
};

class Event {
  /// event recorded on stream where capture is active
  bool onCapture_;
  /// capture stream where event is recorded
  hipStream_t captureStream_ = nullptr;
  /// Previous captured nodes before event record
  std::vector<hipGraphNode_t> nodesPrevToRecorded_;

 public:
  Event(unsigned int flags) : flags(flags), lock_("hipEvent_t", true),
                              event_(nullptr), unrecorded_(false), stream_(nullptr) {
    // No need to init event_ here as addMarker does that
    onCapture_ = false;
    device_id_ = hip::getCurrentDevice()->deviceId();  // Created in current device ctx
  }

  virtual ~Event() {
    if (event_ != nullptr) {
      event_->release();
    }
  }
  unsigned int flags;

  virtual hipError_t query();
  virtual hipError_t synchronize();
  hipError_t elapsedTime(Event& eStop, float& ms);

  virtual hipError_t streamWaitCommand(amd::Command*& command, hip::Stream* stream);
  virtual hipError_t enqueueStreamWaitCommand(hipStream_t stream, amd::Command* command);
  virtual hipError_t streamWait(hipStream_t stream, uint flags);

  virtual hipError_t recordCommand(amd::Command*& command, amd::HostQueue* stream,
                                   uint32_t flags = 0);
  virtual hipError_t enqueueRecordCommand(hipStream_t stream, amd::Command* command, bool record);
  hipError_t addMarker(hipStream_t stream, amd::Command* command, bool record);

  void BindCommand(amd::Command& command, bool record) {
    amd::ScopedLock lock(lock_);
    if (event_ != nullptr) {
      event_->release();
    }
    event_ = &command.event();
    unrecorded_ = !record;
    command.retain();
  }

  bool isUnRecorded() const { return unrecorded_; }
  amd::Monitor& lock() { return lock_; }
  const int deviceId() const { return device_id_; }
  void setDeviceId(int id) { device_id_ = id; }
  amd::Event* event() { return event_; }

  /// End capture on this event
  void EndCapture() {
    onCapture_ = false;
    captureStream_ = nullptr;
  }
  /// Start capture when waited on this event
  void StartCapture(hipStream_t stream) {
    onCapture_ = true;
    captureStream_ = stream;
  }
  /// Get capture status of the graph
  bool GetCaptureStatus() const { return onCapture_; }
  /// Get capture stream where event is recorded
  hipStream_t GetCaptureStream() const { return captureStream_; }
  /// Set capture stream where event is recorded
  void SetCaptureStream(hipStream_t stream) { captureStream_ = stream; }
  /// Returns previous captured nodes before event record
  std::vector<hipGraphNode_t> GetNodesPrevToRecorded() const { return nodesPrevToRecorded_; }
  /// Set last captured graph node before event record
  void SetNodesPrevToRecorded(std::vector<hipGraphNode_t>& graphNode) {
    nodesPrevToRecorded_ = graphNode;
  }
  virtual hipError_t GetHandle(ihipIpcEventHandle_t* handle) {
    return hipErrorInvalidConfiguration;
  }
  virtual hipError_t OpenHandle(ihipIpcEventHandle_t* handle) {
    return hipErrorInvalidConfiguration;
  }
  virtual bool awaitEventCompletion();
  virtual bool ready();
  virtual int64_t time(bool getStartTs) const;

 protected:
  amd::Monitor lock_;
  hip::Stream* stream_;
  amd::Event* event_;
  int device_id_;
  //! Flag to indicate hipEventRecord has not been called. This is needed for
  //! hip*ModuleLaunchKernel API which takes start and stop events so no
  //! hipEventRecord is called. Cleanup needed once those APIs are deprecated.
  bool unrecorded_;
};

class EventDD : public Event {
 public:
  EventDD(unsigned int flags) : Event(flags) {}
  virtual ~EventDD() {}

  virtual bool awaitEventCompletion();
  virtual bool ready();
  virtual int64_t time(bool getStartTs) const;
};

class IPCEvent : public Event {
  // IPC Events
  struct ihipIpcEvent_t {
    std::string ipc_name_;
    int ipc_fd_;
    ihipIpcEventShmem_t* ipc_shmem_;
    ihipIpcEvent_t() : ipc_name_("dummy"), ipc_fd_(0), ipc_shmem_(nullptr) {}
    void setipcname(const char* name) { ipc_name_ = std::string(name); }
  };
  ihipIpcEvent_t ipc_evt_;

 public:
  ~IPCEvent() {
    if (ipc_evt_.ipc_shmem_) {
      int owners = --ipc_evt_.ipc_shmem_->owners;
      // Make sure event is synchronized
      hipError_t status = synchronize();
      status  = ihipHostUnregister(&ipc_evt_.ipc_shmem_->signal);
      if (!amd::Os::MemoryUnmapFile(ipc_evt_.ipc_shmem_, sizeof(hip::ihipIpcEventShmem_t))) {
        // print hipErrorInvalidHandle;
      }
    }
  }
  IPCEvent() : Event(hipEventInterprocess) {}
  bool createIpcEventShmemIfNeeded();
  hipError_t GetHandle(ihipIpcEventHandle_t* handle);
  hipError_t OpenHandle(ihipIpcEventHandle_t* handle);
  hipError_t synchronize();
  hipError_t query();

  hipError_t streamWaitCommand(amd::Command*& command, hip::Stream* stream);
  hipError_t enqueueStreamWaitCommand(hipStream_t stream, amd::Command* command);
  hipError_t streamWait(hipStream_t stream, uint flags);

  hipError_t recordCommand(amd::Command*& command, amd::HostQueue* queue, uint32_t flags = 0);
  hipError_t enqueueRecordCommand(hipStream_t stream, amd::Command* command, bool record);
};

};  // namespace hip

struct CallbackData {
  int previous_read_index;
  hip::ihipIpcEventShmem_t* shmem;
};

#endif  // HIP_EVEMT_H
