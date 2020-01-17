# Copyright 2019 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================

# RUN: %p/multi_variables_v1 | FileCheck %s

# pylint: disable=missing-docstring,line-too-long
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import tensorflow.compat.v1 as tf
from tensorflow.compiler.mlir.tensorflow.tests.tf_saved_model import common_v1

# Verify that the tf.versions attribute exists. It is difficult to enforce
# contents, since the version numbers change over time. The conversion logic
# itself is verified in the common graphdef converter, so here just assert
# it is being invoked.
# CHECK: module
# CHECK-SAME: tf.versions
# CHECK-SAME: bad_consumers
# CHECK-SAME: min_consumer
# CHECK-SAME: producer

# CHECK: "tf_saved_model.global_tensor"() {is_mutable, sym_name = "y", type = tensor<1x3xf32>, value = {{.*}} : tensor<1x3xf32>} : () -> ()
# CHECK: "tf_saved_model.global_tensor"() {is_mutable, sym_name = "z", type = tensor<3x3xf32>, value = {{.*}} : tensor<3x3xf32>} : () -> ()
# CHECK: func @basic([[ARG0:%.*]]: tensor<3x1xf32>,
# CHECK-SAME: [[ARG1:%.*]]: tensor<!tf.resource<tensor<1x3xf32>>> {tf_saved_model.bound_input = @y}
# CHECK-SAME: [[ARG2:%.*]]: tensor<!tf.resource<tensor<3x3xf32>>> {tf_saved_model.bound_input = @z}) -> tensor<3x3xf32>
# CHECK-NEXT: [[R0:%.*]] = "tf.ReadVariableOp"([[ARG1]]) {{{.*}}} : (tensor<!tf.resource<tensor<1x3xf32>>>) -> tensor<1x3xf32>
# CHECK-NEXT: [[R1:%.*]] = "tf.MatMul"([[ARG0]], [[R0]]) {{{.*}}} : (tensor<3x1xf32>, tensor<1x3xf32>) -> tensor<3x3xf32>
# CHECK-NEXT: [[R2:%.*]] = "tf.ReadVariableOp"([[ARG2]]) {{{.*}}} : (tensor<!tf.resource<tensor<3x3xf32>>>) -> tensor<3x3xf32>
# CHECK-NEXT: [[R3:%.*]] = "tf.MatMul"([[R1]], [[R2]]) {{{.*}}} : (tensor<3x3xf32>, tensor<3x3xf32>) -> tensor<3x3xf32>
# CHECK-NEXT: return [[R3]] : tensor<3x3xf32>


def Test():

  # Default TF1.x uses reference variables that are not supported by SavedModel
  # v1 Importer. To use SavedModel V1 Importer, resource variables should be
  # enabled.
  tf.compat.v1.enable_resource_variables()

  tf.compat.v1.disable_eager_execution()

  x = tf.constant([[1.0], [1.0], [1.0]])
  y = tf.compat.v1.get_variable(
      name='y',
      shape=(1, 3),
      initializer=tf.random_normal_initializer(),
      trainable=True)
  z = tf.compat.v1.get_variable(
      name='z',
      shape=(3, 3),
      initializer=tf.random_normal_initializer(),
      trainable=True)
  r = tf.matmul(x, y)
  s = tf.matmul(r, z)

  tensor_info_x = tf.compat.v1.saved_model.utils.build_tensor_info(x)
  tensor_info_s = tf.compat.v1.saved_model.utils.build_tensor_info(s)

  return {
      'basic':
          (tf.compat.v1.saved_model.signature_def_utils.build_signature_def(
              inputs={'x': tensor_info_x},
              outputs={'s': tensor_info_s},
              method_name=tf.saved_model.PREDICT_METHOD_NAME))
  }


if __name__ == '__main__':
  common_v1.do_test(Test())
