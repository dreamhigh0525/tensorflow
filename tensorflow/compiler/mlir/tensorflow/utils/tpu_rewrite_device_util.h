/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_COMPILER_MLIR_TENSORFLOW_UTILS_TPU_REWRITE_DEVICE_UTIL_H_
#define TENSORFLOW_COMPILER_MLIR_TENSORFLOW_UTILS_TPU_REWRITE_DEVICE_UTIL_H_

#include <string>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/util/device_name_utils.h"
#include "tensorflow/stream_executor/lib/statusor.h"

namespace tensorflow {
using stream_executor::port::StatusOr;

// TPU devices to be used for execution (e.g. devices for TPUExecute ops). They
// are ordered by `num_replicas` followed by `num_cores_per_replica`.
using ExecutionDevices =
    llvm::SmallVector<llvm::SmallVector<std::string, 8>, 8>;

// TPU compilation device, execution devices, and optionally execution device
// IDs. Execution device IDs are populated if `topology` and `device_assignment`
// are provided.
struct TPUDeviceAssignment {
  TPUDeviceAssignment(llvm::StringRef compilation_device,
                      ExecutionDevices&& execution_devices)
      : compilation_device(compilation_device),
        execution_devices(std::move(execution_devices)) {}

  TPUDeviceAssignment(llvm::StringRef compilation_device,
                      ExecutionDevices&& execution_devices,
                      xla::DeviceAssignmentProto&& xla_device_assignment)
      : compilation_device(compilation_device),
        execution_devices(std::move(execution_devices)),
        xla_device_assignment(std::move(xla_device_assignment)) {}

