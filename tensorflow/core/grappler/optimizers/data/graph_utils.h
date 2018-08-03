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

#ifndef TENSORFLOW_CORE_GRAPPLER_OPTIMIZERS_DATA_GRAPH_UTILS_H_
#define TENSORFLOW_CORE_GRAPPLER_OPTIMIZERS_DATA_GRAPH_UTILS_H_

#include "tensorflow/core/framework/attr_value.pb.h"
#include "tensorflow/core/framework/function.pb.h"
#include "tensorflow/core/framework/graph.pb.h"
#include "tensorflow/core/framework/node_def.pb.h"
#include "tensorflow/core/framework/tensor.pb.h"
#include "tensorflow/core/framework/tensor_shape.pb.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/grappler/mutable_graph_view.h"
#include "tensorflow/core/grappler/utils.h"
#include "tensorflow/core/lib/core/errors.h"

namespace tensorflow {
namespace grappler {
namespace graph_utils {

// Adds a node to the graph.
NodeDef* AddNode(const string& name, const string& op,
                 const std::vector<string>& inputs,
                 const std::vector<std::pair<string, AttrValue>>& attributes,
                 MutableGraphView* graph);

// Adds a Const node with the given value to the graph.
template <typename T>
NodeDef* AddScalarConstNode(T v, MutableGraphView* graph) {
  // is_same is an idiomatic hack for making it compile if not instantiated.
  // Replacing with false will result in a compile-time error.
  static_assert(!std::is_same<T, T>::value,
                "Invalid specialization of this method for type T.");
  return {};
}

template <>
NodeDef* AddScalarConstNode(bool v, MutableGraphView* graph);
template <>
NodeDef* AddScalarConstNode(double v, MutableGraphView* graph);
template <>
NodeDef* AddScalarConstNode(float v, MutableGraphView* graph);
template <>
NodeDef* AddScalarConstNode(int v, MutableGraphView* graph);
template <>
NodeDef* AddScalarConstNode(int64 v, MutableGraphView* graph);
template <>
NodeDef* AddScalarConstNode(StringPiece v, MutableGraphView* graph);

// Checks whether the two graphs are the same.
bool Compare(const GraphDef& g1, const GraphDef& g2);

// Checks whether the graph contains a node with the given name.
bool ContainsGraphNodeWithName(const string& name, const GraphDef& graph);

// Checks whether the library contains a function with the given name.
bool ContainsGraphFunctionWithName(const string& name,
                                   const FunctionDefLibrary& library);

// Checks whether the function contains a node with the given name.
bool ContainsFunctionNodeWithName(const string& name,
                                  const FunctionDef& function);

// Checks whether the graph contains a node with the given op.
bool ContainsNodeWithOp(const string& op, const GraphDef& graph);

// Returns the index of the node with the given name or -1 if the node does
// not exist.
int FindGraphNodeWithName(const string& name, const GraphDef& graph);

// Returns the index of the function with the given name or -1 if the function
// does not exist.
int FindGraphFunctionWithName(const string& name,
                              const FunctionDefLibrary& library);

// Returns the index of the function node with the given name or -1 if the
// function node does not exist.
int FindFunctionNodeWithName(const string& name, const FunctionDef& function);

// Returns the index of the first node with the given op or -1 if no such  node
// exists.
int FindNodeWithOp(const string& op, const GraphDef& graph);

// Returns the list of indices of all nodes with the given op or empty list if
// no such node exists.
std::vector<int> FindAllGraphNodesWithOp(const string& op,
                                         const GraphDef& graph);

// Sets the node name using `prefix` as a prefix while guaranteeing the name
// is unique across the graph.
void SetUniqueGraphNodeName(const string& prefix, GraphDef* graph,
                            NodeDef* node);

// Sets the function node name using the `prefix` as a prefix while guaranteeing
// the name is unique across the functions nodes.
void SetUniqueFunctionNodeName(const string& prefix, FunctionDef* function,
                               NodeDef* node);

// Sets the node name using the `prefix` name as a prefix while guaranteeing the
// name is unique across the graph.
void SetUniqueGraphFunctionName(const string& prefix,
                                FunctionDefLibrary* library,
                                FunctionDef* function);

}  // end namespace graph_utils
}  // end namespace grappler
}  // end namespace tensorflow

#endif  // TENSORFLOW_CORE_GRAPPLER_OPTIMIZERS_DATA_GRAPH_UTILS_H_
