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

#include "tensorflow/core/grappler/optimizers/data/vectorization_utils.h"
#include "tensorflow/core/grappler/optimizers/data/vectorization/vectorizer_registry.h"

#include "absl/strings/str_join.h"
#include "tensorflow/cc/framework/ops.h"
#include "tensorflow/core/common_runtime/function.h"
#include "tensorflow/core/framework/attr_value.pb.h"
#include "tensorflow/core/framework/device_base.h"
#include "tensorflow/core/framework/function.h"
#include "tensorflow/core/framework/graph_to_functiondef.h"
#include "tensorflow/core/framework/node_def.pb.h"
#include "tensorflow/core/framework/node_def_builder.h"
#include "tensorflow/core/framework/node_def_util.h"
#include "tensorflow/core/framework/op_def.pb.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/graph/node_builder.h"
#include "tensorflow/core/grappler/mutable_graph_view.h"
#include "tensorflow/core/grappler/optimizers/data/function_utils.h"
#include "tensorflow/core/grappler/optimizers/data/graph_utils.h"
#include "tensorflow/core/grappler/utils.h"
#include "tensorflow/core/grappler/utils/functions.h"
#include "tensorflow/core/lib/gtl/map_util.h"

namespace tensorflow {
namespace grappler {
namespace vectorization_utils {

namespace {

// Describes a tensor with its operation Node and output position
typedef std::pair<Node*, int> TensorDesc;

const char* const kRetValOp = "_Retval";

void ReplaceEdgeSources(const TensorDesc& old_src, const TensorDesc& new_src,
                        Graph* graph) {
  // NOTE: We need two for loops here because we can't mutate the set of output
  // edges as we iterate over them.
  std::vector<const Edge*> edges_to_replace;
  for (auto edge : old_src.first->out_edges()) {
    if (edge->src_output() == old_src.second) {
      edges_to_replace.push_back(edge);
    }
  }
  for (auto edge : edges_to_replace) {
    graph->AddEdge(new_src.first, new_src.second, edge->dst(),
                   edge->dst_input());
    graph->RemoveEdge(edge);
  }
}

Status AddMapDefunOutput(FunctionBody* map_defun_fn, Node* map_defun_node,
                         const TensorDesc& output) {
  // Note that we don't update MapDefun attrs as we go, only when we are done
  DataType type = output.first->output_type(output.second);
  int index = map_defun_fn->ret_nodes.size();

  NodeDef ret_node_def;
  ret_node_def.set_name("map_out");
  ret_node_def.set_op(kRetValOp);
  AddNodeAttr("T", type, &ret_node_def);
  AddNodeAttr("index", index, &ret_node_def);

  Status s;
  Node* ret_node = map_defun_fn->graph->AddNode(ret_node_def, &s);
  TF_RETURN_IF_ERROR(s);

  map_defun_fn->graph->AddEdge(output.first, output.second, ret_node, 0);
  map_defun_fn->ret_nodes.push_back(ret_node);
  map_defun_fn->ret_types.push_back(type);

  return s;
}

void RemoveMapDefunOutput(int output_position, Graph* outer_scope,
                          FunctionBody* map_defun_fn, Node* map_defun_node) {
  // Note that we don't update MapDefun attrs as we go, only when we are done
  DCHECK_LT(output_position, map_defun_fn->ret_nodes.size())
      << "Trying to remove output that doesn't exist. Output number: "
      << output_position;

  int num_later_outputs = map_defun_fn->ret_nodes.size() - output_position - 1;

  // Modify map_defun_fn's signature and remove the output node from its graph
  map_defun_fn->graph->RemoveNode(map_defun_fn->ret_nodes[output_position]);
  map_defun_fn->ret_nodes.erase(map_defun_fn->ret_nodes.begin() +
                                output_position);
  map_defun_fn->ret_types.erase(map_defun_fn->ret_types.begin() +
                                output_position);

  // Renumber the nodes and edges that come after
  for (int i = 0; i < num_later_outputs; ++i) {
    ReplaceEdgeSources({map_defun_node, output_position + i + 1},
                       {map_defun_node, output_position + i}, outer_scope);
    // Each ret node has an "index" attr that has to be updated
    map_defun_fn->ret_nodes[output_position + i]->AddAttr("index",
                                                          output_position + i);
  }
}

// Helper class that vectorizes the body of a MapDefun node, adding new
// operations to the graph that collectively compute the same value as what
// running the MapDefun function on slices of the input would produce.
// This class transforms the input FunctionDefs into their corresponding
// Graph objects and works on the graphs directly, then converts them back
// to FunctionDefs when GetResult is called.
class Vectorization {
 public:
  explicit Vectorization(FunctionDefLibrary* lib)
      : lib_(lib), lib_def_(OpRegistry::Global(), *lib) {}

