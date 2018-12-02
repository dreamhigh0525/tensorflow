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

#ifndef TENSORFLOW_CORE_GRAPPLER_GRAPH_ANALYZER_GRAPH_ANALYZER_TOOL_H_
#define TENSORFLOW_CORE_GRAPPLER_GRAPH_ANALYZER_GRAPH_ANALYZER_TOOL_H_

#include "tensorflow/core/lib/strings/str_util.h"

namespace tensorflow {
namespace grappler {
namespace graph_analyzer {

void GraphAnalyzerTool(const string& file_name, int n);

}  // end namespace graph_analyzer
}  // end namespace grappler
}  // end namespace tensorflow

#endif  // TENSORFLOW_CORE_GRAPPLER_GRAPH_ANALYZER_GRAPH_ANALYZER_TOOL_H_
