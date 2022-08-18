/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/core/profiler/convert/hlo_to_tools_data.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "tensorflow/compiler/xla/service/hlo.pb.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/path.h"
#include "tensorflow/core/platform/statusor.h"
#include "tensorflow/core/profiler/convert/hlo_proto_to_graph_view.h"
#include "tensorflow/core/profiler/convert/hlo_proto_to_memory_visualization_utils.h"
#include "tensorflow/core/profiler/convert/tool_options.h"
#include "tensorflow/core/profiler/convert/xplane_to_hlo.h"

namespace tensorflow {
namespace profiler {

namespace {

StatusOr<std::string> ConvertHloProtoToMemoryViewer(
    const xla::HloProto& hlo_proto) {
  static constexpr int kSmallBufferSize = 16 * 1024;  // 16KB
  static constexpr int kMemorySpaceColor = 0;         // HBM

  auto result_or = ConvertHloProtoToPreprocessResult(
      hlo_proto, kSmallBufferSize,
      GetHeapSimulatorTraceId(hlo_proto, kMemorySpaceColor), kMemorySpaceColor);
  if (!result_or.ok()) {
    return errors::Internal(
        "Failed to convert HLO proto to memory viewer result: ",
        result_or.status().message());
  }

  std::string json_output;
  tensorflow::protobuf::util::JsonPrintOptions options;
  options.always_print_primitive_fields = true;
  auto encoded_status = tensorflow::protobuf::util::MessageToJsonString(
      result_or.value(), &json_output, options);
  if (!encoded_status.ok()) {
    const auto& error_message = encoded_status.message();
    return errors::InvalidArgument(
        "Failed to convert memory viewer result to JSON format: ",
        absl::string_view(error_message.data(), error_message.length()));
  }

  return json_output;
}

StatusOr<std::string> ConvertHloProtoToGraphViewer(
    const xla::HloProto& hlo_proto, const ToolOptions& options) {
  TF_ASSIGN_OR_RETURN(GraphViewerParams params,
                      ParseGraphViewerParams(options));
  if (params.type == "graph") {
    return ConvertHloProtoToGraph(hlo_proto, params.node_name,
                                  params.graph_width, params.render_options,
                                  params.format);
  } else {
    return ConvertHloProtoToStringView(hlo_proto, params.verbose,
                                       params.show_metadata);
  }
}

}  // namespace

StatusOr<std::string> ConvertHloProtoToToolData(
    const std::vector<std::string>& xspace_paths,
    const absl::string_view tool_name, const ToolOptions& options) {
  if (xspace_paths.empty()) {
    return std::string();
  }

  // <options> must provide a hlo_module_name field to identify the HLO module.
  std::optional<std::string> hlo_module_name =
      GetParam<std::string>(options, "hlo_module_name");
  if (!hlo_module_name.has_value() || hlo_module_name->empty()) {
    return errors::InvalidArgument(
        "Can not find HLO module name from options.");
  }

  // Load HLO module from file.
  absl::string_view base_dir = tensorflow::io::Dirname(xspace_paths[0]);
  std::string hlo_proto_file_name =
      GetHloProtoFileName(base_dir, *hlo_module_name);
  xla::HloProto hlo_proto;
  TF_RETURN_IF_ERROR(tensorflow::ReadBinaryProto(
      tensorflow::Env::Default(), hlo_proto_file_name, &hlo_proto));

  // Convert from HLO proto to tools data.
  if (tool_name == "memory_viewer") {
    return ConvertHloProtoToMemoryViewer(hlo_proto);
  } else if (tool_name == "graph_viewer") {
    return ConvertHloProtoToGraphViewer(hlo_proto, options);
  } else {
    return errors::InvalidArgument(
        "Can not find tool: ", tool_name,
        ". Please update to the latest version of Tensorflow.");
  }
}

}  // namespace profiler
}  // namespace tensorflow
