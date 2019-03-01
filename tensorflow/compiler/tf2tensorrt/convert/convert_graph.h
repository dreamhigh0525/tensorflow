/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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
#ifndef TENSORFLOW_COMPILER_TF2TENSORRT_CONVERT_CONVERT_GRAPH_H_
#define TENSORFLOW_COMPILER_TF2TENSORRT_CONVERT_CONVERT_GRAPH_H_

#include <vector>

#include "tensorflow/compiler/tf2tensorrt/convert/convert_nodes.h"
#include "tensorflow/core/framework/graph.pb.h"
#include "tensorflow/core/grappler/clusters/cluster.h"
#include "tensorflow/core/grappler/costs/graph_properties.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/types.h"

#if GOOGLE_CUDA
#if GOOGLE_TENSORRT

namespace tensorflow {
namespace tensorrt {
namespace convert {

// Helper class for the segmenter to determine whether given TF node is
// supported by TRT.
class TrtCandidateSelector {
 public:
  TrtCandidateSelector(const grappler::GraphProperties& graph_properties,
                       TrtPrecisionMode precision_mode);

  // Returns OK iff 'node' is a TF-TRT conversion candidate, which will be added
  // to TRT subgraph and later converted into TRT engine.
  Status IsTensorRTCandidate(const Node* node);

 private:
  // The TF-TRT node converter used to verify whether individual node is
  // supported. It will operate in validation-only mode.
  TrtNodeValidator validator_;

  // GraphProperties of the graph whose nodes are to be validated by
  // IsTensorRTCandidate().
  const grappler::GraphProperties& graph_properties_;

  // Quantization ops are only converted when using quantized precisions.
  const TrtPrecisionMode precision_mode_;
};

struct ConversionParams {
  ConversionParams()
      : input_graph_def(nullptr),
        max_batch_size(1),
        max_workspace_size_bytes(1 << 30),
        output_graph_def(nullptr),
        precision_mode(TrtPrecisionMode::FP32),
        minimum_segment_size(3),
        graph_properties(nullptr),
        cluster(nullptr),
        is_dyn_op(false),
        fixed_input_size(true),
        use_calibration(true),
        max_cached_engines(1) {}
  const GraphDef* input_graph_def;
  const std::vector<string>* output_names;
  size_t max_batch_size;
  size_t max_workspace_size_bytes;
  GraphDef* output_graph_def;
  TrtPrecisionMode precision_mode;
  int minimum_segment_size;
  const grappler::GraphProperties* graph_properties;
  const grappler::Cluster* cluster;
  bool is_dyn_op;  //  Whether to create engine on conversion or execution time
  bool fixed_input_size;   // Assume non-batch ranks of input tensors are fixed
  int max_cached_engines;  // maximum number of cached engines
  bool use_calibration;
  std::vector<int> cached_engine_batches;  // list of cached engines
};

// - max_batch_size: maximum batch size which can be used for inference for
//   optimization targets inference run with max batch size.
// - max_workspace_size_bytes: The upper bound of memory allowance for engine
//   building.
Status ConvertGraphDefToTensorRT(
    const GraphDef& graph_def, const std::vector<string>& output_names,
    size_t max_batch_size, size_t max_workspace_size_bytes,
    GraphDef* new_graph_def,
    TrtPrecisionMode precision_mode = TrtPrecisionMode::FP32,
    int minimum_segment_size = 3, bool is_dyn_op = false,
    int max_cached_engines = 1, std::vector<int> cached_engine_batches = {},
    bool use_calibration = true);

// Method to call from optimization pass
Status ConvertAfterShapes(ConversionParams& params);

// Return compile time TensorRT library version information.
std::vector<int> GetLinkedTensorRTVersion();

// Return runtime time TensorRT library version information.
std::vector<int> GetLoadedTensorRTVersion();

// Helper method for the conversion, expose for testing.
std::pair<int, Allocator*> GetDeviceAndAllocator(const ConversionParams& params,
                                                 const EngineInfo& engine);

}  // namespace convert
}  // namespace tensorrt
}  // namespace tensorflow

#endif  // GOOGLE_TENSORRT
#endif  // GOOGLE_CUDA

#endif  // TENSORFLOW_COMPILER_TF2TENSORRT_CONVERT_CONVERT_GRAPH_H_
