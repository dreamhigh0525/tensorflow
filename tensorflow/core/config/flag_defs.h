/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_CORE_CONFIG_FLAG_DEFS_H_
#define TENSORFLOW_CORE_CONFIG_FLAG_DEFS_H_

#include "tensorflow/core/config/flags.h"

namespace tensorflow {
namespace flags {

class Flags {
 public:
  // Test only flags. See flags_test.cc for example usage.
  TF_DECLARE_FLAG(test_only_experiment_1, true, "Test only experiment 1.");
  TF_DECLARE_FLAG(test_only_experiment_2, false, "Test only experiment 2.");

  // Declare flags below here.
  // LINT.IfChange
  TF_DECLARE_FLAG(enable_nested_function_shape_inference, false,
                  "Allow ops such as tf.cond to invoke the ShapeRefiner on "
                  "their nested functions.");
  TF_DECLARE_FLAG(enable_quantized_dtypes_training, false,
                  "Set quantized dtypes, like tf.qint8, to be trainable.");
  TF_DECLARE_FLAG(graph_building_optimization, false,
                  "Optimize graph building for faster tf.function tracing.");
  TF_DECLARE_FLAG(
      op_building_optimization, true,
      "Optimize tf.Operation building for faster tf.function tracing.");
  TF_DECLARE_FLAG(saved_model_fingerprinting, false,
                  "Add fingerprint to SavedModels.");
  // LINT.ThenChange(//tensorflow/core/config/flags_api_wrapper.cc)
};

Flags& Global();

}  // namespace flags
}  // namespace tensorflow

#endif  // TENSORFLOW_CORE_CONFIG_FLAG_DEFS_H_
