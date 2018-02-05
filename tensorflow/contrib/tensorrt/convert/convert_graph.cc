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

#include "tensorflow/contrib/tensorrt/convert/convert_graph.h"

#include <list>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <map>
#include <utility>

#include "NvInfer.h"

#include "tensorflow/contrib/tensorrt/convert/convert_nodes.h"
#include "tensorflow/contrib/tensorrt/segment/segment.h"
#include "tensorflow/core/framework/graph.pb.h"
#include "tensorflow/core/framework/node_def.pb.h"
#include "tensorflow/core/graph/algorithm.h"
#include "tensorflow/core/graph/graph.h"
#include "tensorflow/core/graph/graph_constructor.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/logging.h"

#define _TF_LOG_DEBUG ::tensorflow::internal::LogMessage(__FILE__, __LINE__, -1)
#include "tensorflow/core/grappler/optimizers/constant_folding.h"
#include "tensorflow/core/grappler/optimizers/layout_optimizer.h"
#include "tensorflow/core/grappler/devices.h"
#include "tensorflow/core/grappler/clusters/virtual_cluster.h"
#include "tensorflow/core/protobuf/device_properties.pb.h"
#include "tensorflow/core/grappler/grappler_item.h"
#include "tensorflow/core/grappler/utils.h"

#include "tensorflow/core/grappler/costs/graph_properties.h"

//------------------------------------------------------------------------------
namespace tensorrt {
namespace convert {

namespace {

static std::unordered_set<std::string> output_nodes;
bool IsTensorRTCandidate(const tensorflow::NodeDef& node_def) {
  static const std::set<std::string> candidate_ops = {
      "Identity", "Const", "Conv2D", "MaxPool", "BiasAdd", "Relu",
      "Add",      "Mul",   "Sub",    "Rsqrt",   "Pad"    , "Mean"
                                                       // TODO(ben,jie): ...
  };
  if (output_nodes.count(node_def.name())) return false;
  return candidate_ops.count(node_def.op());
}

void GetSubGraphIncomingEdges(tensorflow::Graph const& graph,
                              std::set<int> const& subgraph_node_ids,
                              tensorflow::EdgeSet* incoming_edges) {
  for (int node_id : subgraph_node_ids) {
    tensorflow::Node const* node = graph.FindNodeId(node_id);
    LOG(DEBUG) << node->name() << " has incoming edges: ";
    for (tensorflow::Edge const* edge : node->in_edges()) {
      if (!subgraph_node_ids.count(edge->src()->id()) &&
          !edge->src()->IsSource()) {
        LOG(DEBUG) << edge->src()->name() << ", ";
        incoming_edges->insert(edge);
      }
    }
  }
}

void GetSubGraphOutgoingEdges(tensorflow::Graph const& graph,
                              std::set<int> const& subgraph_node_ids,
                              tensorflow::EdgeSet* outgoing_edges) {
  for (int node_id : subgraph_node_ids) {
    tensorflow::Node const* node = graph.FindNodeId(node_id);
    LOG(DEBUG) << node->name() << " has outgoing edges: ";
    for (tensorflow::Edge const* edge : node->out_edges()) {
      if (!subgraph_node_ids.count(edge->dst()->id()) &&
          !edge->dst()->IsSink()) {
        outgoing_edges->insert(edge);
      }
    }
  }
}

std::pair<std::string, int> ParseTensorName(std::string name,
                                            int default_idx = 0) {
  int idx = default_idx;
  size_t sep = name.find_last_of(':');
  if (sep != std::string::npos) {
    name = name.substr(0, sep);
    idx = std::stoi(name.substr(sep + 1));
  }
  return std::make_pair(name, idx);
}

std::unordered_map<std::string, std::vector<int>> BuildTensorNameMap(
    const std::vector<std::string>& tensor_names) {
  std::unordered_map<std::string, std::vector<int>> result;
  for (std::string const& tensor_name : tensor_names) {
    std::string node_name;
    int index;
    std::tie(node_name, index) = ParseTensorName(tensor_name);
    result[node_name].push_back(index);
  }
  return result;
}

struct ConvertGraphParams{
  ConvertGraphParams(tensorflow::Graph &graph_,
                     const std::vector<std::string> &output_names_,
                     const std::set<int>& subgraph_node_ids_,
                     size_t max_batch_size_,
                     size_t max_workspace_size_,
                     const tensorflow::grappler::GraphProperties &graph_properties_,
                     bool int8_
  ):graph(graph_),output_names(output_names_),subgraph_node_ids(subgraph_node_ids_),
    max_batch_size(max_batch_size_),max_workspace_size(max_workspace_size_),
    graph_properties(graph_properties_),int8(int8_){

  }

