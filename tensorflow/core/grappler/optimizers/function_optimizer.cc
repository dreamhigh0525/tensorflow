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

#include "tensorflow/core/grappler/optimizers/function_optimizer.h"
#include <unordered_map>
#include "tensorflow/core/common_runtime/device_mgr.h"
#include "tensorflow/core/common_runtime/function.h"
#include "tensorflow/core/common_runtime/process_function_library_runtime.h"
#include "tensorflow/core/framework/function.h"
#include "tensorflow/core/framework/function.pb.h"
#include "tensorflow/core/framework/graph_def_util.h"
#include "tensorflow/core/framework/node_def.pb.h"
#include "tensorflow/core/framework/op_def.pb.h"
#include "tensorflow/core/framework/versions.pb.h"
#include "tensorflow/core/graph/graph_constructor.h"
#include "tensorflow/core/grappler/grappler_item.h"
#include "tensorflow/core/grappler/op_types.h"
#include "tensorflow/core/grappler/utils.h"
#include "tensorflow/core/grappler/utils/functions.h"
#include "tensorflow/core/lib/gtl/map_util.h"

namespace tensorflow {
namespace grappler {
namespace {

// Mark functions that were created as a result of function specialization.
constexpr char kGrapplerSpecializedFuncAttr[] = "_GrapplerSpecializedFunc";

constexpr char kNoInlineAttr[] = "_noinline";

bool AttrIsTrue(const FunctionDef& func, const string& attr) {
  return func.attr().count(attr) != 0 && func.attr().at(attr).b();
}

bool MarkedSpecialized(const FunctionDef& func) {
  return AttrIsTrue(func, kGrapplerSpecializedFuncAttr);
}

bool MarkedNoInline(const FunctionDef& func) {
  return AttrIsTrue(func, kNoInlineAttr);
}

// Find unique name for the specialized function. Collision can happen if
// specialized function is instantiated for the nodes with the same name (e.g.
// inside function body of two different functions).
string UniqueSpecializedFunctionName(const FunctionDef& func,
                                     const NodeDef& func_node,
                                     const FunctionLibraryDefinition& flib) {
  using str_util::StringReplace;
  using strings::StrCat;

  string specialized_name = StrCat(func.signature().name(), "_specialized_for_",
                                   StringReplace(func_node.name(), "/", "_",
                                                 /*replace_all*/ true));
  string unique_name = specialized_name;

  int idx = 0;
  while (flib.Find(unique_name)) {
    unique_name = strings::StrCat(specialized_name, "_", ++idx);
  }
  return unique_name;
}

class FunctionOptimizerContext {
 public:
  explicit FunctionOptimizerContext(const GrapplerItem& item,
                                    RewriterConfig::Toggle opt_level)
      : opt_level_(opt_level),
        function_library_(FunctionLibraryDefinition(OpRegistry::Global(),
                                                    item.graph.library())) {
    InitializeInlinedFunctions(item);
  }

  const FunctionLibraryDefinition& function_library() const {
    return function_library_;
  }

  FunctionLibraryDefinition& mutable_function_library() {
    return function_library_;
  }

  bool IsInlinedFunction(const string& name) const {
    return inlined_functions_.count(name) > 0;
  }

  // Find inlining candidate by name. Return nullptr if not found.
  const FunctionDef* FindInlinedFunction(const string& name) const {
    return gtl::FindWithDefault(inlined_functions_, name, nullptr);
  }

 private:
  void InitializeInlinedFunctions(const GrapplerItem& item) {
    bool aggressive = opt_level_ == RewriterConfig::AGGRESSIVE;

    for (const FunctionDef& func : item.graph.library().function()) {
      // Can't create IdentityN nodes with no input or output: skip these
      // functions for now.
      if (func.signature().input_arg_size() == 0 ||
          func.signature().output_arg_size() == 0) {
        continue;
      }
      bool marked_noinline = MarkedNoInline(func);
      bool marked_specialized = MarkedSpecialized(func);

      if (!marked_specialized && (!marked_noinline || aggressive)) {
        inlined_functions_[func.signature().name()] = &func;
      }
    }
  }

