/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/cc/saved_model/fingerprinting.h"

#include <string>

#include "tensorflow/core/framework/attr_value.pb.h"
#include "tensorflow/core/framework/function.pb.h"
#include "tensorflow/core/framework/op_def.pb.h"
#include "tensorflow/core/framework/types.pb.h"
#include "tensorflow/core/framework/versions.pb.h"
#include "tensorflow/core/grappler/op_types.h"
#include "tensorflow/core/lib/strings/proto_serialization.h"
#include "tensorflow/core/platform/fingerprint.h"
#include "tensorflow/core/protobuf/fingerprint.pb.h"
#include "tensorflow/core/protobuf/meta_graph.pb.h"
#include "tensorflow/core/protobuf/saved_model.pb.h"

namespace tensorflow::fingerprinting {

namespace {

// This function mutates the GraphDef, changing the names and config_proto's
// of the Function nodes.
void CanonicalizeNodes(GraphDef* orig_graph_def) {
  for (NodeDef& node : *orig_graph_def->mutable_node()) {
    // Check if this is a function call.
    if (grappler::IsPartitionedCall(node) ||
        grappler::IsStatefulPartitionedCall(node)) {
      // TODO(b/240174577): Strip UID from the end of function names.
      // Regularize "f" attribute, the function name for PartitionedCall and
      // and StatefulPartitionedCall ops.
      node.mutable_attr()->find("f")->second.mutable_func()->set_name(
          "FINGERPRINT_PASS");
      // Erase the "config_proto" attribute which contains device-specific
      // information.
      node.mutable_attr()->find("config_proto")->second.mutable_s()->erase();
    }
    // Erase the value of string constants, which can vary based on platform.
    if (grappler::IsConstant(node)) {
      if (node.attr().at("dtype").type() == DT_STRING) {
        node.mutable_attr()->find("value")->second.clear_value();
      }
    }
  }
}

}  // namespace

uint64 ComputeHash(const GraphDef& graph_def) {
  std::string graph_def_string;
  SerializeToStringDeterministic(graph_def, &graph_def_string);
  return tensorflow::Fingerprint64(graph_def_string);
}

FingerprintDef CreateFingerprintDef(const MetaGraphDef& metagraph) {
  // Create a copy of `metagraph` which will be used and mutated for fingerprint
  // computation.
  MetaGraphDef metagraph_copy = metagraph;
  FingerprintDef fingerprint_def;
  fingerprint_def.set_graph_def_checksum(
      ComputeHash(metagraph_copy.graph_def()));
  CanonicalizeGraphDef(*metagraph_copy.mutable_graph_def());
  fingerprint_def.set_graph_def_program_hash(
      ComputeHash(metagraph_copy.graph_def()));
  return fingerprint_def;
}

// The GraphDef contains two main sections: a list of nodes and the
// FunctionDefLibrary. Canonicalization treats these two sections separately.
void CanonicalizeGraphDef(GraphDef& graph_def) {
  CanonicalizeNodes(&graph_def);
  // TODO(b/240173815): Complete canonicalization of the FunctionDefLibrary.
  // For now, we just clear the FunctionDefLibrary.
  graph_def.mutable_library()->Clear();
  graph_def.mutable_versions()->Clear();
}

}  // namespace tensorflow::fingerprinting
