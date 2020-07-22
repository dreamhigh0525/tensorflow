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

#include "tensorflow/stream_executor/tpu/tpu_topology.h"

#include "tensorflow/core/tpu/tpu_api.h"

namespace tensorflow {
namespace tpu {

TpuChipCoordinatesExternal TpuCoreLocationExternal::chip_coordinates() const {
  return {
      tpu::ExecutorApiFn()->TpuCoreLocation_ChipCoordinates_XFn(core_location_),
      tpu::ExecutorApiFn()->TpuCoreLocation_ChipCoordinates_YFn(core_location_),
      tpu::ExecutorApiFn()->TpuCoreLocation_ChipCoordinates_ZFn(
          core_location_)};
}

int32 TpuCoreLocationExternal::index() const {
  return tpu::ExecutorApiFn()->TpuCoreLocation_IndexFn(core_location_);
}

int32 TpuCoreLocationExternal::Id() const {
  return tpu::ExecutorApiFn()->TpuCoreLocation_IdFn(core_location_);
}

int32 TpuTopologyExternal::LogicalDevicesPerHost(
    TpuCoreTypeEnum core_type) const {
  return tpu::ExecutorApiFn()->TpuTopology_LogicalDevicesPerHostFn(topology_,
                                                                   core_type);
}

int32 TpuTopologyExternal::LogicalDevicesPerChip(
    TpuCoreTypeEnum core_type) const {
  return tpu::ExecutorApiFn()->TpuTopology_LogicalDevicesPerChipFn(topology_,
                                                                   core_type);
}

TpuTopologyChipBoundsExternal TpuTopologyExternal::chip_bounds() const {
  return {tpu::ExecutorApiFn()->TpuTopology_ChipBounds_XFn(topology_),
          tpu::ExecutorApiFn()->TpuTopology_ChipBounds_YFn(topology_),
          tpu::ExecutorApiFn()->TpuTopology_ChipBounds_ZFn(topology_)};
}

bool TpuTopologyExternal::HasChip(int x, int y, int z) const {
  return tpu::ExecutorApiFn()->TpuTopology_HasChipFn(topology_, x, y, z);
}

TpuCoreLocationExternal TpuTopologyExternal::Core(int x, int y, int z,
                                                  TpuCoreTypeEnum core_type,
                                                  int index) const {
  return TpuCoreLocationExternal(tpu::ExecutorApiFn()->TpuTopology_CoreFn(
      topology_, x, y, z, core_type, index));
}

}  // namespace tpu
}  // namespace tensorflow