  std::string compilation_device;
  ExecutionDevices execution_devices;
  llvm::Optional<xla::DeviceAssignmentProto> xla_device_assignment;
};

// Finds the TPU compilation device and execution devices from `devices` for a
// TPU computation subgraph. Compilation device is determined from looking up
// all TPU_SYSTEM:0 devices and choosing the CPU device associated to the first
// TPU_SYSTEM device sorted lexicographically by replica and task. Execution
// devices are determined by looking up all TPU devices associated with each
// TPU_SYSTEM:0 device found, alongside associated `topology_attr` and
// `device_assignment_attr`. If `topology_attr` not an empty string (parsable to
// TopologyProto), `device_assignment_attr` must not be empty also. When
// `topology_attr` and `device_assignment_attr` are not empty, a general device
// assignment based on those two attributes are used. Otherwise when
// `topology_attr` and `device_assignment_attr` are empty, a full mesh device
// assignment is used instead. A failure will be returned if it is not possible
// (e.g. invalid devices or invalid parameters).
//
//
// For example, for `devices`:
//   {
//     /job:localhost/replica:0/task:0/device:CPU:0,
//     /job:worker/replica:0/task:0/device:CPU:0,
//     /job:worker/replica:0/task:0/device:TPU_SYSTEM:0,
//     /job:worker/replica:0/task:0/device:TPU:0,
//     /job:worker/replica:0/task:0/device:TPU:1,
//     /job:worker/replica:0/task:0/device:TPU:2,
//     /job:worker/replica:0/task:0/device:TPU:3,
//     /job:worker/replica:0/task:1/device:CPU:0,
//     /job:worker/replica:0/task:1/device:TPU_SYSTEM:0,
//     /job:worker/replica:0/task:1/device:TPU:0,
//     /job:worker/replica:0/task:1/device:TPU:1,
//     /job:worker/replica:0/task:1/device:TPU:2,
//     /job:worker/replica:0/task:1/device:TPU:3
//   }
//
//
// With the following parameters (full mesh device assignment):
//   `num_replicas` = 8
//   `num_cores_per_replica` = 1
//   `topology_attr` = ""
//   `device_assignment_attr` = {}
//
// The `compilation_device` will be:
//   /job:worker/replica:0/task:0/device:CPU:0
//
// `execution_devices` will be:
//   {
//     {
//       /job:worker/replica:0/task:0/device:TPU:0
//     },
//     {
//       /job:worker/replica:0/task:0/device:TPU:1
//     },
//     {
//       /job:worker/replica:0/task:0/device:TPU:2
//     },
//     {
//       /job:worker/replica:0/task:0/device:TPU:3
//     },
//     {
//       /job:worker/replica:0/task:1/device:TPU:0
//     },
//     {
//       /job:worker/replica:0/task:1/device:TPU:1
//     },
//     {
//       /job:worker/replica:0/task:1/device:TPU:2
//     },
//     {
//       /job:worker/replica:0/task:1/device:TPU:3
//     }
//   }
//
// and `xla_device_assignment` will not be set.
//
//
// With the following parameters (general device assignment):
//   `num_replicas` = 4
//   `num_cores_per_replica` = 2
//   `topology_attr` (in proto debug string format) =
//     {
//       mesh_shape: 2
//       mesh_shape: 2
//       mesh_shape: 2
//       num_tasks: 2
//       num_tpu_devices_per_task: 4
//       device_coordinates: 0
//       device_coordinates: 0
//       device_coordinates: 0
//       device_coordinates: 0
//       device_coordinates: 1
//       device_coordinates: 0
//       device_coordinates: 1
//       device_coordinates: 1
//       device_coordinates: 0
//       device_coordinates: 1
//       device_coordinates: 0
//       device_coordinates: 0
//       device_coordinates: 1
//       device_coordinates: 0
//       device_coordinates: 1
//       device_coordinates: 1
//       device_coordinates: 1
//       device_coordinates: 1
//       device_coordinates: 0
//       device_coordinates: 1
//       device_coordinates: 1
//       device_coordinates: 0
//       device_coordinates: 0
//       device_coordinates: 1
//     }
//   `device_assignment` =
//     {0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1}
//
// The `compilation_device` will be:
//   /job:worker/replica:0/task:0/device:CPU:0
//
// `execution_devices` will be:
//   {
//     {
//       "/job:worker/replica:0/task:0/device:TPU:0",
//       "/job:worker/replica:0/task:1/device:TPU:3"
//     },
//     {
//       "/job:worker/replica:0/task:0/device:TPU:1",
//       "/job:worker/replica:0/task:1/device:TPU:2"
//     },
//     {
//       "/job:worker/replica:0/task:0/device:TPU:3",
//       "/job:worker/replica:0/task:1/device:TPU:0"
//     },
//     {
//       "/job:worker/replica:0/task:0/device:TPU:2",
//       "/job:worker/replica:0/task:1/device:TPU:1"
//     }
//   }
//
// and `xla_device_assignment` will be:
//   {
//     replica_count: 4
//     computation_count: 2
//     computation_devices {
//       replica_device_ids: 0
//       replica_device_ids: 4
//       replica_device_ids: 2
//       replica_device_ids: 6
//     }
//     computation_devices {
//       replica_device_ids: 1
//       replica_device_ids: 5
//       replica_device_ids: 3
//       replica_device_ids: 7
//     }
//   }
StatusOr<TPUDeviceAssignment> GetTPUCompilationAndExecutionDevices(
    llvm::ArrayRef<DeviceNameUtils::ParsedName> devices, int num_replicas,
    int num_cores_per_replica, llvm::StringRef topology_attr,
    llvm::ArrayRef<int64_t> device_assignment_attr);

// Virtual device is used for evice assignment for executing ops on a specified
// logical core.
std::string GetDeviceAliasForLogicalCore(int core_index);

// Finds associated CPU host device for given TPU device. This assumes a
// matching CPU host device exists based on TPU device name. An error will be
// returned if the TPU device name is invalid.
StatusOr<std::string> GetCPUHostForTPUDevice(llvm::StringRef tpu_device);

// Finds associated CPU host devices for given TPU devices. This assumes a
// matching CPU host device exist based on each TPU device name. An error will
// be returned if a TPU device name is invalid.
StatusOr<llvm::SmallVector<std::string, 8>> GetCPUHostsForTPUDevices(
    llvm::ArrayRef<std::string> tpu_devices);

}  // namespace tensorflow

#endif  // TENSORFLOW_COMPILER_MLIR_TENSORFLOW_UTILS_TPU_REWRITE_DEVICE_UTIL_H_
