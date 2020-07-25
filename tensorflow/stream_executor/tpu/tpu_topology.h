/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef TENSORFLOW_STREAM_EXECUTOR_TPU_TPU_TOPOLOGY_H_
#define TENSORFLOW_STREAM_EXECUTOR_TPU_TPU_TOPOLOGY_H_

#include "tensorflow/core/platform/types.h"
#include "tensorflow/stream_executor/tpu/c_api_decl.h"

namespace tensorflow {
namespace tpu {

struct TpuChipCoordinatesExternal {
  int x;
  int y;
  int z;
};

class TpuCoreLocationExternal {
 public:
  TpuCoreLocationExternal() : core_location_(nullptr) {}
  explicit TpuCoreLocationExternal(void* core_location)
      : core_location_(core_location) {}
  TpuChipCoordinatesExternal chip_coordinates() const;
  int32 index() const;
  int32 Id() const;

 private:
  void* core_location_;
};

struct TpuTopologyChipBoundsExternal {
  int x;
  int y;
  int z;
};

class TpuTopologyExternal {
 public:
  explicit TpuTopologyExternal(void* topology) : topology_(topology) {}
  int32 LogicalDevicesPerHost(TpuCoreTypeEnum core_type) const;
  int32 LogicalDevicesPerChip(TpuCoreTypeEnum core_type) const;
  TpuTopologyChipBoundsExternal chip_bounds() const;
  bool HasChip(int x, int y, int z) const;
  TpuCoreLocationExternal Core(int x, int y, int z, TpuCoreTypeEnum core_type,
                               int index) const;

 private:
  void* topology_;
};

}  // namespace tpu
}  // namespace tensorflow

#endif  // TENSORFLOW_STREAM_EXECUTOR_TPU_TPU_TOPOLOGY_H_