  tensorflow::Graph& graph;
  const std::vector<std::string>& output_names;
  const std::set<int>& subgraph_node_ids;
  size_t max_batch_size;
  size_t max_workspace_size;
  const tensorflow::grappler::GraphProperties& graph_properties;
  bool int8;
  std::vector<std::pair<int,int>> subgraph_inputs;
  std::vector<std::pair<int,int>> subgraph_outputs;
  tensorflow::EdgeSet subgraph_incoming_edges;
  tensorflow::EdgeSet subgraph_outgoing_edges;
};

tensorflow::Status FillSubGraphEdgeSets(ConvertGraphParams &p){

  GetSubGraphIncomingEdges(p.graph, p.subgraph_node_ids, &p.subgraph_incoming_edges);
  for (tensorflow::Edge const* edge : p.subgraph_incoming_edges) {
    p.subgraph_inputs.push_back({edge->src()->id(), edge->src_output()});
  }
  auto output_name_to_index_map = BuildTensorNameMap(p.output_names);
  std::set<std::pair<int, int>> subgraph_outputs_set;

  for (int node_id : p.subgraph_node_ids) {
    tensorflow::Node* node = p.graph.FindNodeId(node_id);
    if (output_name_to_index_map.count(node->name())) {
      for (int index : output_name_to_index_map.at(node->name())) {
        subgraph_outputs_set.insert({node_id, index});
      }
    }
  }

  GetSubGraphOutgoingEdges(p.graph, p.subgraph_node_ids, &p.subgraph_outgoing_edges);
  for (tensorflow::Edge const* edge : p.subgraph_outgoing_edges) {
    subgraph_outputs_set.insert({edge->src()->id(), edge->src_output()});
  }
  p.subgraph_outputs.reserve(subgraph_outputs_set.size());
  p.subgraph_outputs.insert(p.subgraph_outputs.begin(),
      subgraph_outputs_set.begin(), subgraph_outputs_set.end());
  return tensorflow::Status::OK();

};

tensorflow::Status GetCalibNode(ConvertGraphParams *params){

  FillSubGraphEdgeSets(*params);
  tensorflow::NodeDef trt_node_def;

  SubGraphParams s(params->graph, params->subgraph_node_ids, params->subgraph_inputs, params->subgraph_outputs,
                   params->max_batch_size, params->max_workspace_size, params->graph_properties, &trt_node_def);
  TF_RETURN_IF_ERROR(InjectCalibrationNode(s));
  tensorflow::Status status;
  tensorflow::Node* trt_node = params->graph.AddNode(trt_node_def, &status);

  TF_RETURN_IF_ERROR(status);

  for (auto inp_port: params->subgraph_inputs) {  // loop over incoming edges and attach them to calib node
    tensorflow::Node * in_node =params->graph.FindNodeId(inp_port.first);
    params->graph.UpdateEdge(trt_node, inp_port.second, in_node, inp_port.second);
  }
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertSubGraphToTensorRT(ConvertGraphParams* params ) {

//  tensorflow::EdgeSet subgraph_incoming_edges;
//
//  std::vector<std::pair<int, int>> subgraph_inputs;
//
//
//  // Collect inputs by looking for incoming edges
//  for (tensorflow::Edge const* edge : subgraph_incoming_edges) {
//    subgraph_inputs.push_back({edge->src()->id(), edge->src_output()});
//  }
//  std::set<std::pair<int, int>> subgraph_outputs_set;
//  // Collect outputs referenced from output_names
//  auto output_name_to_index_map = BuildTensorNameMap(output_names);
//  for (int node_id : subgraph_node_ids) {
//    tensorflow::Node* node = graph.FindNodeId(node_id);
//    if (output_name_to_index_map.count(node->name())) {
//      for (int index : output_name_to_index_map.at(node->name())) {
//        subgraph_outputs_set.insert({node_id, index});
//      }
//    }
//  }
//  // Collect outputs referenced from outgoing edges
//  tensorflow::EdgeSet subgraph_outgoing_edges;
//  // GetSubGraphOutgoingEdges(graph, subgraph_node_ids_no_placeholder,
//  //  &subgraph_outgoing_edges);
//  GetSubGraphOutgoingEdges(graph, subgraph_node_ids, &subgraph_outgoing_edges);
//  for (tensorflow::Edge const* edge : subgraph_outgoing_edges) {
//    subgraph_outputs_set.insert({edge->src()->id(), edge->src_output()});
//  }
//  // Impose an ordering on the outputs
//  std::vector<std::pair<int, int>> subgraph_outputs(
//      subgraph_outputs_set.begin(), subgraph_outputs_set.end());
//  // Build TensorRT node and add it to the graph
  FillSubGraphEdgeSets(*params);
  tensorflow::NodeDef trt_node_def;

  SubGraphParams s(params->graph, params->subgraph_node_ids, params->subgraph_inputs, params->subgraph_outputs,
                   params->max_batch_size, params->max_workspace_size, params->graph_properties, &trt_node_def);
  TF_RETURN_IF_ERROR(ConvertSubGraphToTensorRTNodeDef(s));
  tensorflow::Status status;
  tensorflow::Node* trt_node = params->graph.AddNode(trt_node_def, &status);

  TF_RETURN_IF_ERROR(status);

  // Re-map outgoing edges to use the new TRT node instead of the orig subgraph
  std::map<std::pair<int, int>, int> subgraph_edge_to_output_map;
  for (size_t i = 0; i < params->subgraph_outputs.size(); ++i) {
    subgraph_edge_to_output_map.insert({params->subgraph_outputs.at(i), i});
  }
  TF_RETURN_IF_ERROR(status);
  for (tensorflow::Edge const* edge : params->subgraph_outgoing_edges) {
    std::pair<int, int> old_src = {edge->src()->id(), edge->src_output()};
    int new_src_output = subgraph_edge_to_output_map.at(old_src);
    params->graph.UpdateEdge(trt_node, new_src_output, edge->dst(), edge->dst_input());
  }
  // Remove the original subgraph
  for (int node_id : params->subgraph_node_ids) {
    tensorflow::Node* node = params->graph.FindNodeId(node_id);
    // Don't remove the input placeholders
    if (node->type_string() == "Placeholder") {
      continue;
    }
    params->graph.RemoveNode(node);
  }
  return tensorflow::Status::OK();
}

tensorflow::Status BuildNodeMap(
    const tensorflow::Graph& graph,
    std::unordered_map<std::string, tensorflow::Node*>* node_map) {
  for (auto* node : graph.op_nodes()) {
    if (!node_map->insert({node->name(), node}).second) {
      return tensorflow::errors::AlreadyExists(
          "Node name is not unique in graph: " + node->name());
    }
  }
  return tensorflow::Status::OK();
}

}  // namespace

tensorflow::Status ConvertGraphDefToTensorRT(
    const tensorflow::GraphDef& graph_def,
    const std::vector<std::string>& output_names, size_t max_batch_size,
    size_t max_workspace_size,
    tensorflow::GraphDef* new_graph_def,
    bool int8=false) {

  // optimization pass
  tensorflow::grappler::GrapplerItem item;
  item.fetch = output_names;
  tensorflow::GraphDef gdef;

  // layout optimization
  item.graph = graph_def;
  tensorflow::grappler::LayoutOptimizer optimizer;
  tensorflow::grappler::Cluster* gCluster;

  // virtual cluster
  tensorflow::DeviceProperties device_properties;
  device_properties.set_type("GPU");
  device_properties.mutable_environment()->insert({"architecture", "6"});
  gCluster =
    new tensorflow::grappler::VirtualCluster({{"/GPU:0", device_properties}});

  // single machine
  int num_cpu_cores = tensorflow::grappler::GetNumAvailableLogicalCPUCores();
  int num_gpus = tensorflow::grappler::GetNumAvailableGPUs();
  LOG(DEBUG) << "cpu_cores: " << num_cpu_cores;
  LOG(DEBUG) << "gpus: " << num_gpus;
  // int timeout_s = 60 * 10;
  // gCluster = new tensorflow::grappler::SingleMachine(
  //                  timeout_s, num_cpu_cores, num_gpus);

  tensorflow::Status status = optimizer.Optimize(gCluster, item, &gdef);

  if (status !=tensorflow::Status::OK())
    return status;
 
  // constant folding
  item.graph = gdef;
  tensorflow::grappler::ConstantFolding fold(nullptr);
  status = fold.Optimize(nullptr, item, &gdef);
  if (status !=tensorflow::Status::OK()) {
    return status;
  }
  // AJ refactoring shape inference through grappler/GraphProperties.
  tensorflow::grappler::GraphProperties static_graph_properties(item);
  static_graph_properties.InferStatically(false);
  // TF_CHECK_OK(static_graph_prop.InferStatically(false));
  // ShapeMap shape_map;
  // TF_RETURN_IF_ERROR(
  //     tensorflow::trt::inferShapes(gdef, output_names, shape_map));
  // std::stringstream oss;
  // for (auto& n : shape_map) {  // nodes
  //   oss << " Node= " << n.first << ", ";
  //   for (auto o : n.second) {  // outputs
  //     oss << o.first.DebugString() << " T= " << o.second << ", ";
  //   }
  //   LOG(DEBUG) << oss.str();
  //   oss.str("");
  // }

  // Build full graph
  tensorflow::FunctionLibraryDefinition flib(tensorflow::OpRegistry::Global(),
                                             gdef.library());
  tensorflow::Graph graph(flib);
  TF_RETURN_IF_ERROR(tensorflow::ConvertGraphDefToGraph(
      tensorflow::GraphConstructorOptions(), gdef, &graph));

  // Segment the graph into subgraphs that can be converted to TensorRT
  tensorrt::segment::SegmentOptions segment_options;
  // TODO(ben,jie,sami): exclude output nodes (DISCUSS IT)
  for (auto node : output_names) output_nodes.insert(node);

  // TODO(sami): this should be passed as a knob!!!!
  segment_options.minimum_segment_size = 2;
  tensorrt::segment::SegmentNodesVector segments;
  TF_RETURN_IF_ERROR(tensorrt::segment::SegmentGraph(
      gdef, IsTensorRTCandidate, segment_options, &segments));
  if (segments.size() > 1) {
    // LOG(WARNING) << "Multiple TensorRT candidate subgraphs were found, "
    //<< "but only the first can be converted.";
    // segments.erase(++segments.begin(), segments.end());
    LOG(INFO) << "MULTIPLE tensorrt candidate conversion: " << segments.size();
  }
  std::unordered_map<std::string, tensorflow::Node*> node_map;
  TF_RETURN_IF_ERROR(BuildNodeMap(graph, &node_map));
  for (std::set<std::string> const& subgraph_node_names : segments) {
    std::set<int> subgraph_node_ids;
    for (std::string const& node_name : subgraph_node_names) {
      subgraph_node_ids.insert(node_map.at(node_name)->id());
    }

    ConvertGraphParams p(graph,output_names,subgraph_node_ids,max_batch_size,max_workspace_size,
                         static_graph_properties,int8);
    if(int8) {
      TF_RETURN_IF_ERROR(GetCalibNode(&p));
    } else{
      TF_RETURN_IF_ERROR(ConvertSubGraphToTensorRT(&p));
    }
  }
  graph.ToGraphDef(new_graph_def);
  return tensorflow::Status::OK();
}

}  // namespace convert
}  // namespace tensorrt
