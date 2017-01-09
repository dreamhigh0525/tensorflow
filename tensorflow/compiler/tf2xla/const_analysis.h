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

#ifndef TENSORFLOW_COMPILER_TF2XLA_CONST_ANALYSIS_H_
#define TENSORFLOW_COMPILER_TF2XLA_CONST_ANALYSIS_H_

#include <vector>

#include "tensorflow/core/graph/graph.h"
#include "tensorflow/core/lib/core/status.h"

namespace tensorflow {

// Backwards dataflow analysis that finds arguments (_Arg nodes) to a graph that
// must be compile-time constants.
Status BackwardsConstAnalysis(const Graph& graph,
                              std::vector<bool>* compile_time_const_args);

}  // namespace tensorflow

#endif  // TENSORFLOW_COMPILER_TF2XLA_CONST_ANALYSIS_H_