  // Adds the vectorized function and new map_defun_fn to lib, and points
  // vectorized_function to the former. Returns an error status if
  // the conversion between FunctionDef -> Graph -> FunctionDef failed anywhere
  // along the way.
  Status Vectorize(const FunctionDef& outer_scope,
                   const NodeDef& map_defun_node, FunctionDef** result);

 private:
  // Converts FunctionDefs to Graphs and adds mappings from
  // arg nodes and unstacked nodes to the corresponding nodes in outer_scope_.
  Status Initialize(const FunctionDef& outer_scope,
                    const NodeDef& map_defun_node);

  // Converts Graphs back to FunctionDefs and adds them to `lib_`.
  Status GetResult(FunctionDef** vectorized_function);

  // Repeatedly tries to convert outputs of `map_defun_fn_` into new nodes in
  // `outer_scope_`, until there are no convertible outputs remaining.
  void VectorizeHelper();

  // Vectorizes map_defun_fn's output at output_position.
  Status ConvertOutput(int output_position);

  // Adds mappings from node's outputs tensors to converted output tensors,
  // creating the necessary new node(s). Generally, the steps to convert an op
  // are:
  // 1) Create new node(s) in `outer_scope_` that act on batched input tensors.
  //    These operations collectively compute the same value as what running
  //    the original operation on slices of the input tensors would produce.
  //    For example, a Cast op in MapDefun translates to a Cast op in
  //    `outer_scope_`, since the vectorized version of Cast is itself.
  // 2) Promote the inputs of the op inputs to outputs of the
  //    `map_defun_node_` and `map_defun_fn_`.
  // 3) Add edges between the promoted inputs (that are now outputs of
  //    `map_defun_node`) and the inputs ports of the new node(s).
  // 4) For each output of the old node, add the mapping of output tensors to
  //    the conversion map.
  Status AddConversionMapping(Node* op_node);

  // Given a tensor t in `unstacked`, stacks it by doing the equivalent of
  // tf.tile(tf.expand_dims(t, 0), [n, 1, 1, ...]) where n is dimension 0 of
  // inputs to `map_defun_node_`. This stacked tensor will be compatible with
  // the expected output shape of `map_defun_node_`.
  // This is equivalent to the _stack function in python Pfor.
  Status StackTensor(WrappedTensor* unstacked, TensorDesc* result);

  // Recursively looks for unstacked nodes in the `map_defun_fn_` graph by
  // doing a depth-first search from the ret nodes. Lifts nodes that are
  // unstacked (i.e. don't derive from arg nodes) into `outer_scope_` directly
  // and add mappings to `conversion_map_`.
  Status AddUnstackedNodeMappings();

  // Recursive helper for `AddUnstackedNodeMappings`, returns true if tensor
  // is unstacked.
  bool AddUnstackedNodeMappingsHelper(TensorDesc&& tensor, Status* status);

  // Add mappings from `map_defun_fn_` arg nodes to `map_defun_node_` input
  // nodes to `conversion_map_`.
  Status AddArgNodeMappings();

  // Maps a tensor to the corresponding WrappedTensor. For example,
  // {"Cast" Node*, 0} -> WrappedTensor({"Vectorize/Cast" Node*, 0}, true)
  std::map<TensorDesc, WrappedTensor> conversion_map_;

  // Unconvertible ret nodes
  std::set<Node*> unconvertible_;

