// Copyright 2019 ByteDance Inc. All Rights Reserved.
// Copyright 2018 Uber Technologies, Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#include <chrono>
#include <memory>
#include <thread>
#include <torch/extension.h>
#include <torch/torch.h>

#include "../common/operations.h"
#include "adapter.h"
#include "cuda_util.h"
#include "handle_manager.h"
#include "ready_event.h"

namespace byteps {
namespace torch {

static HandleManager handle_manager;

namespace {

std::string GetOpName(const std::string& prefix, const std::string& name,
                      int handle) {
  if (!name.empty()) {
    return prefix + "." + std::string(name);
  }
  return prefix + ".noname." + std::to_string(handle);
}

int GetDeviceID(const ::torch::Tensor& tensor) {
  if (tensor.device().is_cuda()) {
    return tensor.device().index();
  }
  return CPU_DEVICE_ID;
}

} // namespace

int DoPushPull(::torch::Tensor tensor, ::torch::Tensor output, int average,
                const std::string& name, int version, int priority) {
    ThrowIfError(common::CheckInitialized());

    auto handle = handle_manager.AllocateHandle();
    auto device = GetDeviceID(tensor);
    auto ready_event = RecordReadyEvent(device);
    auto byteps_input = std::make_shared<TorchTensor>(tensor);
    auto byteps_output = std::make_shared<TorchTensor>(output);

    std::string tensor_name = GetOpName("byteps", name.c_str(), 0);
    size_t size = byteps_input->size();
    auto dtype = byteps_input->dtype();

    // check if we need to init the tensor
    if (!common::IsTensorInitialized(tensor_name, size)) {
        // we need to init this tensor with PS
        auto& context = common::GetContextFromName(tensor_name);
        // the following init is blocking, in order to guarantee the order
        common::InitTensor(context, tensor_name, dtype,
          (device == CPU_DEVICE_ID) ? const_cast<void*>(byteps_input->data()) : nullptr);
    }

    auto& context = common::GetContextFromName(tensor_name);

    std::vector<common::QueueType> queue_list;
    if (common::IsRoot()) {
        queue_list.push_back(common::REDUCE);
        if (common::IsDistributedJob()) {
            queue_list.push_back(common::COPYD2H);
            queue_list.push_back(common::PUSH);
            queue_list.push_back(common::PULL);
            queue_list.push_back(common::COPYH2D);
        }
        queue_list.push_back(common::BROADCAST);
    }
    else {
        queue_list.push_back(common::COORDINATE_REDUCE);
        queue_list.push_back(common::REDUCE);
        queue_list.push_back(common::COORDINATE_BROADCAST);
        queue_list.push_back(common::BROADCAST);
    }

    auto enqueue_result = common::EnqueueTensor(
        context, byteps_input, byteps_output, ready_event,
        tensor_name, device, priority, version,
        [handle, average, tensor](const Status& status) mutable {
            // Will execute in the `device` context.
            if (average) {
                tensor.div_(byteps_size());
            }
            handle_manager.MarkDone(handle, status);
        }, queue_list);

    ThrowIfError(enqueue_result);

    return handle;
}

int PollHandle(int handle) { return handle_manager.PollHandle(handle) ? 1 : 0; }

void WaitAndClear(int handle) {
  while (!handle_manager.PollHandle(handle)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  auto status = handle_manager.ReleaseHandle(handle);
  ThrowIfError(*status);
}

PYBIND11_MODULE(c_lib, m) {
  // push_pull
  m.def("byteps_torch_push_pull_async_torch_IntTensor", &DoPushPull);
  m.def("byteps_torch_push_pull_async_torch_LongTensor", &DoPushPull);
  m.def("byteps_torch_push_pull_async_torch_HalfTensor", &DoPushPull);
  m.def("byteps_torch_push_pull_async_torch_FloatTensor", &DoPushPull);
  m.def("byteps_torch_push_pull_async_torch_DoubleTensor", &DoPushPull);

#if HAVE_CUDA
  m.def("byteps_torch_push_pull_async_torch_cuda_IntTensor", &DoPushPull);
  m.def("byteps_torch_push_pull_async_torch_cuda_LongTensor", &DoPushPull);
  m.def("byteps_torch_push_pull_async_torch_cuda_HalfTensor", &DoPushPull);
  m.def("byteps_torch_push_pull_async_torch_cuda_FloatTensor", &DoPushPull);
  m.def("byteps_torch_push_pull_async_torch_cuda_DoubleTensor", &DoPushPull);
#endif

  // basics
  m.def("byteps_torch_poll", &PollHandle);
  m.def("byteps_torch_wait_and_clear", &WaitAndClear);
}

} // namespace torch
} // namespace byteps