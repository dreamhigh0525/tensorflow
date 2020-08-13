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

#ifndef TENSORFLOW_CORE_PROFILER_CONVERT_XPLANE_TO_OP_STATS_H_
#define TENSORFLOW_CORE_PROFILER_CONVERT_XPLANE_TO_OP_STATS_H_

#include "absl/container/flat_hash_set.h"
#include "tensorflow/core/profiler/protobuf/op_stats.pb.h"
#include "tensorflow/core/profiler/protobuf/xplane.pb.h"

namespace tensorflow {
namespace profiler {

enum OpStatsKind {
  OP_METRICS_DB,
  STEP_DB,
  KERNEL_STATS_DB,
};

using OpStatsConfig = absl::flat_hash_set<OpStatsKind>;

// NOTE: call GroupTfEvents before if OpStats.step_db needs to be generated.
OpStats ConvertXSpaceToOpStats(const XSpace& space,
                               const OpStatsConfig& config);

// Propagate and dedup the diagnostics in XSpace and add to OpStats.
void PropagateXSpaceDiagnosticsToOpStats(const XSpace& space,
                                         OpStats* op_stats);

// Populates PerfEnv.
PerfEnv MakePerfEnv(double peak_tera_flops_per_second,
                    double peak_hbm_bw_giga_bytes_per_second);

// Extracts PerfEnv from XPlane stats.
PerfEnv GetPerfEnvFromXPlane(const XPlane& device_plane);

}  // namespace profiler
}  // namespace tensorflow

#endif  // TENSORFLOW_CORE_PROFILER_CONVERT_XPLANE_TO_OP_STATS_H_