  RewriterConfig::Toggle opt_level_;
  FunctionLibraryDefinition function_library_;
  // Functions that can be inlined into optimized graph.
  std::unordered_map<string, const FunctionDef*> inlined_functions_;

  TF_DISALLOW_COPY_AND_ASSIGN(FunctionOptimizerContext);
};

Status SpecializeFunction(const NodeDef& func_node, const FunctionDef& func,
                          FunctionOptimizerContext* ctx,
                          GraphDef* optimized_graph) {
  const std::unordered_map<string, AttrValue> func_attr(
      func_node.attr().begin(), func_node.attr().end());

  const auto& flib = ctx->function_library();

  // Make a GrapplerFunctionItem and immediately convert it back to FunctionDef.
  GrapplerFunctionItem item;
  TF_RETURN_IF_ERROR(MakeGrapplerFunctionItem(func, func_attr, flib, &item));

  // TODO(ezhulenev): Push down const inputs and known input shapes.
  FunctionDef specialized;
  TF_RETURN_IF_ERROR(MakeSpecializedFunctionDef(item, flib, &specialized));

  // Find a name for specialized function.
  const string specialized_func_name =
      UniqueSpecializedFunctionName(func, func_node, flib);

  specialized.mutable_signature()->set_name(specialized_func_name);
  auto* specialized_attr = specialized.mutable_attr();
  (*specialized_attr)[kGrapplerSpecializedFuncAttr].set_b(true);

  // Add specialized function to the library.
  TF_RETURN_IF_ERROR(
      ctx->mutable_function_library().AddFunctionDef(specialized));

  // Add a function call node for the specialized function.
  NodeDef* specialized_func_node = optimized_graph->add_node();
  *specialized_func_node = func_node;
  specialized_func_node->set_op(specialized_func_name);

  return Status::OK();
}

// Copy input/output argument type to the type_list. Return error if argument
// type is not explicitly defined, and not specified in function attributes.
Status CopyArgType(const NodeDef& func_node,
                   const std::unordered_map<string, AttrValue>& func_attr,
                   const string& arg_kind, const OpDef::ArgDef& arg,
                   AttrValue::ListValue* type_list) {
  if (arg.type() != DT_INVALID) {
    type_list->add_type(arg.type());
  } else {
    auto it = func_attr.find(arg.type_attr());
    if (it == func_attr.end() || it->second.type() == DT_INVALID) {
      return errors::InvalidArgument(
          "Invalid ", arg_kind, " argument ", arg.name(), " for function ",
          func_node.op(), " instantiated by ", func_node.name());
    }
    type_list->add_type(it->second.type());
  }
  return Status::OK();
}

// Add an IdentityN op to hook the function inputs to: this ensures that
// they're all evaluated before the evaluation of the function body starts.
Status HookInlinedFunctionInputs(
    const NodeDef& func_node, const FunctionDef& func,
    const std::unordered_map<string, AttrValue>& func_attr, NodeDef* inputs) {
  inputs->set_name(strings::StrCat(func_node.name(), "/", "inlined_inputs"));
  inputs->set_op("IdentityN");
  inputs->set_device(func_node.device());
  *inputs->mutable_input() = func_node.input();
  AttrValue::ListValue* type_list =
      (*inputs->mutable_attr())["T"].mutable_list();
  for (const OpDef::ArgDef& arg : func.signature().input_arg()) {
    TF_RETURN_IF_ERROR(
        CopyArgType(func_node, func_attr, "input", arg, type_list));
  }
  return Status::OK();
}

// Add an IdentityN op to hook the function outputs to: this ensures that the
// function body is fully evaluated before its fanout gets scheduled.
Status HookInlinedFunctionOutputs(
    const NodeDef& func_node, const FunctionDef& func,
    const std::unordered_map<string, AttrValue>& func_attr,
    const gtl::ArraySlice<string> fetch, NodeDef* outputs) {
  outputs->set_name(func_node.name());
  outputs->set_op("IdentityN");
  outputs->set_device(func_node.device());
  AttrValue::ListValue* type_list =
      (*outputs->mutable_attr())["T"].mutable_list();
  for (int i = 0; i < func.signature().output_arg_size(); ++i) {
    const OpDef::ArgDef& arg = func.signature().output_arg(i);
    TF_RETURN_IF_ERROR(
        CopyArgType(func_node, func_attr, "output", arg, type_list));
    // Use the fetch names since they take into account the output mapping.
    outputs->add_input(strings::StrCat(func_node.name(), "/", fetch[i]));
  }
  return Status::OK();
}

Status InlineFunction(const NodeDef& func_node, const FunctionDef& func,
                      const FunctionOptimizerContext& ctx,
                      GraphDef* optimized_graph) {
  const std::unordered_map<string, AttrValue> func_attr(
      func_node.attr().begin(), func_node.attr().end());

  GrapplerFunctionItem item;
  Status item_status =
      MakeGrapplerFunctionItem(func, func_attr, ctx.function_library(), &item);

  if (!item_status.ok()) {
    return errors::InvalidArgument("Failed to inline function ", func_node.op(),
                                   " instantiated by ", func_node.name(),
                                   ". Error: ", item_status.error_message());
  }

  std::unordered_map<string, int> input_nodes;
  for (int i = 0; i < func.signature().input_arg_size(); ++i) {
    const OpDef::ArgDef& arg = func.signature().input_arg(i);
    input_nodes[arg.name()] = i;
  }

  // Hook inlined function inputs to IdentityN node
  NodeDef* func_inputs = optimized_graph->add_node();
  TF_RETURN_IF_ERROR(
      HookInlinedFunctionInputs(func_node, func, func_attr, func_inputs));

  for (NodeDef& func_body_node : *item.mutable_function_body().mutable_node()) {
    if (input_nodes.find(func_body_node.name()) != input_nodes.end()) {
      CHECK_EQ(0, func_body_node.input_size());
      // Turn input placeholders into identity nodes
      if (IsPlaceholder(func_body_node)) {
        func_body_node.set_op("Identity");
      }
      int input_id = input_nodes[func_body_node.name()];
      func_body_node.add_input(
          strings::StrCat(func_inputs->name(), ":", input_id));
    } else {
      // Update the input names if any.
      for (string& input : *func_body_node.mutable_input()) {
        input = AddPrefixToNodeName(input, /*prefix=*/func_node.name());
      }
      // If the node has no input, make hook it up to the func_inputs node to
      // ensure it runs in the same frame as the other nodes of the function
      // body.
      if (func_body_node.input_size() == 0) {
        *func_body_node.add_input() = AsControlDependency(func_inputs->name());
      }
    }

    // Add the node name as a prefix to avoid collisions after inlining
    func_body_node.set_name(
        strings::StrCat(func_node.name(), "/", func_body_node.name()));

    // Make sure the node is placed
    func_body_node.set_device(func_node.device());

    // Check if a body node is itself a function
    const FunctionDef* func_body_node_func =
        ctx.FindInlinedFunction(func_body_node.op());
    if (func_body_node_func != nullptr) {
      // Recursively inline function calls
      TF_RETURN_IF_ERROR(InlineFunction(func_body_node, *func_body_node_func,
                                        ctx, optimized_graph));
    } else {
      // Annotate the node with the function attributes.
      for (const auto& attr : func.attr()) {
        func_body_node.mutable_attr()->insert(attr);
      }
      // Move the node to the main graph
      optimized_graph->add_node()->Swap(&func_body_node);
    }
  }

  // Hook inlined function outputs to IdentityN node
  NodeDef* func_outputs = optimized_graph->add_node();
  std::vector<string> fetch = OutputTensors(item);
  TF_RETURN_IF_ERROR(HookInlinedFunctionOutputs(func_node, func, func_attr,
                                                fetch, func_outputs));

  return Status::OK();
}

class FakeCPUDevice : public Device {
 public:
  FakeCPUDevice(Env* env, const DeviceAttributes& attr) : Device(env, attr) {}
  Status Sync() override { return Status::OK(); }
};

class SymbolicGradientEnv {
 public:
  SymbolicGradientEnv(int graph_version, const FunctionDefLibrary& library)
      : graph_version_(graph_version), library_(library) {}