  FunctionDefLibrary* lib_;  // Not owned
  FunctionLibraryDefinition lib_def_;
  // Note that FunctionBody has a pointer to a Graph object that corresponds
  // to the function's subgraph, with additional kArgOp and kRetValOp nodes
  // that denote that function arguments and return values. These nodes have the
  // attrs "T" for the type, and "index" for the argument / retval index
  // respectively. FunctionBody also keeps track of arg/ret_nodes and
  // arg/ret_types, that should be ordered according to argument/output indices.
  std::unique_ptr<Graph> outer_scope_;
  std::unique_ptr<FunctionBody> map_defun_fn_;
  Node* map_defun_node_ = nullptr;  // Owned by `outer_scope`

  // Caches the loop_len_node_ needed for tiling unstacked output. This
  // corresponds to a vector with one element.
  Node* loop_len_node_ = nullptr;  // Owned by `outer_scope`
  Status status_;
};

Status Vectorization::AddConversionMapping(Node* op_node) {
  for (auto edge : op_node->in_edges()) {
    if (edge->IsControlEdge()) {
      return errors::InvalidArgument(
          "Vectorizing outputs with control inputs is currently not "
          "supported.");
    }
  }

  auto vectorizer = VectorizerRegistry::Global()->Get(op_node->type_string());
  if (vectorizer == nullptr) {
    return errors::Unimplemented("No vectorizer registered for op: ",
                                 op_node->type_string());
  }
  std::vector<WrappedTensor> inputs, outputs;
  inputs.reserve(op_node->num_inputs());
  outputs.reserve(op_node->num_outputs());

  std::vector<const Edge*> input_edges;
  TF_RETURN_IF_ERROR(op_node->input_edges(&input_edges));

  // The inputs for the node to be converted may already have been converted
  // themselves. For those that are not, we promote them to MapDefun outputs.
  for (size_t i = 0; i < op_node->num_inputs(); ++i) {
    auto edge = input_edges[i];
    if (auto found = gtl::FindOrNull(conversion_map_,
                                     {edge->src(), edge->src_output()})) {
      inputs.push_back(*found);
    } else {
      // TODO(rachelim): Handle the case where unconverted inputs are unstacked.
      // We assume that all unconverted inputs will be stacked, since we
      // converted all unstacked nodes in `Initialize`. However, it's actually
      // possible that yet-unconverted nodes may produce unstacked outputs after
      // they are vectorized. (For example, see the "Shape" converter in
      // tensorflow/python/ops/parallel_for/pfor.py). If a vectorizer expects
      // an unstacked input but receives a stacked one, vectorizer->Vectorize
      // will return an error.
      TF_RETURN_IF_ERROR(AddMapDefunOutput(map_defun_fn_.get(), map_defun_node_,
                                           {edge->src(), edge->src_output()}));
      int output_index = map_defun_fn_->ret_nodes.size() - 1;
      inputs.push_back({map_defun_node_, output_index, true});
    }
  }

  TF_RETURN_IF_ERROR(vectorizer->Vectorize(*op_node, outer_scope_.get(),
                                           std::move(inputs), &outputs));

  if (op_node->num_outputs() != outputs.size()) {
    return errors::Internal(
        "Number of vectorizer outputs does not match. Expected: ",
        op_node->num_outputs(), " Actual: ", outputs.size());
  }

  // Add output mappings.
  for (size_t i = 0; i < op_node->num_outputs(); ++i) {
    conversion_map_.insert({{op_node, i}, outputs[i]});
  }

  return Status::OK();
}

Status Vectorization::ConvertOutput(int output_position) {
  // ret_edge->src() is the actual op that generated the retval, and
  // ret_edge->dst() is the retval node whose op is "_Retval"
  const Edge* ret_edge;
  TF_RETURN_IF_ERROR(
      map_defun_fn_->ret_nodes[output_position]->input_edge(0, &ret_edge));

  TensorDesc output({ret_edge->src(), ret_edge->src_output()});
  TensorDesc converted_output;

  // It's possible the output already has a mapping, if it comes from a node
  // that has already been converted.
  auto found = gtl::FindOrNull(conversion_map_, output);
  if (!found) {
    TF_RETURN_IF_ERROR(AddConversionMapping(output.first));
    found = &conversion_map_.at(output);
  }

  if (found->stacked) {
    converted_output = {found->node, found->output_index};
  } else {
    // Some outputs may be unstacked if they don't derive from arg nodes
    // (for example, if a function returns a constant). For these, we
    // have to add extra nodes to tile it in the 0th dimension.
    TF_RETURN_IF_ERROR(StackTensor(found, &converted_output));
  }

  ReplaceEdgeSources({map_defun_node_, output_position}, converted_output,
                     outer_scope_.get());
  RemoveMapDefunOutput(output_position, outer_scope_.get(), map_defun_fn_.get(),
                       map_defun_node_);

  return Status::OK();
}

Status Vectorization::Vectorize(const FunctionDef& outer_scope,
                                const NodeDef& map_defun_node,
                                FunctionDef** result) {
  TF_RETURN_IF_ERROR(Initialize(outer_scope, map_defun_node));
  VectorizeHelper();
  return GetResult(result);
}

void Vectorization::VectorizeHelper() {
  while (true) {
    int output_position = graph_utils::GetFirstElementIndexWithPredicate(
        [this](Node* n) {
          return this->unconvertible_.find(n) == this->unconvertible_.end();
        },
        map_defun_fn_->ret_nodes);

    // No outputs left to convert
    if (output_position == -1) break;

    Status s = ConvertOutput(output_position);
    if (!s.ok()) {
      Node* output_node = map_defun_fn_->ret_nodes.at(output_position);
      VLOG(2) << "Could not convert the output at node: "
              << output_node->DebugString() << "\nError: " << s;
      unconvertible_.insert(output_node);
    }
  }

  // If we've converted all the outputs of the MapDefun function, we no longer
  // need the MapDefun node and can delete it.
  if (map_defun_fn_->ret_nodes.empty()) {
    outer_scope_->RemoveNode(map_defun_node_);
  } else {
    // Update MapDefun node attrs accordingly
    DCHECK_EQ(map_defun_fn_->ret_types.size(), map_defun_fn_->ret_nodes.size());
    map_defun_node_->AddAttr(
        "output_shapes",
        std::vector<PartialTensorShape>(map_defun_fn_->ret_types.size()));
    map_defun_node_->AddAttr("output_types", map_defun_fn_->ret_types);
  }
}

Status Vectorization::Initialize(const FunctionDef& outer_scope,
                                 const NodeDef& map_defun_node) {
  // Convert outer_scope and map_defun_fn to FunctionBodys so we can
  // work on Graphs directly.
  const FunctionDef* map_defun_fn =
      lib_def_.Find(map_defun_node.attr().at("f").func().name());

  if (map_defun_fn == nullptr) {
    return errors::NotFound("Could not find function with name ",
                            map_defun_node.attr().at("f").func().name(),
                            " in function library.");
  }

  auto get_func_sig = [this](const string& op, const OpDef** sig) {
    return this->lib_def_.LookUpOpDef(op, sig);
  };

  FunctionBody* outer_fn;
  TF_RETURN_IF_ERROR(FunctionDefToBodyHelper(outer_scope, {}, &lib_def_,
                                             get_func_sig, &outer_fn));
  // We don't need outer_fn, just the graph
  outer_scope_.reset(outer_fn->graph);
  outer_fn->graph = nullptr;
  delete outer_fn;

  FunctionBody* tmp;
  TF_RETURN_IF_ERROR(FunctionDefToBodyHelper(*map_defun_fn, {}, &lib_def_,
                                             get_func_sig, &tmp));
  map_defun_fn_.reset(tmp);

  // Find the MapDefun node in outer_scope_
  int node_id = graph_utils::GetFirstElementIndexWithPredicate(
      [&map_defun_node](Node* n) { return n->name() == map_defun_node.name(); },
      outer_scope_->nodes());
  if (node_id == -1) {
    return errors::NotFound("Could not find node with name ",
                            map_defun_node.name(), " in outer_scope.");
  }
  map_defun_node_ = outer_scope_->FindNodeId(node_id);

  TF_RETURN_IF_ERROR(AddArgNodeMappings());

  TF_RETURN_IF_ERROR(AddUnstackedNodeMappings());
  loop_len_node_ = nullptr;

  return Status::OK();
}

// TODO(rachelim): It might be profitable to use the C++ API for this instead of
// NodeBuilder
Status Vectorization::StackTensor(WrappedTensor* unstacked,
                                  TensorDesc* result) {
  // Note that all these nodes are necessary as the size of the batch may not be
  // constant.
  if (unstacked->stacked) {
    return errors::Internal("Can only stack unstacked tensor.");
  }

  Graph* g = outer_scope_.get();
  auto node_builder = [](StringPiece op) {
    return NodeBuilder(strings::StrCat("vectorized/stack/", op), op);
  };

  auto make_const = [&node_builder](const Input::Initializer& val, Graph* graph,
                                    Node** result) {
    TF_RETURN_IF_ERROR(val.status);
    return node_builder("Const")
        .Attr("value", val.tensor)
        .Attr("dtype", val.tensor.dtype())
        .Finalize(graph, result);
  };

  // If loop_len_node_ hasn't been created yet, add the node and cache it.
  if (loop_len_node_ == nullptr) {
    Node* input_node;
    TF_RETURN_IF_ERROR(map_defun_node_->input_node(0, &input_node));

    Node* shape_node;
    TF_RETURN_IF_ERROR(
        node_builder("Shape").Input(input_node).Finalize(g, &shape_node));

    Node* const_vec_0;
    TF_RETURN_IF_ERROR(make_const({0}, g, &const_vec_0));
    Node* const_vec_1;
    TF_RETURN_IF_ERROR(make_const({1}, g, &const_vec_1));

    Node* strided_slice_node;
    TF_RETURN_IF_ERROR(node_builder("StridedSlice")
                           .Input(shape_node)   // input
                           .Input(const_vec_0)  // begin
                           .Input(const_vec_1)  // end
                           .Input(const_vec_1)  // strides
                           .Finalize(g, &strided_slice_node));

    // Produces a vector of length 1
    TF_RETURN_IF_ERROR(node_builder("Reshape")
                           .Input(strided_slice_node)  // tensor
                           .Input(const_vec_1)         // shape
                           .Finalize(g, &loop_len_node_));
  }

  Node* ones_shape;
  TF_RETURN_IF_ERROR(node_builder("Shape")
                         .Input(unstacked->node)  // input
                         .Finalize(g, &ones_shape));

  Node* ones;
  TF_RETURN_IF_ERROR(
      node_builder("OnesLike").Input(ones_shape).Finalize(g, &ones));

  Node* const_0;
  TF_RETURN_IF_ERROR(make_const(0, g, &const_0));

  Node* multiples;
  TF_RETURN_IF_ERROR(node_builder("Concat")
                         .Input(const_0)                           // concat_dim
                         .Input({{loop_len_node_, 0}, {ones, 0}})  // values
                         .Finalize(g, &multiples));

  Node* expand_dims;
  TF_RETURN_IF_ERROR(node_builder("ExpandDims")
                         .Input(unstacked->node)  // input
                         .Input(const_0)          // dim
                         .Finalize(g, &expand_dims));

  TF_RETURN_IF_ERROR(node_builder("Tile")
                         .Input(expand_dims)  // input
                         .Input(multiples)    // multiples
                         .Finalize(g, &result->first));
  result->second = 0;
  return Status::OK();
}

Status Vectorization::AddArgNodeMappings() {
  for (auto arg_node : map_defun_fn_->arg_nodes) {
    Node* input_node;
    TF_RETURN_IF_ERROR(map_defun_node_->input_node(
        arg_node->attrs().Find("index")->i(), &input_node));

    conversion_map_.insert({{arg_node, 0}, {input_node, 0, true}});

    // Control inputs
    conversion_map_.insert({{arg_node, Graph::kControlSlot},
                            {input_node, Graph::kControlSlot, true}});
  }
  return Status::OK();
}

bool Vectorization::AddUnstackedNodeMappingsHelper(TensorDesc&& tensor,
                                                   Status* status) {
  if (auto found = gtl::FindOrNull(conversion_map_, tensor)) {
    return !found->stacked;
  }

  if (tensor.first->op_def().is_stateful()) {
    // We don't lift stateful nodes directly out of the MapDefun, since they may
    // have to be executed N times.
    return false;
  }

  bool is_unstacked = true;
  for (auto edge : tensor.first->in_edges()) {
    // Ignore Source nodes. Note that these are also ignored in the
    // GraphToFunctionDef conversion.
    if (edge->src()->IsSource()) continue;

    // A node is unstacked if all of its inputs are unstacked
    is_unstacked &= AddUnstackedNodeMappingsHelper(
        {edge->src(), edge->src_output()}, status);
  }

  if (!is_unstacked) {
    return false;
  }

  // If the node is unstacked, we copy it into outer_scope_ and
  // add it to the map. Note that we don't clean up the nodes that are copied
  // in map_defun_fn_, and rely on them being pruned out later.
  Node* node = outer_scope_->AddNode(tensor.first->def(), status);
  if (!status->ok()) return true;

  // Add input edges to nodes that should already have been lifted.
  for (auto edge : tensor.first->in_edges()) {
    // Ignore Source nodes. Note that these are also ignored in the
    // GraphToFunctionDef conversion.
    if (edge->src()->IsSource()) continue;

    if (auto found = gtl::FindOrNull(conversion_map_,
                                     {edge->src(), edge->src_output()})) {
      outer_scope_->AddEdge(found->node, found->output_index, node,
                            edge->dst_input());
    } else {
      status->Update(errors::Internal(
          "Could not find input conversion even though we did depth first "
          "conversion."));
    }
  }

  // Add output mappings
  for (int i = 0; i < tensor.first->num_outputs(); ++i) {
    conversion_map_.insert({{tensor.first, i}, WrappedTensor(node, i, false)});
  }
  conversion_map_.insert({{tensor.first, Graph::kControlSlot},
                          WrappedTensor(node, Graph::kControlSlot, false)});

  return true;
}

Status Vectorization::AddUnstackedNodeMappings() {
  SetVector<Node*> unstacked_nodes;
  Status s;
  for (const auto& ret_node : map_defun_fn_->ret_nodes) {
    const Edge* in_edge = nullptr;
    TF_RETURN_IF_ERROR(ret_node->input_edge(0, &in_edge));
    AddUnstackedNodeMappingsHelper({in_edge->src(), in_edge->src_output()}, &s);
    TF_RETURN_IF_ERROR(s);
  }
  return Status::OK();
}

Status Vectorization::GetResult(FunctionDef** vectorized_function) {
  TF_RETURN_IF_ERROR(status_);
  TF_RETURN_IF_ERROR(graph_utils::EnsureNodeNamesUnique(outer_scope_.get()));
  TF_RETURN_IF_ERROR(graph_utils::EnsureNodeNamesUnique(map_defun_fn_->graph));

  if (!map_defun_fn_->ret_nodes.empty()) {
    FunctionDef* map_defun_fn = lib_->add_function();
    graph_utils::SetUniqueGraphFunctionName("map_defun_fn", lib_, map_defun_fn);
    TF_RETURN_IF_ERROR(GraphToFunctionDef(
        *map_defun_fn_->graph, map_defun_fn->signature().name(), map_defun_fn));

    AttrValue func_attr;
    func_attr.mutable_func()->set_name(map_defun_fn->signature().name());
    map_defun_node_->AddAttr("f", func_attr);
  }

  *vectorized_function = lib_->add_function();
  graph_utils::SetUniqueGraphFunctionName("vectorized_fn", lib_,
                                          *vectorized_function);
  TF_RETURN_IF_ERROR(GraphToFunctionDef(
      *outer_scope_, (*vectorized_function)->signature().name(),
      *vectorized_function));
  return Status::OK();
}

}  // namespace

Status VectorizeMapDefun(const FunctionDef& outer_scope,
                         const NodeDef& map_defun_node, FunctionDefLibrary* lib,
                         FunctionDef** result) {
  *result = nullptr;
  return Vectorization(lib).Vectorize(outer_scope, map_defun_node, result);
}

}  // end namespace vectorization_utils
}  // end namespace grappler
}  // end namespace tensorflow
