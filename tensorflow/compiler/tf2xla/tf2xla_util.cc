/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/tf2xla/tf2xla_util.h"

#include <queue>
#include <random>
#include <set>
#include <unordered_map>

#include "absl/strings/str_cat.h"
#include "absl/types/optional.h"
#include "tensorflow/compiler/tf2xla/sharding_util.h"
#include "tensorflow/compiler/tf2xla/tf2xla.pb.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/common_runtime/function.h"
#include "tensorflow/core/framework/graph.pb.h"
#include "tensorflow/core/framework/graph_def_util.h"
#include "tensorflow/core/framework/graph_to_functiondef.h"
#include "tensorflow/core/framework/node_def.pb.h"
#include "tensorflow/core/framework/node_def_builder.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/tensor_shape.pb.h"
#include "tensorflow/core/framework/versions.pb.h"
#include "tensorflow/core/graph/tensor_id.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"

namespace tensorflow {

namespace {

Status ValidateTensorId(const tf2xla::TensorId& id) {
  if (id.node_name().empty()) {
    return errors::InvalidArgument("TensorId node_name must be non-empty");
  }
  if (id.output_index() < 0) {
    return errors::InvalidArgument("TensorId output_index must be positive");
  }
  return Status::OK();
}

Status CheckNameDuplicates(const string& kind, const string& name,
                           std::set<string>* names) {
  if (!name.empty()) {
    if (!names->insert(name).second) {
      return errors::InvalidArgument("duplicate ", kind, " name: ", name);
    }
  }
  return Status::OK();
}

Status CheckFeedFetchNameConflicts(const string& kind,
                                   const std::set<string>& names) {
  // We don't allow the feeds or fetches to contain both "foo" and "foo_data",
  // since that will cause a collision in codegen symbols.
  for (const string& name : names) {
    const string name_data(name + "_data");
    if (names.find(name_data) != names.end()) {
      return errors::InvalidArgument("conflicting ", kind, " name: ", name,
                                     " and ", name_data);
    }
  }
  return Status::OK();
}

}  // namespace

const char kXlaOutsideCompilationAttrName[] = "_xla_outside_compilation";

Status ValidateConfig(const tf2xla::Config& config) {
  std::set<string> names;
  for (const tf2xla::Feed& feed : config.feed()) {
    TF_RETURN_IF_ERROR(ValidateTensorId(feed.id()));
    TF_RETURN_IF_ERROR(TensorShape::IsValidShape(feed.shape()));
    TF_RETURN_IF_ERROR(CheckNameDuplicates("feed", feed.name(), &names));
  }
  TF_RETURN_IF_ERROR(CheckFeedFetchNameConflicts("feed", names));
  names.clear();
  for (const tf2xla::Fetch& fetch : config.fetch()) {
    TF_RETURN_IF_ERROR(ValidateTensorId(fetch.id()));
    TF_RETURN_IF_ERROR(CheckNameDuplicates("fetch", fetch.name(), &names));
  }
  TF_RETURN_IF_ERROR(CheckFeedFetchNameConflicts("fetch", names));
  if (config.fetch().empty()) {
    return errors::InvalidArgument("fetches must be specified");
  }
  return Status::OK();
}

Status AddPlaceholdersForFeeds(
    const tf2xla::Config& config, const OpRegistryInterface* op_registry,
    std::unordered_map<string, string>* feed_remapping, GraphDef* graph_def) {
  struct PlaceholderInfo {
    const tf2xla::Feed* feed = nullptr;  // point to Feed in <config>.
    string placeholder_name;
    DataType data_type = DT_INVALID;
  };

  // Put each fed tensor into a map by name:port. A map is used for determinism
  // when creating placeholders (genrules want deterministic output).
  std::map<string, PlaceholderInfo> placeholder_info;
  for (int i = 0; i < config.feed_size(); ++i) {
    const tf2xla::Feed* feed = &config.feed(i);
    const string name_port = TensorIdToString(feed->id());
    PlaceholderInfo& info = placeholder_info[name_port];
    info.feed = feed;
    info.placeholder_name = absl::StrCat("aot_feed_", feed->id().output_index(),
                                         "/", feed->id().node_name());
    (*feed_remapping)[name_port] = info.placeholder_name;
  }

  // Verify node exists and determine data type.
  std::unordered_map<string, const NodeDef*> name_to_node;
  for (int i = 0; i < graph_def->node_size(); ++i) {
    name_to_node[graph_def->node(i).name()] = &graph_def->node(i);
  }
  for (auto it = placeholder_info.begin(); it != placeholder_info.end(); ++it) {
    PlaceholderInfo& info = it->second;
    const tf2xla::TensorId& feed_id = info.feed->id();

    // Find the existing node and determine data type.
    auto node_it = name_to_node.find(feed_id.node_name());
    if (node_it == name_to_node.end()) {
      return errors::NotFound("Can't find feed node: ",
                              TensorIdToString(feed_id));
    }
    const NodeDef* existing = node_it->second;

    if (info.feed->type() != DT_INVALID) {
      info.data_type = info.feed->type();
    } else {
      // Build the node in order to infer its type.

      // Must first add default attrs as well, so do this in a copied GraphDef.
      GraphDef gd;
      *gd.mutable_versions() = graph_def->versions();
      *gd.add_node() = *existing;
      TF_RETURN_IF_ERROR(
          AddDefaultAttrsToGraphDef(&gd, *op_registry, 0 /*node_offset*/));

      // Now build the node from the copied node def.
      Graph g(op_registry);
      g.set_versions(graph_def->versions());
      Status status;
      Node* feed_node = g.AddNode(gd.node(0), &status);
      TF_RETURN_IF_ERROR(status);

      if (info.feed->id().output_index() < feed_node->num_outputs()) {
        info.data_type =
            BaseType(feed_node->output_type(info.feed->id().output_index()));
      } else {
        return errors::InvalidArgument(
            "Invalid output_index ", info.feed->id().output_index(),
            " for feed node ", info.feed->id().node_name());
      }
    }
  }

  // Create placeholders. Note that we could avoid creating a placeholder for
  // feeds which are already placeholders, but we omit that to avoid more cases
  // in this code.
  for (auto it = placeholder_info.begin(); it != placeholder_info.end(); ++it) {
    const PlaceholderInfo& info = it->second;
    NodeDef* d = graph_def->add_node();
    d->set_name(info.placeholder_name);
    d->set_op("PlaceholderV2");
    auto& attr_map = *d->mutable_attr();
    attr_map["dtype"].set_type(info.data_type);
    *attr_map["shape"].mutable_shape() = info.feed->shape();
  }

  // Rewrite references to the fed tensors to refer to the placeholder.
  for (int i = 0; i < graph_def->node_size(); ++i) {
    NodeDef* node_def = graph_def->mutable_node(i);
    for (int j = 0; j < node_def->input_size(); ++j) {
      auto id = ParseTensorName(node_def->input(j));
      auto it = placeholder_info.find(id.ToString());
      if (it != placeholder_info.end()) {
        node_def->set_input(j, it->second.placeholder_name);
      }
    }
  }

  return Status::OK();
}

Status PruneGraphDefInto(const tf2xla::Config& config, const GraphDef& in,
                         GraphDef* out) {
  *out = in;
  out->clear_node();

  // Tensors needed for feeding.
  std::set<std::pair<string, int>> feed_tensors;
  for (const tf2xla::Feed& feed : config.feed()) {
    feed_tensors.insert(
        std::make_pair(feed.id().node_name(), feed.id().output_index()));
  }

  // Maps node name to reachability.
  std::unordered_map<string, std::pair<bool, const NodeDef*>> node_by_name;
  for (const NodeDef& node : in.node()) {
    node_by_name[node.name()] = std::pair<bool, const NodeDef*>(false, &node);
  }

  // Traverse.
  std::queue<string> name_queue;
  for (int i = 0; i < config.fetch_size(); ++i) {
    name_queue.push(config.fetch(i).id().node_name());
  }
  while (!name_queue.empty()) {
    const string name = name_queue.front();
    name_queue.pop();

    auto find_it = node_by_name.find(name);
    if (find_it == node_by_name.end()) {
      return errors::InvalidArgument("While pruning graph, node ", name,
                                     " needed but not found in the graph.");
    }
    auto& map_entry = find_it->second;
    if (map_entry.first) {
      continue;
    }
    map_entry.first = true;

    // Push input nodes of the currently visited node to name_queue.
    for (const string& in_edge : map_entry.second->input()) {
      auto id = ParseTensorName(in_edge);
      const string node_name = string(id.first);
      if (feed_tensors.find(std::make_pair(node_name, id.second)) ==
          feed_tensors.end()) {
        name_queue.push(node_name);
      } else {
        // The input tensor is from an edge that is being fed. Therefore,
        // we skip recursing down that edge, to avoid requiring nodes that
        // may not be needed (note that the input node may still be added
        // to name_queue later if one of its output edges is not being fed).
      }
    }
  }

  // Copy over, preserving order of original and only nodes that are reachable
  // from the fetches.
  out->mutable_node()->Reserve(in.node_size());
  for (const NodeDef& node : in.node()) {
    if (node_by_name[node.name()].first) {
      *out->add_node() = node;
    }
  }
  return Status::OK();
}

string TensorIdToString(const tf2xla::TensorId& id) {
  return absl::StrCat(id.node_name(), ":", id.output_index());
}

Status SetNodeShardingFromNeighbors(Node* n, bool out_edges) {
  int core = -1;
  const Node* matching_node = nullptr;
  for (const Edge* edge : (out_edges ? n->out_edges() : n->in_edges())) {
    if (edge->IsControlEdge()) continue;
    const Node* possible_match = out_edges ? edge->dst() : edge->src();
    TF_ASSIGN_OR_RETURN(
        absl::optional<xla::OpSharding> sharding,
        ParseShardingFromDevice(
            *possible_match,
            /*num_cores_per_replica=*/std::numeric_limits<int32>::max()));
    if (sharding.has_value()) {
      TF_RET_CHECK(sharding.value().type() ==
                   xla::OpSharding::Type::OpSharding_Type_MAXIMAL);
      const int core_annotation = sharding.value().tile_assignment_devices(0);
      if (core == -1 || core > core_annotation) {
        core = core_annotation;
        matching_node = possible_match;
      }
    }
  }
  if (matching_node != nullptr) {
    n->set_assigned_device_name(matching_node->assigned_device_name());
    n->set_requested_device(matching_node->requested_device());
  }
  return Status::OK();
}

void AddDtypeToKernalDefConstraint(absl::string_view name, DataType dtype,
                                   KernelDef* kdef) {
  for (KernelDef::AttrConstraint& constraint : *kdef->mutable_constraint()) {
    if (constraint.name() == name) {
      constraint.mutable_allowed_values()->mutable_list()->add_type(dtype);
    }
  }
}

namespace {
uint32 InitialRandomSeed() {
  // Support plumbing the TF seed through to XLA is being worked on.
  // If a user wants deterministic behavior, their best option
  // is to start with a known checkpoint. This also handles issues when
  // multiple random calls can be invoked in any order by TF executor.
  // Another option is to use stateless random ops. They have much cleaner
  // semantics.
  // If a user really wants to set a deterministic seed for XLA-based
  // devices, this is the place to do it.
  std::random_device rd;
  // Make the starting value odd.
  return rd() | 1;
}
}  // namespace

uint32 GetXLARandomSeed() {
  // We initialize counter with an odd number and increment it by two
  // everytime. This ensures that it will never be zero, even
  // after an overflow. When seeded with zero, some XLA backends
  // can return all zeros instead of random numbers.
  static std::atomic<uint32> counter(InitialRandomSeed());
  return counter.fetch_add(2);
}

// TODO(b/77601805): add tests for associated function related stuff.
bool HasAssociatedFunction(const NodeDef& node_def,
                           FunctionLibraryRuntime* flr) {
  if (flr->GetFunctionLibraryDefinition()->Contains(node_def.op())) {
    return true;
  }

  if (node_def.op() == FunctionLibraryDefinition::kGradientOp) {
    // Skip gradient op. Gradient op has "f" attr, which is set to the function
    // we are getting gradient for. That function is not associated with the op.
    return false;
  }

  for (const auto& iter : node_def.attr()) {
    if (iter.second.has_func()) {
      return true;
    }
  }

  return false;
}

std::vector<AssociatedFunctionInfo> GetAssociatedFunctions(
    const Node& node, FunctionLibraryRuntime* flr) {
  std::vector<AssociatedFunctionInfo> results;
  const string& op = node.type_string();
  if (flr->GetFunctionLibraryDefinition()->Contains(op)) {
    // This is a function call node.
    AttrValueMap attrs(node.attrs().begin(), node.attrs().end());
    results.emplace_back(AssociatedFunctionInfo(op, attrs));
  } else if (node.type_string() == FunctionLibraryDefinition::kGradientOp) {
    // Skip gradient op. Gradient op has "f" attr, which is set to the function
    // we are getting gradient for. That function is not associated with the op.
  } else {
    // Collect all function attrs for the node.
    for (auto& iter : node.attrs()) {
      if (iter.second.has_func()) {
        VLOG(2) << "Found function attr for node " << node.name() << ": "
                << iter.first << " = " << iter.second.func().name();
        results.emplace_back(AssociatedFunctionInfo(
            iter.second.func().name(), iter.second.func().attr(), iter.first));
      }
    }
  }
  return results;
}

Status RewriteAssociatedFunction(
    Graph* graph, Node* node, FunctionLibraryDefinition* fld,
    const AssociatedFunctionInfo& associated_function,
    const string& rewritten_function_name) {
  switch (associated_function.type()) {
    case AssociatedFunctionInfo::kFunctionCallNode: {
      // Change this node to call the new function.
      NodeDefBuilder builder(node->name(), rewritten_function_name, fld);
      for (auto attr : node->attrs()) {
        builder.Attr(attr.first, attr.second);
      }
      for (int i = 0; i < node->num_inputs(); i++) {
        Node* input_node;
        TF_RETURN_IF_ERROR(node->input_node(i, &input_node));
        builder.Input(input_node->name(), i, node->input_type(i));
      }
      builder.Device(node->assigned_device_name().empty()
                         ? node->requested_device()
                         : node->assigned_device_name());
      NodeDef node_def;
      TF_RETURN_IF_ERROR(builder.Finalize(&node_def));
      Status s;
      Node* new_node = graph->AddNode(node_def, &s);
      TF_RETURN_IF_ERROR(s);
      for (auto edge : node->in_edges()) {
        graph->AddEdge(edge->src(), edge->src_output(), new_node,
                       edge->dst_input());
      }
      for (auto edge : node->out_edges()) {
        graph->AddEdge(new_node, edge->src_output(), edge->dst(),
                       edge->dst_input());
      }
      graph->RemoveNode(node);
      break;
    }
    case AssociatedFunctionInfo::kFunctionAttr: {
      // Change function attr to rewritten functions.
      NameAttrList func;
      TF_RETURN_IF_ERROR(
          GetNodeAttr(node->attrs(), associated_function.attr_name(), &func));
      node->ClearAttr(associated_function.attr_name());
      func.set_name(rewritten_function_name);
      node->AddAttr(associated_function.attr_name(), func);
      break;
    }
  }

  return Status::OK();
}

}  // namespace tensorflow
