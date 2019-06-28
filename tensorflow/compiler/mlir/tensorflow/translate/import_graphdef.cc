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

#include "tensorflow/compiler/mlir/tensorflow/translate/import_graphdef.h"

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/inlined_vector.h"
#include "absl/strings/escaping.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/raw_ostream.h"
#include "mlir/IR/Attributes.h"  // TF:local_config_mlir
#include "mlir/IR/Builders.h"  // TF:local_config_mlir
#include "mlir/IR/Identifier.h"  // TF:local_config_mlir
#include "mlir/IR/Location.h"  // TF:local_config_mlir
#include "mlir/IR/MLIRContext.h"  // TF:local_config_mlir
#include "mlir/IR/Module.h"  // TF:local_config_mlir
#include "mlir/IR/Types.h"  // TF:local_config_mlir
#include "mlir/StandardOps/Ops.h"  // TF:local_config_mlir
#include "tensorflow/compiler/jit/shape_inference_helpers.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/control_flow_ops.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/mlir/tensorflow/translate/mlir_roundtrip_flags.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/convert_tensor.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/convert_type.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/mangling_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/core/common_runtime/function.h"
#include "tensorflow/core/common_runtime/shape_refiner.h"
#include "tensorflow/core/framework/attr_value.pb.h"
#include "tensorflow/core/framework/graph.pb.h"
#include "tensorflow/core/framework/node_def.pb.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/shape_inference.h"
#include "tensorflow/core/framework/tensor.pb.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/graph/algorithm.h"
#include "tensorflow/core/graph/graph.h"
#include "tensorflow/core/graph/graph_constructor.h"
#include "tensorflow/core/graph/node_builder.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/strings/str_util.h"
#include "tensorflow/core/platform/protobuf.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/protobuf/graph_debug_info.pb.h"

namespace tensorflow {
using stream_executor::port::StatusOr;

namespace {

// Stateful helper class to import a GraphDef into an MLIR Module. The nodes
// defined in the graph is converted to a function called "main". All the
// library function definitions are converted to MLIR functions in the module.
class Importer {
 public:
  // Main entry point: converts the given graph to an MLIR Module.
  static StatusOr<std::unique_ptr<mlir::Module>> Convert(
      mlir::MLIRContext* context, const Graph& graph,
      const GraphDebugInfo& debug_info,
      const FunctionLibraryDefinition& flib_def, const NodeSpecs& specs);

 private:
  explicit Importer(
      const FunctionLibraryDefinition& flib, const GraphDebugInfo& debug_info,
      const NodeSpecs& specs, mlir::Module* module,
      std::unordered_map<std::string, std::string>* tf_name_to_mlir_name)
      : module_(module),
        context_(module->getContext()),
        tf_name_to_mlir_name_(tf_name_to_mlir_name),
        graph_flib_(flib),
        specs_(specs),
        debug_info_(debug_info) {}

  // Prepares converting the graph to an MLIR module. This step removes the
  // backedges of the graph, orders the nodes and infers the shapes.
  Status PrepareConvert(const Graph& graph);

  // Returns the function signature of the main function of converted MLIR
  // module, the input nodes and output nodes. The type and shape information
  // for the function arguments are read from the specs_, but the type and shape
  // information for the function returns are inferred by the shape_refiner_.
  StatusOr<mlir::FunctionType> InferMainFunctionType(
      absl::InlinedVector<OutputTensor, 4>* arg_nodes,
      absl::InlinedVector<OutputTensor, 4>* ret_nodes);

  // Returns the inferred function signature of the given function body. Input
  // types are unranked tensor of the respective datatype in the function and
  // result types are inferred by the shape_refiner_. Result types need not be
  // unranked tensors and could be ranked tensors in cases where result type
  // depends on an op with static output shape like tf.Const.
  StatusOr<mlir::FunctionType> InferLibFunctionType(const FunctionBody& fbody);

  // Converts the prepared graph to a Function and adds it to the module. A set
  // of nodes from the graph are given to converted to the arguments and returns
  // of the function.
  Status Convert(llvm::StringRef func_name, mlir::FunctionType func_type,
                 const absl::InlinedVector<OutputTensor, 4>& arg_nodes,
                 const absl::InlinedVector<OutputTensor, 4>& ret_nodes,
                 llvm::ArrayRef<mlir::NamedAttribute> attrs);

  // Adds all the ordered_ndoes_ to the shape refiner shape_refiner_. Then all
  // data type and shape information is maintained by the shape_refiner_.
  Status AddNodesToShapeRefiner();

  // Gets the inferred type for the i-th input of the node in the context.
  StatusOr<mlir::Type> InferInputType(ExtendedInferenceContext* shape_context,
                                      int i, mlir::Builder builder);

  // Gets the inferred type for the i-th output of the node in the context.
  StatusOr<mlir::Type> InferOutputType(ExtendedInferenceContext* shape_context,
                                       int i, mlir::Builder builder);

  // Converts the TF DataType dtype into an MLIR (scalar) type.
  Status ConvertDataType(const DataType& dtype, mlir::Builder builder,
                         mlir::Type* type) {
    return ::tensorflow::ConvertDataType(dtype, builder, type);
  }

  // Converts the inferred shape referred to by 'handle' in 'context', with
  // given element type, and return an MLIR type.
  StatusOr<mlir::Type> ConvertShapeAndDataType(
      const shape_inference::ShapeHandle& handle,
      shape_inference::InferenceContext* context, DataType dtype,
      mlir::Builder builder);

  // Converts the tensor proto into an MLIR elements attribute.
  StatusOr<mlir::ElementsAttr> ConvertTensorProto(const TensorProto& value) {
    return ::tensorflow::ConvertTensorProto(value, builder_.get());
  }

  // Converts func name in graphdef to mlir::FunctionAttribute.
  StatusOr<mlir::FunctionAttr> ConvertFunctionCallName(
      const std::string& func_name);

  // Converts the given non-function-call AttrValue to an MLIR Attribute.
  StatusOr<mlir::Attribute> ConvertAttributeValue(const AttrValue& value);

  // Converts the given function-call AttrValue to MLIR Attributes and pushes
  // them to the given attributes list. For example, if there is a kFunc
  // AttrValue {name : foo, attrs : {k1 : bar, k2 : rfc}}, it will convert it to
  // a list of MLIR Attributes: [{base_name : foo}, {base_name.k1 : bar},
  // {base_name.k2 : rfc}}.
  Status ConvertFunctionCallAttribute(
      const std::string& base_name, const AttrValue& value,
      llvm::SmallVector<mlir::NamedAttribute, 4>* attributes);

  // Converts one NodeDef from the input GraphDef into an Operation and
  // inserts it into the MLIR module using builder_.
  Status ConvertNode(const Node& node);

  // If the input graph represents a while-loop, the edges pointing from a
  // "NextIteration" node to a "Merge" node add cyclic dependencies and make the
  // topological sorting impossible. We need to remove these edges from the
  // input graph to infer shapes and construct a Function. For each
  // "NextIteration" node, there are two operations, "NextIteration.source"
  // and "NextIteration.sink" are added to the MLIR module.
  using BackEdge = BackEdgeHelper::BackEdge;
  using Backedges = std::vector<const BackEdge*>;

