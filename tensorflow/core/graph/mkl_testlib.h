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

#ifndef TENSORFLOW_CORE_GRAPH_MKL_TESTLIB_H_
#define TENSORFLOW_CORE_GRAPH_MKL_TESTLIB_H_

#ifdef INTEL_MKL

#include "tensorflow/core/graph/graph.h"

namespace tensorflow {
namespace test {
namespace graph {

// Adds a _MklMatmul node in g doing in0.contract(in1).
Node* oneDNNMatmul(Graph* g, Node* in0, Node* in1, bool transpose_a,
                   bool transpose_b);

}  // namespace graph
}  // namespace test
}  // namespace tensorflow

#endif  // INTEL_MKL
#endif  // TENSORFLOW_CORE_GRAPH_MKL_TESTLIB_H_