  FunctionLibraryDefinition* function_library() {
    InitializeIfNeeded();
    return fld_.get();
  }
  FunctionLibraryRuntime* function_library_runtime() {
    InitializeIfNeeded();
    return flr_;
  }

 private:
  // This initialization is expensive. Do it lazily to avoid paying for it
  // unless it's needed.
  void InitializeIfNeeded() {
    if (flr_) {
      return;
    }
    Env* env = Env::Default();
    DeviceAttributes attr;
    attr.set_name("/device:CPU:0");
    attr.set_device_type("CPU");
    FakeCPUDevice* dev = new FakeCPUDevice(env, attr);
    std::vector<Device*> devices;
    devices.push_back(dev);
    dvc_mgr_.reset(new DeviceMgr(devices));
    fld_.reset(new FunctionLibraryDefinition(OpRegistry::Global(), library_));
    OptimizerOptions optimizer_opts;
    optimizer_opts.set_do_function_inlining(true);
    pflr_.reset(new ProcessFunctionLibraryRuntime(
        dvc_mgr_.get(), env, graph_version_, fld_.get(), optimizer_opts));
    flr_ = pflr_->GetFLR(dev->name());
  }

  const int graph_version_;
  const FunctionDefLibrary& library_;
  std::unique_ptr<DeviceMgr> dvc_mgr_;
  std::unique_ptr<FunctionLibraryDefinition> fld_;
  std::unique_ptr<ProcessFunctionLibraryRuntime> pflr_;
  FunctionLibraryRuntime* flr_ = nullptr;
};

Status InlineSymbolicGradient(const NodeDef& node, SymbolicGradientEnv* env,
                              GraphDef* inlined_graph) {
  GraphDef graph_def;

  // Create a node to anchor the gradient inputs
  NodeDef* inlined_input = graph_def.add_node();
  inlined_input->set_name("FunctionInputs");
  inlined_input->set_op("IdentityN");
  AttrValue::ListValue* type_list =
      (*inlined_input->mutable_attr())["T"].mutable_list();
  for (const auto& type : node.attr().at("Tin").list().type()) {
    type_list->add_type(static_cast<DataType>(type));
  }

  // Add the gradient node
  NodeDef* inlined = graph_def.add_node();
  *inlined = node;
  inlined->clear_input();
  for (int i = 0; i < node.attr().at("Tin").list().type_size(); ++i) {
    inlined->add_input(strings::StrCat(inlined_input->name(), ":", i));
  }

  // Create a node to anchor the gradient outputs
  NodeDef* inlined_output = graph_def.add_node();
  inlined_output->set_name("FunctionOutputs");
  inlined_output->set_op("IdentityN");
  type_list = (*inlined_output->mutable_attr())["T"].mutable_list();
  for (const auto& type : node.attr().at("Tout").list().type()) {
    type_list->add_type(static_cast<DataType>(type));
  }
  for (int i = 0; i < node.attr().at("Tout").list().type_size(); ++i) {
    inlined_output->add_input(strings::StrCat(inlined->name(), ":", i));
  }

  // Convert the graphdef to a graph
  GraphConstructorOptions graph_ctor_opts;
  graph_ctor_opts.allow_internal_ops = true;
  graph_ctor_opts.expect_device_spec = false;
  Graph graph(env->function_library());
  TF_RETURN_IF_ERROR(
      ConvertGraphDefToGraph(graph_ctor_opts, graph_def, &graph));

  // Recursively inline the functions until there is nothing more to inline. We
  // should at least expand one function.
  int counter = 0;
  while (counter < 50 &&
         ExpandInlineFunctions(env->function_library_runtime(), &graph)) {
    ++counter;
  }

  GraphDef inlined_graph_def;
  graph.ToGraphDef(&inlined_graph_def);

  // Add the default values of attributes to the nodes that have been inlined.
  TF_RETURN_IF_ERROR(AddDefaultAttrsToGraphDef(&inlined_graph_def,
                                               *graph.op_registry(), 0, true));

  // Add the inlined nodes to the graph
  for (NodeDef& inlined_node : *inlined_graph_def.mutable_node()) {
    if (inlined_node.name() == "FunctionOutputs") {
      inlined_node.set_name(node.name());
      for (int i = 0; i < inlined_node.input_size(); ++i) {
        inlined_node.set_input(
            i, AddPrefixToNodeName(inlined_node.input(i), node.name()));
      }
    } else if (inlined_node.name() == "FunctionInputs") {
      inlined_node.set_name(
          AddPrefixToNodeName(inlined_node.name(), node.name()));
      inlined_node.clear_input();
      for (int i = 0; i < node.input_size(); ++i) {
        inlined_node.add_input(node.input(i));
      }
    } else {
      inlined_node.set_name(
          AddPrefixToNodeName(inlined_node.name(), node.name()));
      for (int i = 0; i < inlined_node.input_size(); ++i) {
        inlined_node.set_input(
            i, AddPrefixToNodeName(inlined_node.input(i), node.name()));
      }
      // If the node has no input, hook it up to the function input node to make
      // sure it runs in the same frame as the other nodes of the function body.
      if (inlined_node.input_size() == 0) {
        *inlined_node.add_input() = AsControlDependency(
            AddPrefixToNodeName("FunctionInputs", node.name()));
      }
    }
    inlined_node.set_device(node.device());
    inlined_graph->add_node()->Swap(&inlined_node);
  }

  return Status::OK();
}

}  // namespace

Status FunctionOptimizer::Optimize(Cluster* cluster, const GrapplerItem& item,
                                   GraphDef* optimized_graph) {
  // Nothing to do here.
  if (item.graph.library().function_size() == 0) {
    *optimized_graph = item.graph;
    return Status::OK();
  }

  FunctionOptimizerContext ctx(item, opt_level_);
  SymbolicGradientEnv env(item.graph.versions().producer(),
                          item.graph.library());

  bool inline_gradients = options_.enable_symbolic_gradient_inlining;
  bool inline_func = options_.enable_function_inlining;
  bool specialize_func = options_.enable_function_specialization;

  for (const NodeDef& node : item.graph.node()) {
    const string func_name = node.op();

    if (func_name == "SymbolicGradient" && inline_gradients) {
      // Inline symbolic gradients only if the corresponding function is inlined
      const auto* f_attr = gtl::FindOrNull(node.attr(), "f");
      string f_name = f_attr != nullptr ? f_attr->func().name() : "";
      if (ctx.IsInlinedFunction(f_name)) {
        TF_RETURN_IF_ERROR(InlineSymbolicGradient(node, &env, optimized_graph));
        continue;
      }
    }

    const FunctionDef* func = ctx.function_library().Find(func_name);
    if (func != nullptr) {
      if (inline_func && ctx.IsInlinedFunction(func_name)) {
        // Inline function body into the optimized graph}
        TF_RETURN_IF_ERROR(InlineFunction(node, *func, ctx, optimized_graph));
        continue;
      }

      if (specialize_func && IsParametrized(*func)) {
        // TODO(ezhulenev): Specialize function call if input is a Const or has
        // a known shape. Const input tensors can be pushed into the function
        // body and removed from function inputs.

        // Specialize function body for its instantiation attributes and inputs.
        TF_RETURN_IF_ERROR(
            SpecializeFunction(node, *func, &ctx, optimized_graph));
        continue;
      }
    }

    // If we reached this point, node was not handled by any of the stages
    // (inline, specialize), simply add a copy to the graph.
    *optimized_graph->add_node() = node;
  }

  // TODO(bsteiner): trim the library to remove unused function definitions
  *optimized_graph->mutable_versions() = item.graph.versions();
  *optimized_graph->mutable_library() = ctx.function_library().ToProto();

  return Status::OK();
}

void FunctionOptimizer::Feedback(Cluster* cluster, const GrapplerItem& item,
                                 const GraphDef& optimized_graph,
                                 double result) {
  // Nothing to do for FunctionOptimizer.
}

}  // end namespace grappler
}  // end namespace tensorflow