  // Removes backedges from the input graph. The removed edges are added back to
  // to OpBuilder after the remaining graph is converted to the Function.
  Status RemoveBackedges(const Graph& graph);

  // Restores backedges removed during shape inference to the final Function.
  Status AddBackedges();

  // Restores a single backedge in the Function by adding a replicated
  // operation before the dst operation.
  Status AddBackedge(mlir::Operation* sink, mlir::Operation* dst,
                     int dst_input);

  // Gets the "source" of a NextIteration operation. If it doesn't exist,
  // creates and inserts it to the front of the basic block.
  mlir::Operation* GetOrCreateNextIterationSource(mlir::Operation* sink,
                                                  mlir::Operation* dst);

  // Finds out the function definition for the given function name from the
  // graph and converts it to a function of the module. This method is called
  // on demand because the graph flib_def does not provide an iterator
  // interface. The consequence is that only the referred functions are added to
  // the MLIR module.
  Status ConvertLibFunction(const std::string& func_name);

  // Adds the input arguments and return operation to the function. The
  // arguments are added as basic block argument. Also the argument types and
  // the id of the nodes from the input graph needs to be specified.
  Status ConvertFunctionArgAndRets(
      mlir::Block* bb, llvm::ArrayRef<mlir::Type> arg_types,
      const absl::InlinedVector<OutputTensor, 4>& arg_nodes,
      const absl::InlinedVector<OutputTensor, 4>& ret_nodes);

  // Gets the location information of the given node. It uses the
  // "original_node_name" in the NodeDef to get the corresponding file location
  // (FileLineColLoc) from the input DebugInfo and returns an CallSiteLoc. If
  // there are multiple "original_node_names", a FusedLoc is returned. If the
  // node name couldn't be found in the input DebugInfo, a NameLoc is used as
  // the location.
  mlir::Location GetLocation(const NodeDef& node);

  // Gets the location information string for the given node.
  std::string GetLocationStr(const Node& node, bool includeNodeName = false);

  // Inserts a placeholder node in the graph to replace the input node. Replaces
  // all the output edges of the input_node with the placeholder node, and
  // removes the input_node from the graph. The new node has the same name as
  // the input_node, so Nodespecs do not need any modification.
  // Note: This modifies the graph, and so any list of ordered nodes needs to be
  // reconstructed.
  StatusOr<Node*> ReplaceWithPlaceholderNode(const TensorShapeProto& shape,
                                             DataType dtype, Node* input_node);

  // Gets the output_nodes corresponding to the specified output_arrays in
  // specs_. If there are no output_arrays set, output_nodes will be empty
  Status GetOutputNodes(std::unordered_set<const Node*>* output_nodes);

  // The input graph with backedges removed. The removed backedges are stored
  // in the back_edge_helper.
  BackEdgeHelper back_edge_helper_;
  // A map between node and output index, for each backedge.
  absl::flat_hash_map<const Node*, int> back_edge_node_output_;
  absl::flat_hash_map<const Node*, Backedges> back_edge_dst_inputs_;
  // A map between sink and source operation of NextIteration
  absl::flat_hash_map<mlir::Operation*, mlir::Operation*>
      next_iteration_sink_source_;

  // All nodes and version information about the (copied) imported graph.
  std::unique_ptr<Graph> graph_;
  const VersionDef* graph_versions_;
  std::vector<Node*> ordered_nodes_;

  // Maps from a Node ID to a MLIR value.
  using NodeValueMap = absl::flat_hash_map<int, mlir::Operation*>;

