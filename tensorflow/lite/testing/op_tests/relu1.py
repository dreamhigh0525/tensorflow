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
"""Test configs for relu1."""
import numpy as np
import tensorflow as tf
from tensorflow.lite.testing.zip_test_utils import create_tensor_data
from tensorflow.lite.testing.zip_test_utils import make_zip_of_tests
from tensorflow.lite.testing.zip_test_utils import register_make_test_function


@register_make_test_function()
def make_relu1_tests(options):
  """Make a set of tests to do relu1."""

  # Chose a set of parameters
  test_parameters = [{
      "input_shape": [[], [1, 1, 1, 1], [1, 3, 4, 3], [3, 15, 14, 3]],
      "fully_quantize": [True, False],
      "input_range": [(-2, 8)]
  }]

  def build_graph(parameters):
    input_tensor = tf.compat.v1.placeholder(
        dtype=tf.float32, name="input", shape=parameters["input_shape"])
    # Note that the following is not supported:
    #   out = tf.maximum(-1.0, tf.minimum(input_tensor, 1.0))
    out = tf.minimum(1.0, tf.maximum(input_tensor, -1.0))
    return [input_tensor], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    min_value, max_value = parameters["input_range"]
    input_values = create_tensor_data(
        np.float32, parameters["input_shape"], min_value, max_value)
    return [input_values], sess.run(
        outputs, feed_dict=dict(zip(inputs, [input_values])))

  make_zip_of_tests(options, test_parameters, build_graph, build_inputs)
