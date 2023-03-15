/* Copyright (c) 2015 - 2022 Advanced Micro Devices, Inc.

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

#include <hip/hip_runtime.h>

#include "hip_event.hpp"
#if !defined(_MSC_VER)
#include <unistd.h>
#else
#include <io.h>
#endif

// ================================================================================================

hipError_t ihipEventCreateWithFlags(hipEvent_t* event, unsigned flags);

namespace hip {

bool IPCEvent::createIpcEventShmemIfNeeded() {
  if (ipc_evt_.ipc_shmem_) {
    // ipc_shmem_ already created, no need to create it again
    return true;
  }

  char name_template[] = "/tmp/eventXXXXXX";
#if !defined(_MSC_VER)
  int temp_fd = mkstemp(name_template);
#else
  _mktemp_s(name_template, sizeof(name_template));
#endif

  ipc_evt_.ipc_name_ = name_template;
  ipc_evt_.ipc_name_.replace(0, 5, "/hip_");
  if (!amd::Os::MemoryMapFileTruncated(
          ipc_evt_.ipc_name_.c_str(),
          const_cast<const void**>(reinterpret_cast<void**>(&(ipc_evt_.ipc_shmem_))),
          sizeof(hip::ihipIpcEventShmem_t))) {
    return false;
  }

#if !defined(_MSC_VER)
  close(temp_fd);
#endif

  ipc_evt_.ipc_shmem_->owners = 1;
  ipc_evt_.ipc_shmem_->read_index = -1;
  ipc_evt_.ipc_shmem_->write_index = 0;
  for (uint32_t sig_idx = 0; sig_idx < IPC_SIGNALS_PER_EVENT; ++sig_idx) {
    ipc_evt_.ipc_shmem_->signal[sig_idx] = 0;
  }

  // device sets 0 to this ptr when the ipc event is completed
  hipError_t status = ihipHostRegister(&ipc_evt_.ipc_shmem_->signal,
                                       sizeof(uint32_t) * IPC_SIGNALS_PER_EVENT,
                                       0);
  if (status != hipSuccess) {
    return false;
  }
  return true;
}

hipError_t IPCEvent::query() {
  if (ipc_evt_.ipc_shmem_) {
    int prev_read_idx = ipc_evt_.ipc_shmem_->read_index;
    int offset = (prev_read_idx % IPC_SIGNALS_PER_EVENT);
    if (ipc_evt_.ipc_shmem_->read_index < prev_read_idx + IPC_SIGNALS_PER_EVENT &&
        ipc_evt_.ipc_shmem_->signal[offset] != 0) {
      return hipErrorNotReady;
    }
  }
  return hipSuccess;
}

hipError_t IPCEvent::synchronize() {
  if (ipc_evt_.ipc_shmem_) {
    int prev_read_idx = ipc_evt_.ipc_shmem_->read_index;
    if (prev_read_idx >= 0) {
      int offset = (prev_read_idx % IPC_SIGNALS_PER_EVENT);
      while ((ipc_evt_.ipc_shmem_->read_index < prev_read_idx + IPC_SIGNALS_PER_EVENT) &&
             (ipc_evt_.ipc_shmem_->signal[offset] != 0)) {
        amd::Os::sleep(1);
      }
    }
  }
  return hipSuccess;
}

hipError_t IPCEvent::streamWaitCommand(amd::Command*& command, hip::Stream* stream) {
  command = new amd::Marker(*stream, false);
  if (command == NULL) {
    return hipErrorOutOfMemory;
  }
  return hipSuccess;
}

hipError_t IPCEvent::enqueueStreamWaitCommand(hipStream_t stream, amd::Command* command) {
  auto t{new CallbackData{ipc_evt_.ipc_shmem_->read_index, ipc_evt_.ipc_shmem_}};
  StreamCallback* cbo = new StreamAddCallback(
      stream, reinterpret_cast<hipStreamCallback_t>(WaitThenDecrementSignal), t);
  if (!command->setCallback(CL_COMPLETE, ihipStreamCallback, cbo)) {
    command->release();
    return hipErrorInvalidHandle;
  }
  command->enqueue();
  command->release();
  command->awaitCompletion();
  return hipSuccess;
}

hipError_t IPCEvent::streamWait(hipStream_t stream, uint flags) {
  hip::Stream* hip_stream = hip::getStream(stream);

  amd::ScopedLock lock(lock_);
  if(query() != hipSuccess) {
    amd::Command* command;
    hipError_t status = streamWaitCommand(command, hip_stream);
    if (status != hipSuccess) {
      return status;
    }
    status = enqueueStreamWaitCommand(stream, command);
    return status;
  }
  return hipSuccess;
}

hipError_t IPCEvent::recordCommand(amd::Command*& command, amd::HostQueue* stream, uint32_t flags) {
  bool unrecorded = isUnRecorded();
  if (unrecorded) {
    command = new amd::Marker(*stream, kMarkerDisableFlush);
  } else {
    return Event::recordCommand(command, stream);
  }
  return hipSuccess;
}

hipError_t IPCEvent::enqueueRecordCommand(hipStream_t stream, amd::Command* command, bool record) {
  bool unrecorded = isUnRecorded();
  if (unrecorded) {
    amd::Event& tEvent = command->event();
    createIpcEventShmemIfNeeded();
    int write_index = ipc_evt_.ipc_shmem_->write_index++;
    int offset = write_index % IPC_SIGNALS_PER_EVENT;
    while (ipc_evt_.ipc_shmem_->signal[offset] != 0) {
      amd::Os::sleep(1);
    }
    // Lock signal.
    ipc_evt_.ipc_shmem_->signal[offset] = 1;
    ipc_evt_.ipc_shmem_->owners_device_id = deviceId();
    command->enqueue();

    // device writes 0 to signal after the hipEventRecord command is completed
    // the signal value is checked by WaitThenDecrementSignal cb
    hipError_t status = ihipStreamOperation(stream, ROCCLR_COMMAND_STREAM_WRITE_VALUE,
                                 &(ipc_evt_.ipc_shmem_->signal[offset]),
                                 0,
                                 0, 0, sizeof(uint32_t));
    if (status != hipSuccess) {
      return status;
    }

    // Update read index to indicate new signal.
    int expected = write_index - 1;
    while (!ipc_evt_.ipc_shmem_->read_index.compare_exchange_weak(expected, write_index)) {
      amd::Os::sleep(1);
    }
  } else {
    return Event::enqueueRecordCommand(stream, command, record);
  }
  return hipSuccess;
}

hipError_t IPCEvent::GetHandle(ihipIpcEventHandle_t* handle) {
  if (!createIpcEventShmemIfNeeded()) {
    return hipErrorInvalidValue;
  }
  ipc_evt_.ipc_shmem_->owners_device_id = deviceId();
  ipc_evt_.ipc_shmem_->owners_process_id = amd::Os::getProcessId();
  memset(handle->shmem_name, 0, HIP_IPC_HANDLE_SIZE);
  ipc_evt_.ipc_name_.copy(handle->shmem_name, std::string::npos);
  return hipSuccess;
}

hipError_t IPCEvent::OpenHandle(ihipIpcEventHandle_t* handle) {
  ipc_evt_.ipc_name_ = handle->shmem_name;
  if (!amd::Os::MemoryMapFileTruncated(ipc_evt_.ipc_name_.c_str(),
                                       (const void**)&(ipc_evt_.ipc_shmem_),
                                       sizeof(ihipIpcEventShmem_t))) {
    return hipErrorInvalidValue;
  }

  if (amd::Os::getProcessId() == ipc_evt_.ipc_shmem_->owners_process_id.load()) {
    // If this is in the same process, return error.
    return hipErrorInvalidContext;
  }

  ipc_evt_.ipc_shmem_->owners += 1;
  // device sets 0 to this ptr when the ipc event is completed
  hipError_t status = hipSuccess;
  status = ihipHostRegister(&ipc_evt_.ipc_shmem_->signal,
                            sizeof(uint32_t) * IPC_SIGNALS_PER_EVENT,
                            0);
  return status;
}

}  // namespace hip

// ================================================================================================

hipError_t hipIpcGetEventHandle(hipIpcEventHandle_t* handle, hipEvent_t event) {
  HIP_INIT_API(hipIpcGetEventHandle, handle, event);

  if (handle == nullptr || event == nullptr) {
    HIP_RETURN(hipErrorInvalidValue);
  }
  hip::Event* e = reinterpret_cast<hip::Event*>(event);
  HIP_RETURN(e->GetHandle(reinterpret_cast<ihipIpcEventHandle_t*>(handle)));
}

hipError_t hipIpcOpenEventHandle(hipEvent_t* event, hipIpcEventHandle_t handle) {
  HIP_INIT_API(hipIpcOpenEventHandle, event, handle);

  hipError_t hip_err = hipSuccess;
  if (event == nullptr) {
    HIP_RETURN(hipErrorInvalidValue);
  }
  hip_err = ihipEventCreateWithFlags(event, hipEventDisableTiming | hipEventInterprocess);
  if (hip_err != hipSuccess) {
    HIP_RETURN(hip_err);
  }
  hip::Event* e = reinterpret_cast<hip::Event*>(*event);
  ihipIpcEventHandle_t* iHandle = reinterpret_cast<ihipIpcEventHandle_t*>(&handle);
  HIP_RETURN(e->OpenHandle(iHandle));
}