  std::unique_ptr<mlir::OpBuilder> builder_;
  mlir::Module* module_;
  mlir::MLIRContext* context_;
  std::unordered_map<std::string, std::string>* tf_name_to_mlir_name_;
  const FunctionLibraryDefinition& graph_flib_;
  const NodeSpecs& specs_;
  const GraphDebugInfo& debug_info_;
  NodeValueMap node_values_;
  std::unique_ptr<ShapeRefiner> shape_refiner_;
};

// Adds the default attributes to each node def if they are missing from the
// GraphDef.
Status AddDefaultsToNodeDef(GraphDef* graph_def) {
  const tensorflow::OpRegistrationData* op_reg_data;
  for (auto& node_def : *graph_def->mutable_node()) {
    auto status =
        tensorflow::OpRegistry::Global()->LookUp(node_def.op(), &op_reg_data);
    if (!status.ok()) {
      // This is likely a function call node, so we should continue.
      VLOG(1) << status.ToString();
      continue;
    }
    ::tensorflow::AddDefaultsToNodeDef(op_reg_data->op_def, &node_def);
  }
  return Status::OK();
}

Status Importer::RemoveBackedges(const Graph& graph) {
  // TODO(fengliuai): Converting to GraphDef and back is the easiest way to
  // clone a graph.
  // TODO(fengliuai): clone the graph without going to graph_def first.
  GraphDef graph_def;
  graph.ToGraphDef(&graph_def);
  graph_ = absl::make_unique<Graph>(graph.flib_def());
  GraphConstructorOptions opts;
  opts.allow_internal_ops = true;
  TF_RETURN_IF_ERROR(
      ::tensorflow::ConvertGraphDefToGraph(opts, graph_def, graph_.get()));

  // Remove all the backedges. So the nodes can be added to the shape refiner.
  TF_RETURN_IF_ERROR(back_edge_helper_.Remove(graph_.get()));
  VLOG(1) << "Found " << (back_edge_helper_.RemovedEdges().size())
          << " backedges.";

  // Creates a map for quickly identifying whether a node output is a backedge.
  for (const auto& edge : back_edge_helper_.RemovedEdges()) {
    if (back_edge_node_output_.find(edge.src) != back_edge_node_output_.end() &&
        back_edge_node_output_[edge.src] != edge.src_output) {
      return errors::FailedPrecondition(
          "More than one of the src node outputs are backedges!");
    }
    back_edge_node_output_[edge.src] = edge.src_output;
    back_edge_dst_inputs_[edge.dst].push_back(&edge);
  }

  // Obtains a RPO ordering, using node names as a tiebreak for stable sorting.
  GetReversePostOrder(
      *graph_, &ordered_nodes_,
      [](const Node* n1, const Node* n2) { return n1->name() < n2->name(); });

  return Status::OK();
}

StatusOr<Node*> Importer::ReplaceWithPlaceholderNode(
    const TensorShapeProto& shape, DataType dtype, Node* input_node) {
  Node* placeholder_node;
  NodeBuilder builder(input_node->name(), "Placeholder");
  builder.Attr("shape", shape);
  builder.Attr("dtype", dtype);
  TF_RETURN_IF_ERROR(builder.Finalize(graph_.get(), &placeholder_node));

  while (!input_node->out_edges().empty()) {
    const Edge* oe = *input_node->out_edges().begin();
    TF_RETURN_IF_ERROR(graph_->UpdateEdge(
        placeholder_node,
        oe->src_output() == Graph::kControlSlot ? Graph::kControlSlot : 0,
        oe->dst(), oe->dst_input()));
  }

  graph_->RemoveNode(input_node);

  return placeholder_node;
}

Status Importer::GetOutputNodes(std::unordered_set<const Node*>* output_nodes) {
  auto node_name_map = graph_->BuildNodeNameIndex();
  for (auto& output_node_name : specs_.output_arrays) {
    auto it = node_name_map.find(output_node_name);
    if (it == node_name_map.end()) {
      return errors::FailedPrecondition(absl::StrCat(
          "Graph does not contain a node corresponding to output array:",
          output_node_name));
    }
    output_nodes->insert(it->second);
  }
  return Status::OK();
}

// TODO(fengliuai): Replace the iterative algorithm by an one pass propagation
Status Importer::AddNodesToShapeRefiner() {
  shape_refiner_ =
      absl::make_unique<ShapeRefiner>(*graph_versions_, graph_->op_registry());
  // Some operations (for example "TPUExecute") don't have shape inference
  // function defined, so we should set this to false for adding nodes with
  // these types of operations.
  shape_refiner_->set_require_shape_inference_fns(false);
  shape_refiner_->set_function_library_for_shape_inference(&graph_flib_);

  // First add all nodes to the refiner.
  for (Node* node : ordered_nodes_) {
    // We need to use a TensorFlow node to teach the shape refiner that user
    // specifies certain data type and shape for the inputs in the `specs_`.
    // This node shouldn't have any inputs, only have one output and its
    // output type/shape is only determined by its "named" attributes. (The
    // attributes should have fixed names so we can use the info from `specs_`
    // to set the value of them.) `Placeholder` satisfies these constraints.
    //
    // Therefore, if the input node isn't a `Placeholder`, we create one and use
    // it to replace the original input node, so the shape refiner can
    // successfully propagate the user's input type and shape to the rest of the
    // graph.
    auto it = specs_.inputs.find(node->name());
    if (it != specs_.inputs.end()) {
      auto node_name = node->op_def().name();
      if (node_name != "Placeholder" && node_name != "LegacyFedInput") {
        // We do not handle the case where the input node has multple outputs
        if (node->num_outputs() > 1) {
          return errors::FailedPrecondition(absl::StrCat(
              "Input arrays can only have op with single output. Node op:",
              node_name));
        }
        // For single output nodes, replace them with Placeholder node
        TF_ASSIGN_OR_RETURN(
            node, ReplaceWithPlaceholderNode(it->second.shape,
                                             it->second.imported_dtype, node));
      } else {
        node->AddAttr("shape", it->second.shape);
        node->AddAttr("dtype", it->second.imported_dtype);
      }
    }
    // Adds the node to the shape refiner.
    TF_RETURN_WITH_CONTEXT_IF_ERROR(shape_refiner_->AddNode(node),
                                    GetLocationStr(*node));

    // If it is the argument node, the shape handle is set explicitly, so it
    // can be propagated to the body nodes of the function.
    if (StringPiece(node->type_string()) == FunctionLibraryDefinition::kArgOp) {
      auto* node_context = shape_refiner_->GetContext(node);
      DCHECK(node_context != nullptr);
      auto it = node->def().attr().find("shape");
      if (it != node->def().attr().end()) {
        shape_inference::ShapeHandle handle;
        TF_RETURN_WITH_CONTEXT_IF_ERROR(
            node_context->MakeShapeFromShapeProto(it->second.shape(), &handle),
            GetLocationStr(*node));
        node_context->set_output(0, handle);
      } else {
        node_context->set_output(0, node_context->UnknownShape());
      }
    }
  }

  // Since we might have inserted and removed nodes from the graph, fix
  // source/sink edges and reconstruct the RPO ordering of nodes
  FixupSourceAndSinkEdges(graph_.get());

  // Prune nodes in the graph that are not reachable from the output.
  if (specs_.prune_unused_nodes) {
    std::unordered_set<const Node*> prune_start;
    TF_RETURN_IF_ERROR(GetOutputNodes(&prune_start));
    if (!prune_start.empty()) {
      if (PruneForReverseReachability(graph_.get(), prune_start)) {
        VLOG(1) << "Pruned unused nodes in graphdef";
      } else {
        VLOG(1) << "No unused nodes in graphdef to prune";
      }
    } else {
      VLOG(1) << "No output nodes specified, skipping pruning";
    }
  } else {
    VLOG(1) << "Pruning unused nodes in graphdef is disabled";
  }

  // Re-initialize ordered_nodes_ since we might have modified the graph.
  GetReversePostOrder(
      *graph_, &ordered_nodes_,
      [](const Node* n1, const Node* n2) { return n1->name() < n2->name(); });

  VLOG(1) << "Inferring graph shapes to fixpoint";

  // The "changed" information from UpdateNode can give false positives, so we
  // create a dedicated method to verify the shapes are not changed before and
  // after the shape refine.
  auto same_inferred_shape = [](shape_inference::InferenceContext* c,
                                shape_inference::ShapeHandle s0,
                                shape_inference::ShapeHandle s1) -> bool {
    if (s0.SameHandle(s1) || (!c->RankKnown(s0) && !c->RankKnown(s1))) {
      return true;
    }
    if (c->Rank(s0) != c->Rank(s1)) {
      return false;
    }
    for (int i = 0; i < c->Rank(s0); ++i) {
      if (!c->Dim(s0, i).SameHandle(c->Dim(s1, i))) {
        int64 val0 = c->Value(c->Dim(s0, i));
        int64 val1 = c->Value(c->Dim(s1, i));
        // Negative value is treated as unknown so all negative values indicate
        // the same dimension.
        if (val0 >= 0 && val1 >= 0 && val0 != val1) return false;
      }
    }
    return true;
  };

  bool changed = true;
  int i = 0;
  const int kMaxIterationCount = 2;
  while (changed && i != kMaxIterationCount) {
    changed = false;
    for (const Node* node : ordered_nodes_) {
      auto* shape_context = shape_refiner_->GetContext(node);
      DCHECK(shape_context != nullptr);
      absl::InlinedVector<shape_inference::ShapeHandle, 4> existing;
      existing.reserve(shape_context->num_outputs());
      for (int o = 0; o < shape_context->num_outputs(); ++o) {
        existing.push_back(shape_context->output(o));
      }
      bool inferred = false;
      TF_RETURN_WITH_CONTEXT_IF_ERROR(
          shape_refiner_->UpdateNode(node, /*relax=*/false, &inferred),
          GetLocationStr(*node));
      for (int o = 0; o < shape_context->num_outputs(); ++o) {
        if (!same_inferred_shape(shape_context, shape_context->output(o),
                                 existing[o])) {
          changed = true;
          break;
        }
      }
    }
    ++i;
  }
  if (i >= kMaxIterationCount) {
    LOG(WARNING) << "Graph shapes did not converge to a fixpoint within "
                 << kMaxIterationCount
                 << " iterations. Graph shapes may be conservative.";
  }
  VLOG(1) << "Graph shapes were inferred with " << (i - 1)
          << " extra rounds of analysis to reach a fixpoint.";
  return Status::OK();
}

StatusOr<mlir::Type> Importer::InferInputType(
    ExtendedInferenceContext* shape_context, int i, mlir::Builder builder) {
  DataType dtype = shape_context->input_type(i);
  shape_inference::ShapeHandle input_shape_handle =
      shape_context->get_context()->input(i);
  return ConvertShapeAndDataType(input_shape_handle,
                                 shape_context->get_context(), dtype, builder);
}

StatusOr<mlir::Type> Importer::InferOutputType(
    ExtendedInferenceContext* shape_context, int i, mlir::Builder builder) {
  DataType dtype = shape_context->output_type(i);
  shape_inference::ShapeHandle output_shape_handle =
      shape_context->get_context()->output(i);
  return ConvertShapeAndDataType(output_shape_handle,
                                 shape_context->get_context(), dtype, builder);
}

StatusOr<mlir::Type> Importer::ConvertShapeAndDataType(
    const shape_inference::ShapeHandle& handle,
    shape_inference::InferenceContext* context, DataType dtype,
    mlir::Builder builder) {
  mlir::Type output_type;
  mlir::Type element_type;
  TF_RETURN_IF_ERROR(ConvertDataType(dtype, builder, &element_type));

  if (!context->RankKnown(handle)) {
    return builder.getTensorType(element_type);
  }

  // Sentinel for an unknown dimension size. getTensorType interprets any
  // negative value as an unknown dimension.
  // TODO(jmolloy): Ideally this shouldn't be a local sentinel.
  const int64_t kUnknownDim = -1;

  absl::InlinedVector<int64_t, 4> dimensions;
  int32 rank = context->Rank(handle);
  dimensions.reserve(rank);
  for (int i = 0; i < rank; ++i) {
    auto dim_handle = context->Dim(handle, i);
    if (!context->ValueKnown(dim_handle))
      dimensions.push_back(kUnknownDim);
    else
      dimensions.push_back(context->Value(dim_handle));
  }

  return builder.getTensorType(
      llvm::makeArrayRef(dimensions.begin(), dimensions.end()), element_type);
}

Status Importer::ConvertFunctionCallAttribute(
    const std::string& base_name, const AttrValue& value,
    llvm::SmallVector<mlir::NamedAttribute, 4>* attributes) {
  TF_ASSIGN_OR_RETURN(auto func_attr,
                      ConvertFunctionCallName(value.func().name()));
  attributes->push_back(builder_->getNamedAttr(base_name, func_attr));

  for (const auto& it : value.func().attr()) {
    auto name = absl::StrCat(base_name, ".", it.first);
    TF_ASSIGN_OR_RETURN(auto value, ConvertAttributeValue(it.second));
    attributes->push_back(builder_->getNamedAttr(name, value));
  }
  return Status::OK();
}

StatusOr<mlir::FunctionAttr> Importer::ConvertFunctionCallName(
    const std::string& func_name) {
  TF_RETURN_IF_ERROR(ConvertLibFunction(func_name));
  auto mlir_func_name = (*tf_name_to_mlir_name_)[func_name];
  auto func = module_->getNamedFunction(mlir_func_name);
  return builder_->getFunctionAttr(func);
}

StatusOr<mlir::Attribute> Importer::ConvertAttributeValue(
    const AttrValue& value) {
  switch (value.value_case()) {
    case AttrValue::kI:
      return builder_->getI64IntegerAttr(value.i());
    case AttrValue::kS:
      return builder_->getStringAttr(value.s());
    case AttrValue::kF:
      return builder_->getFloatAttr(builder_->getF32Type(), value.f());
    case AttrValue::kB:
      return builder_->getBoolAttr(value.b());
    case AttrValue::kType:
      return builder_->getStringAttr(
          mangling_util::MangleDataType(value.type()));
    case AttrValue::kShape:
      return builder_->getStringAttr(mangling_util::MangleShape(value.shape()));
    case AttrValue::kTensor:
      return ConvertTensorProto(value.tensor());
    case AttrValue::kList: {
      absl::InlinedVector<mlir::Attribute, 8> attrs;
      for (const auto& item : value.list().i())
        attrs.push_back(builder_->getI64IntegerAttr(item));
      for (const auto& item : value.list().s())
        attrs.push_back(builder_->getStringAttr(item));
      for (const auto& item : value.list().f())
        attrs.push_back(builder_->getFloatAttr(builder_->getF32Type(), item));
      for (const auto& item : value.list().b())
        attrs.push_back(builder_->getBoolAttr(item));
      for (const auto& item : value.list().type()) {
        attrs.push_back(builder_->getStringAttr(
            mangling_util::MangleDataType(static_cast<DataType>(item))));
      }
      for (const auto& item : value.list().shape()) {
        attrs.push_back(
            builder_->getStringAttr(mangling_util::MangleShape(item)));
      }
      for (const auto& item : value.list().tensor()) {
        TF_ASSIGN_OR_RETURN(auto attr, ConvertTensorProto(item));
        attrs.push_back(attr);
      }
      for (const auto& item : value.list().func()) {
        TF_ASSIGN_OR_RETURN(auto attr, ConvertFunctionCallName(item.name()));
        if (item.attr_size() != 0)
          return errors::Unimplemented(
              "func attributes with non-zero attr.size()");
        attrs.push_back(attr);
      }
      return builder_->getArrayAttr(
          llvm::makeArrayRef(attrs.begin(), attrs.end()));
    }
    case AttrValue::kFunc:
      return errors::Unknown("kFunc type should be handled separately!");
    // kPlaceholder is not implemented.
    default:
      return errors::Unimplemented(
          absl::StrCat("Attribute ", value.DebugString()));
  }
}

Status Importer::ConvertLibFunction(const std::string& func_name) {
  // If the library function has been converted already, nothing needs to be
  // done.
  if (tf_name_to_mlir_name_->find(func_name) != tf_name_to_mlir_name_->end())
    return Status::OK();

  std::string mlir_func_name = graph_flib_.UniqueFunctionName(func_name);
  (*tf_name_to_mlir_name_)[func_name] = mlir_func_name;

  const auto& func_lib = graph_flib_;
  const auto* func_def = func_lib.Find(func_name);
  if (func_def == nullptr) {
    return errors::FailedPrecondition(
        absl::StrCat("Failed to find function '", func_name,
                     "'. The imported TensorFlow GraphDef is ill-formed."));
  }

  // Converts the function definition to a graph.
  std::unique_ptr<FunctionBody> fbody;
  TF_RETURN_IF_ERROR(
      FunctionDefToBodyHelper(*func_def, AttrSlice(), &func_lib, &fbody));

  // Converts the argument and return types to mlir types.
  absl::InlinedVector<mlir::NamedAttribute, 8> attributes;
  attributes.reserve(func_def->attr_size());
  for (const auto& name_and_value : func_def->attr()) {
    // This is a function definition attribute, so it shouldn't contain
    // kFunc attribute and it is treated as normal one.
    TF_ASSIGN_OR_RETURN(auto attr,
                        ConvertAttributeValue(name_and_value.second));
    std::string attr_name =
        mangling_util::MangleAttributeName(name_and_value.first);
    attributes.push_back(builder_->getNamedAttr(attr_name, attr));
  }

  // Checks for an associated custom gradient function. Adds it to the attribute
  // list of this function.
  auto grad_func_name = func_lib.FindGradient(func_name);
  if (!grad_func_name.empty()) {
    TF_RETURN_IF_ERROR(ConvertLibFunction(grad_func_name));
    auto mlir_grad_func_name = (*tf_name_to_mlir_name_)[grad_func_name];
    auto grad_func = module_->getNamedFunction(mlir_grad_func_name);
    auto gradient_attr = builder_->getFunctionAttr(grad_func);
    auto grad_string = mlir::TF::TensorFlowDialect::GetGradientAttrName();
    attributes.push_back(builder_->getNamedAttr(grad_string, gradient_attr));
  }

  // Converts the graph to a MLIR function and adds it to the module. Uses the
  // default node spec without any inputs or outputs as the function graph has
  // special '_Arg' and '_Retval' ops for argument and return values.
  NodeSpecs specs;
  Importer child_importer(graph_flib_, debug_info_, specs, module_,
                          tf_name_to_mlir_name_);
  TF_RETURN_IF_ERROR(child_importer.PrepareConvert(*fbody->graph));

  TF_ASSIGN_OR_RETURN(auto func_type,
                      child_importer.InferLibFunctionType(*fbody));

  absl::InlinedVector<OutputTensor, 4> arg_nodes;
  arg_nodes.reserve(fbody->arg_nodes.size());
  absl::InlinedVector<OutputTensor, 4> ret_nodes;
  ret_nodes.reserve(fbody->ret_nodes.size());
  for (auto arg : fbody->arg_nodes) {
    arg_nodes.emplace_back(arg, 0);
  }
  for (auto ret : fbody->ret_nodes) {
    ret_nodes.emplace_back(ret, 0);
  }

  TF_RETURN_IF_ERROR(child_importer.Convert(
      mlir_func_name, func_type, arg_nodes, ret_nodes,
      llvm::makeArrayRef(attributes.begin(), attributes.end())));
  return Status::OK();
}

Status Importer::PrepareConvert(const Graph& graph) {
  graph_versions_ = &graph.versions();
  TF_RETURN_IF_ERROR(RemoveBackedges(graph));
  TF_RETURN_IF_ERROR(AddNodesToShapeRefiner());
  return Status::OK();
}

Status Importer::ConvertFunctionArgAndRets(
    mlir::Block* bb, llvm::ArrayRef<mlir::Type> arg_types,
    const absl::InlinedVector<OutputTensor, 4>& arg_nodes,
    const absl::InlinedVector<OutputTensor, 4>& ret_nodes) {
  for (int i = 0, e = arg_types.size(); i < e; ++i) {
    auto* inst = node_values_[arg_nodes[i].node->id()];
    auto* bb_arg = bb->addArgument(arg_types[i]);
    mlir::Value* arg_def = bb_arg;

    // If this is an input node add argument to the operation operands by
    // creating a new input operation.
    if (StringPiece(arg_nodes[i].node->type_string()) !=
        FunctionLibraryDefinition::kArgOp) {
      auto inst_name = inst->getName().getStringRef();
      mlir::OperationState state(inst->getLoc(),
                                 inst_name.str().append(".input"));
      state.attributes.append(inst->getAttrs().begin(), inst->getAttrs().end());

      // If there are quantization specifications, add them as the attributes
      auto name = inst->getAttrOfType<mlir::StringAttr>("name").getValue();
      auto input_spec_it = specs_.inputs.find(name.str());
      if (input_spec_it != specs_.inputs.end()) {
        auto input_spec = input_spec_it->second;
        if (IsQuantizationType(input_spec.final_dtype)) {
          // Uses the MLIR built-in type so it can be handled easily later.
          auto final_type = mlir::IntegerType::get(
              GetQuantizationTypeWidth(input_spec.final_dtype), context_);
          state.attributes.push_back(builder_->getNamedAttr(
              "min", builder_->getF32FloatAttr(input_spec.min_value)));
          state.attributes.push_back(builder_->getNamedAttr(
              "max", builder_->getF32FloatAttr(input_spec.max_value)));
          state.attributes.push_back(builder_->getNamedAttr(
              "type", builder_->getTypeAttr(final_type)));
          bb->getFunction()->setAttr("tf.quantize", builder_->getUnitAttr());
        }
      }

      for (auto* r : inst->getResults()) state.types.push_back(r->getType());

      state.operands.append(inst->getOperands().begin(),
                            inst->getOperands().end());
      state.operands.push_back(bb_arg);
      builder_->setInsertionPoint(inst);
      auto* input = builder_->createOperation(state);
      arg_def = input->getResult(arg_nodes[i].index);
      // Verify on the equivalent TF op would have failed, but catching this
      // earlier for now as this exposed a bug. TODO(jpienaar): remove post
      // dialect refactoring.
      DCHECK(input->getResult(0)->getType() == input->getOperand(0)->getType())
          << "invalid placeholder_input constructed";
    }

    for (auto index = 0; index < inst->getNumResults(); index++) {
      inst->getResult(index)->replaceAllUsesWith(arg_def);
    }
    inst->dropAllReferences();
    inst->erase();
  }

  absl::InlinedVector<mlir::Value*, 8> inst_to_returned;
  for (const auto& ret : ret_nodes) {
    auto* inst = node_values_[ret.node->id()];
    auto op = absl::string_view(ret.node->type_string());
    if (op == FunctionLibraryDefinition::kRetOp ||
        op == FunctionLibraryDefinition::kDeviceRetOp) {
      // Remove kRetOp or kDeviceRetOp operation and return its operand.
      // kRetOp and kDeviceRetOp should have just one operand unless they have
      // control dependencies.
      if (inst->getNumOperands() != 1)
        return errors::Unimplemented("Return node with multiple inputs.");
      inst_to_returned.push_back(inst->getOperand(0));
      node_values_[ret.node->id()]->dropAllReferences();
      node_values_[ret.node->id()]->erase();
    } else {
      inst_to_returned.push_back(inst->getResult(ret.index));
    }
  }
  builder_->setInsertionPointToEnd(bb);
  builder_->create<mlir::ReturnOp>(
      mlir::UnknownLoc::get(context_),
      llvm::makeArrayRef(inst_to_returned.begin(), inst_to_returned.end()));
  return Status::OK();
}

mlir::Location Importer::GetLocation(const NodeDef& node_def) {
  const auto& debug_info = debug_info_.traces();

  // Get the CallSiteLoc for a node name.
  // - If the debug info of the node couldn't be found, the caller of the
  //   returned CallSiteLoc is set to an UnknownLoc;
  // - If the debug info of the node is found, the caller of the returned
  //   CallSiteLoc is set to a call stack which is formed by the debug info.
  auto node_name_to_call_site = [&](const std::string& name) -> mlir::Location {
    auto name_id = mlir::Identifier::get(name, context_);
    const auto& location_it = debug_info.find(name);
    if (location_it == debug_info.end()) {
      // Only the node name is stored if the location is unknown.
      return mlir::NameLoc::get(name_id, context_);
    }

    // Convert the stack trace to a chain of mlir::CallSiteLocs.
    const auto& trace = location_it->second;
    llvm::SmallVector<mlir::Location, 4> locations;
    locations.reserve(trace.file_line_cols_size());
    for (const auto& location : trace.file_line_cols()) {
      const auto& file = debug_info_.files(location.file_index());
      auto file_name = mlir::Identifier::get(file, context_);
      auto file_line_loc = mlir::FileLineColLoc::get(file_name, location.line(),
                                                     location.col(), context_);
      locations.push_back(file_line_loc);
    }
    // Handle empty location vector.
    if (locations.empty()) return mlir::NameLoc::get(name_id, context_);

    // Use the front FileLineColLoc to generate a NameLoc.
    mlir::Location node_name_loc =
        mlir::NameLoc::get(name_id, locations.front(), context_);

    // If there are more locations then generate a stack trace, otherwise just
    // return the name loc.
    auto callsite_locs = llvm::makeArrayRef(locations).drop_front();
    return callsite_locs.empty()
               ? node_name_loc
               : mlir::CallSiteLoc::get(node_name_loc, callsite_locs, context_);
  };

  // For NextIteration nodes, location is used to pair source and sink nodes.
  // Hence, we use node name as location to keep it unique.
  // TODO(prakalps): In future the plan is to use tokens to pair source/sink
  // nodes. Then NextIteration nodes would not need to be handled seprately.
  if (node_def.op() == "NextIteration")
    return node_name_to_call_site(node_def.name());

  auto original_nodes =
      node_def.experimental_debug_info().original_node_names();

  if (original_nodes.empty()) {
    // If the original nodes are not defined in the node def, but the current
    // node name is contained in the debug info file, then we fall back to use
    // the current node name to get the location info. Otherwise, use a
    // NameLoc with node name as in a TensorFlow graph the node name is unique.
    auto& curr_node_name = node_def.name();
    if (debug_info.find(curr_node_name) == debug_info.end()) {
      return mlir::NameLoc::get(mlir::Identifier::get(curr_node_name, context_),
                                context_);
    } else {
      return node_name_to_call_site(curr_node_name);
    }
  } else {
    // If the original nodes are defined, then we use them to get a list of
    // call sites, and then fuse them to a single fused location.
    llvm::SmallVector<mlir::Location, 4> node_call_sites;
    node_call_sites.reserve(original_nodes.size());
    for (const auto& node_name : original_nodes) {
      node_call_sites.push_back(node_name_to_call_site(node_name));
    }
    return mlir::FusedLoc::get(node_call_sites, context_);
  }
}

std::string Importer::GetLocationStr(const Node& node, bool includeNodeName) {
  const auto location = GetLocation(node.def());
  std::string s;
  llvm::raw_string_ostream ss(s);
  location.print(ss);
  ss.flush();
  // Removes the node name prefix if it exists.
  if (!s.empty() && s[0] == '\"' && s.find_first_of(node.name()) == 1) {
    return s.replace(0, node.name().size() + 3, "");
  }
  return s;
}

Status Importer::ConvertNode(const Node& node) {
  if (!node.IsOp()) {
    // Don't import the pseudo-nodes _SOURCE or _SINK. These are added by
    // Graph and don't exist in GraphDef.
    return Status::OK();
  }

  // If it is a custom OP, its definition should be found in the library. We
  // create the MLIR function and insert it to the module if it doesn't exist.
  std::string node_type_name = node.type_string();
  const auto* func_def = graph_flib_.Find(node_type_name);
  if (func_def) {
    TF_RETURN_IF_ERROR(ConvertLibFunction(node_type_name));
    node_type_name = (*tf_name_to_mlir_name_)[node_type_name];
  }

  const char* kTfControlFlowFormPrefix = "_tf.";
  std::string op_name = kTfControlFlowFormPrefix + node_type_name;

  if (back_edge_node_output_.contains(&node)) {
    op_name = op_name + ".sink";
  }

  const auto& node_def = node.def();
  mlir::OperationState result(GetLocation(node_def), op_name);

  ExtendedInferenceContext* context = shape_refiner_->GetExtendedContext(&node);
  for (int i = 0; i < node.num_outputs(); ++i) {
    // The backedge has been removed, so we shouldn't count the corresponding
    // output from the src node when converting to an operation.
    if (back_edge_node_output_.contains(&node) &&
        back_edge_node_output_[&node] == i) {
      continue;
    }
    DataType dtype = context->output_type(i);
    shape_inference::ShapeHandle output_shape_handle =
        context->get_context()->output(i);
    TF_ASSIGN_OR_RETURN(
        auto type,
        ConvertShapeAndDataType(output_shape_handle, context->get_context(),
                                dtype, *builder_));
    result.types.push_back(type);
  }
  result.types.push_back(
      builder_->getType<mlir::TFControlFlow::TFControlType>());

  // Surprisingly input edges can be nondeterministically ordered. This
  // particularly seems to be the case for the control edges between _SOURCE
  // and _SINK that the Graph constructor inserts. Copy the input edges and
  // sort the edges, but only the control edges, not data edges!
  // TODO(jmolloy): We should probably just ignore _SOURCE and _SINK nodes.
  // They'll break roundtripping anyway unless we strip them when converting
  // back to graphdef.
  absl::InlinedVector<const Edge*, 8> in_edges(node.in_edges().size());
  absl::c_copy(node.in_edges(), in_edges.begin());
  absl::c_stable_sort(in_edges, [](const Edge* e1, const Edge* e2) {
    if (e1->IsControlEdge() && !e2->IsControlEdge()) return false;
    if (!e1->IsControlEdge() && e2->IsControlEdge()) return true;
    return e1->dst_input() < e2->dst_input();
  });

  result.operands.reserve(in_edges.size());
  for (const auto* input_edge : in_edges) {
    const Node& input_node = *input_edge->src();
    if (input_node.IsSource()) {
      if (in_edges.size() != 1) {
        return errors::FailedPrecondition(
            "The node has other inputs besides the _Source node");
      }
      // We don't import the _SOURCE node.
      continue;
    }
    if (input_node.IsArg() && input_edge->IsControlEdge()) {
      // Currently we have not reached consensus as to what TF function
      // semantics are (b/133509504). Here we assume that all arguments to a
      // function should be available before we start execution of any internal
      // node. This makes the control dependencies between function arguments
      // and internal nodes redundant, and so we do not import them. The TF
      // inliner however assumes no such dependency between function args and
      // internal nodes exists, unless explicitly stated. Since we drop control
      // dependencies here, it leads to loss of information. If the function is
      // inlined later, the inliner would not know of these explicit control
      // dependencies present in the original graph.
      continue;
    }
    if (node_values_.find(input_node.id()) == node_values_.end())
      return errors::FailedPrecondition(
          "Graph not traversed in reverse post order; use seen before def!");
    mlir::Operation* inst = node_values_[input_node.id()];
    result.operands.push_back(inst->getResult(input_edge->IsControlEdge()
                                                  ? inst->getNumResults() - 1
                                                  : input_edge->src_output()));
  }

  using FuncPairType = std::pair<const std::string*, const AttrValue*>;
  std::vector<FuncPairType> funcs;
  result.attributes.reserve(node.attrs().size() + 2);
  for (const auto& name_and_value : node.attrs()) {
    const auto& attr_name = name_and_value.first;
    const AttrValue& attr_value = name_and_value.second;
    if (attr_value.value_case() == AttrValue::kFunc) {
      // Attribute iteration order is not defined for protocol buffer Map.
      // Process function attributes separately in the lexicographical order to
      // have deterministic order of functions in the constructed IR.
      funcs.emplace_back(&attr_name, &attr_value);
    } else {
      TF_ASSIGN_OR_RETURN(auto attr, ConvertAttributeValue(attr_value));
      result.attributes.push_back(builder_->getNamedAttr(attr_name, attr));
    }
  }

  auto comparator = [](const FuncPairType& a, const FuncPairType& b) {
    return *a.first < *b.first;
  };
  std::sort(funcs.begin(), funcs.end(), comparator);
  for (const auto& func : funcs) {
    TF_RETURN_IF_ERROR(ConvertFunctionCallAttribute(*func.first, *func.second,
                                                    &result.attributes));
  }

  result.attributes.push_back(builder_->getNamedAttr(
      "name", builder_->getStringAttr(std::string(node.name()))));
  result.attributes.push_back(builder_->getNamedAttr(
      "device", builder_->getStringAttr(std::string(node_def.device()))));

  node_values_[node.id()] = builder_->createOperation(result);
  return Status::OK();
}

// Add the backedges to the CFG. Given a backedge, we replace the original
// source and destination operations by two new operations. Most of the
// fields of the replacements are copied from the original operations.
// However,
// - for the src operation, one output is inserted to the front of the output
//   list. The type of the output is set to the type of the non-control result
//   of the dst operation, and
// - for the dst operation, one operand is inserted to the front of the
//   operand list. This operand is using the first result of the src
//   operation.
// TODO(fengliuai): Preserve the order of the results and operands if
// necessary.
Status Importer::AddBackedges() {
  for (auto& it : back_edge_dst_inputs_) {
    auto& back_edges = it.second;
    absl::c_stable_sort(back_edges, [](const BackEdge* e1, const BackEdge* e2) {
      return e1->dst_input < e2->dst_input;
    });
    for (const auto* edge : back_edges) {
      if (!edge->src->IsNextIteration() || !edge->dst->IsMerge()) {
        return errors::FailedPrecondition(
            "Invalid backedge; should be from NextIteration to Merge!");
      }
      auto* sink = node_values_[edge->src->id()];
      auto* dst = node_values_[edge->dst->id()];
      TF_RETURN_IF_ERROR(AddBackedge(sink, dst, edge->dst_input));
    }
  }
  return Status::OK();
}

Status Importer::AddBackedge(mlir::Operation* sink, mlir::Operation* dst,
                             int dst_input) {
  mlir::Operation* source = GetOrCreateNextIterationSource(sink, dst);

  // Adds the "source" to the operands of the dst by creating a new dst
  // operation.
  mlir::OperationState state(dst->getLoc(), dst->getName());
  auto num_operands = dst->getNumOperands();
  state.operands.reserve(num_operands + 1);
  for (int input = 0, e = num_operands + 1; input != e; ++input) {
    if (input < dst_input) {
      state.operands.push_back(dst->getOperand(input));
    } else if (input == dst_input) {
      state.operands.push_back(source->getResult(0));
    } else {
      state.operands.push_back(dst->getOperand(input - 1));
    }
  }
  state.attributes.append(dst->getAttrs().begin(), dst->getAttrs().end());
  for (auto* result : dst->getResults()) {
    state.types.push_back(result->getType());
  }
  builder_->setInsertionPoint(dst);
  auto* new_dst = builder_->createOperation(state);

  // Replaces the output uses of the old operation by the corresponding
  // result of the new operation, and deletes the old operation.
  for (unsigned i = 0, e = dst->getNumResults(); i != e; ++i) {
    auto* new_output = new_dst->getResult(i);
    dst->getResult(i)->replaceAllUsesWith(new_output);
  }
  dst->dropAllReferences();
  dst->erase();
  return Status::OK();
}

mlir::Operation* Importer::GetOrCreateNextIterationSource(
    mlir::Operation* sink, mlir::Operation* dst) {
  auto iter = next_iteration_sink_source_.find(sink);
  if (iter != next_iteration_sink_source_.end()) return iter->second;

  auto inst_name = sink->getName().getStringRef();
  inst_name.consume_back(".sink");
  mlir::OperationState src_state(sink->getLoc(),
                                 inst_name.str().append(".source"));
  src_state.attributes.append(sink->getAttrs().begin(), sink->getAttrs().end());
  src_state.types.push_back(dst->getResult(0)->getType());
  src_state.types.push_back(
      builder_->getType<mlir::TFControlFlow::TFControlType>());
  builder_->setInsertionPoint(dst->getBlock(), dst->getBlock()->begin());
  mlir::Operation* source = builder_->createOperation(src_state);
  next_iteration_sink_source_[sink] = source;
  return source;
}

Status Importer::Convert(llvm::StringRef func_name,
                         mlir::FunctionType func_type,
                         const absl::InlinedVector<OutputTensor, 4>& arg_nodes,
                         const absl::InlinedVector<OutputTensor, 4>& ret_nodes,
                         llvm::ArrayRef<mlir::NamedAttribute> attrs) {
  // TODO(b/122040776): Uses debug info for FunctionDef.
  auto* function = new mlir::Function(mlir::UnknownLoc::get(context_),
                                      func_name, func_type, attrs);

  module_->getFunctions().push_back(function);
  builder_ = absl::make_unique<mlir::OpBuilder>(function->getBody());
  // Seeds the builder with an initial block.
  auto* bb = builder_->createBlock();

  for (const Node* node : ordered_nodes_) {
    TF_RETURN_IF_ERROR(ConvertNode(*node));
  }

  // Adds the backedges back to the function by creating the source and sink
  // pairs.
  TF_RETURN_IF_ERROR(AddBackedges());

  return ConvertFunctionArgAndRets(bb, func_type.getInputs(), arg_nodes,
                                   ret_nodes);
}

StatusOr<mlir::FunctionType> Importer::InferMainFunctionType(
    absl::InlinedVector<OutputTensor, 4>* arg_nodes,
    absl::InlinedVector<OutputTensor, 4>* ret_nodes) {
  // Finds out all the input nodes and output nodes.
  if (!specs_.inputs.empty() || !specs_.output_arrays.empty()) {
    arg_nodes->resize(specs_.inputs.size());
    ret_nodes->resize(specs_.output_arrays_order.size());

    for (Node* n : ordered_nodes_) {
      // Handle inputs/arguments.
      auto input_it = specs_.inputs.find(n->name());
      if (input_it != specs_.inputs.end()) {
        (*arg_nodes)[std::distance(specs_.inputs.begin(), input_it)] = {n, 0};
      }

      // Handle outputs/returns.
      if (specs_.output_arrays.find(n->name()) != specs_.output_arrays.end()) {
        for (int i = 0, e = specs_.output_arrays_order.size(); i != e; ++i) {
          std::pair<std::string, std::string> name_and_port =
              absl::StrSplit(specs_.output_arrays_order[i], ':');
          auto name = name_and_port.first;
          if (name != n->name()) continue;
          int port = 0;
          if (!name_and_port.second.empty() &&
              !absl::SimpleAtoi(name_and_port.second, &port)) {
            return errors::InvalidArgument("Invalid port specification: ",
                                           specs_.output_arrays_order[i]);
          }
          (*ret_nodes)[i] = {n, port};
        }
      }
    }
  }

  int i = 0;
  for (auto it : specs_.inputs) {
    if (arg_nodes->at(i++).node == nullptr) {
      return errors::InvalidArgument("Input ", it.first,
                                     " was not found in graph");
    }
  }
  for (int i = 0, e = specs_.output_arrays_order.size(); i != e; ++i) {
    if (ret_nodes->at(i).node == nullptr) {
      return errors::InvalidArgument("Output ", specs_.output_arrays_order[i],
                                     " was not found in graph");
    }
  }

  // Starts to construct the function type.
  llvm::SmallVector<mlir::Type, 4> arg_types;
  llvm::SmallVector<mlir::Type, 4> ret_types;
  arg_types.reserve(specs_.inputs.size());
  ret_types.reserve(specs_.output_arrays.size());
  mlir::Builder builder(context_);

  // Input nodes as function arguments.
  for (const auto& input : specs_.inputs) {
    mlir::Type element_type;
    const auto& node_info = input.second;
    TF_RETURN_IF_ERROR(::tensorflow::ConvertDataType(node_info.imported_dtype,
                                                     builder, &element_type));
    llvm::SmallVector<int64_t, 4> shape;
    TF_RETURN_IF_ERROR(ConvertToMlirShape(node_info.shape, &shape));
    arg_types.push_back(builder.getTensorType(shape, element_type));
  }

  // Output nodes as function returns.
  for (const auto& ret : *ret_nodes) {
    if (ret.node->num_outputs() < 1) {
      return errors::FailedPrecondition(
          "Invalid output node; should have at least 1 output: " +
          ret.node->name());
    }
    auto* shape_context = shape_refiner_->GetExtendedContext(ret.node);
    TF_ASSIGN_OR_RETURN(auto type,
                        InferOutputType(shape_context, ret.index, builder));
    ret_types.push_back(type);
  }

  return builder.getFunctionType(arg_types, ret_types);
}

StatusOr<mlir::FunctionType> Importer::InferLibFunctionType(
    const FunctionBody& fbody) {
  mlir::Builder builder(context_);

  llvm::SmallVector<mlir::Type, 4> arg_types;
  arg_types.reserve(fbody.arg_types.size());
  for (auto dataType : fbody.arg_types) {
    mlir::Type element_type;
    TF_RETURN_IF_ERROR(ConvertDataType(dataType, builder, &element_type));
    // TODO(hinsu): Derive shape of function arguments based on shapes available
    // at call sites of this function. That way it is possible to have a
    // partially known shape in some cases instead of unranked tensor types.
    arg_types.push_back(builder.getTensorType(element_type));
  }

  llvm::SmallVector<mlir::Type, 4> ret_types;
  ret_types.reserve(fbody.ret_types.size());
  for (auto ret : fbody.ret_nodes) {
    // Find node in the graph using the node id instead of using `ret` directly
    // because the graph has been cloned.
    auto* node = graph_->FindNodeId(ret->id());
    auto* shape_context = shape_refiner_->GetExtendedContext(node);

    // Return type of the function is type of the only input of the respective
    // return node in the function.
    TF_ASSIGN_OR_RETURN(auto type,
                        InferInputType(shape_context, /*i=*/0, builder));
    ret_types.push_back(type);
  }

  return builder.getFunctionType(arg_types, ret_types);
}

StatusOr<std::unique_ptr<mlir::Module>> Importer::Convert(
    mlir::MLIRContext* context, const Graph& graph,
    const GraphDebugInfo& debug_info, const FunctionLibraryDefinition& flib_def,
    const NodeSpecs& specs) {
  auto module = absl::make_unique<mlir::Module>(context);
  std::unordered_map<std::string, std::string> tf_name_to_mlir_name;
  Importer importer(flib_def, debug_info, specs, module.get(),
                    &tf_name_to_mlir_name);
  TF_RETURN_IF_ERROR(importer.PrepareConvert(graph));

  // Collects the argument and return nodes by looking up the node names
  // specified by the user.
  absl::InlinedVector<OutputTensor, 4> arg_nodes;
  absl::InlinedVector<OutputTensor, 4> ret_nodes;
  TF_ASSIGN_OR_RETURN(auto func_type,
                      importer.InferMainFunctionType(&arg_nodes, &ret_nodes));

  // Record the input and output mapping.
  llvm::SmallVector<mlir::NamedAttribute, 1> attrs;
  if (!specs.inputs.empty() || !specs.output_arrays.empty()) {
    mlir::Builder b(context);
    std::string s;
    llvm::raw_string_ostream ss(s);
    mlir::interleaveComma(
        specs.inputs, ss,
        [&](const std::pair<std::string, ArrayInfo>& v) { ss << v.first; });
    auto inputs = b.getNamedAttr("inputs", b.getStringAttr(ss.str()));
    s.clear();
    mlir::interleaveComma(specs.output_arrays, ss,
                          [&](const std::string& v) { ss << v; });
    auto outputs = b.getNamedAttr("outputs", b.getStringAttr(ss.str()));

    attrs.push_back(b.getNamedAttr("tf.entry_function",
                                   b.getDictionaryAttr({inputs, outputs})));
  }

  TF_RETURN_IF_ERROR(
      importer.Convert("main", func_type, arg_nodes, ret_nodes, attrs));
  return module;
}
}  // namespace

StatusOr<std::unique_ptr<mlir::Module>> ConvertGraphdefToMlir(
    const GraphDef& graphdef, const GraphDebugInfo& debug_info,
    const NodeSpecs& specs, mlir::MLIRContext* context,
    bool add_default_attributes) {
  GraphConstructorOptions options;
  options.allow_internal_ops = true;
  Graph graph(OpRegistry::Global());

  GraphDef preprocessed_graphdef(graphdef);
  if (add_default_attributes) {
    TF_RETURN_IF_ERROR(AddDefaultsToNodeDef(&preprocessed_graphdef));
  }
  TF_RETURN_IF_ERROR(
      ConvertGraphDefToGraph(options, preprocessed_graphdef, &graph));

  return ConvertGraphToMlir(graph, debug_info, graph.flib_def(), specs,
                            context);
}

StatusOr<std::unique_ptr<mlir::Module>> ConvertGraphToMlir(
    const Graph& graph, const GraphDebugInfo& debug_info,
    const FunctionLibraryDefinition& flib_def, const NodeSpecs& specs,
    mlir::MLIRContext* context) {
  return Importer::Convert(context, graph, debug_info, flib_def, specs);
}

}  // namespace tensorflow
