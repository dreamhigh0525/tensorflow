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
"""Test configs for transpose_conv."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np
import tensorflow as tf
from tensorflow.lite.testing.zip_test_utils import create_tensor_data
from tensorflow.lite.testing.zip_test_utils import make_zip_of_tests
from tensorflow.lite.testing.zip_test_utils import register_make_test_function


# Since compute output_shape is fairly complicated for
# tf.nn.conv2d_transpose input_sizes argument, so we here first perform a
# "conv2d" operation to get the output, then we use the output to feed in
# tf.nn.conv2d_backprop_input.
# This test will depend on the "conv2d" operation's correctness.
@register_make_test_function()
def make_transpose_conv_tests(options):
  """Make a set of tests to do transpose_conv."""

  # Tensorflow only supports equal strides
  test_parameters = [{
      "input_shape": [[1, 3, 4, 1], [1, 10, 10, 3], [3, 20, 20, 1]],
      "filter_size": [[1, 1], [1, 2], [3, 3]],
      "strides": [[1, 1, 1, 1], [1, 3, 3, 1]],
      "padding": ["SAME", "VALID"],
      "data_format": ["NHWC"],
      "channel_multiplier": [1, 2],
  }]

  def get_tensor_shapes(parameters):
    input_shape = parameters["input_shape"]
    filter_size = parameters["filter_size"]
    filter_shape = filter_size + [
        input_shape[3], parameters["channel_multiplier"]
    ]
    return [input_shape, filter_shape]

  def build_graph(parameters):
    """Build a transpose_conv graph given `parameters`."""
    input_shape, filter_shape = get_tensor_shapes(parameters)
    input_tensor = tf.compat.v1.placeholder(
        dtype=tf.float32, name="input", shape=input_shape)

    filter_input = tf.compat.v1.placeholder(
        dtype=tf.float32, name="filter", shape=filter_shape)

    conv_outputs = tf.nn.conv2d(
        input_tensor,
        filter_input,
        strides=parameters["strides"],
        padding=parameters["padding"],
        data_format=parameters["data_format"])
    out = tf.compat.v1.nn.conv2d_backprop_input(
        input_shape,
        filter_input,
        conv_outputs,
        strides=parameters["strides"],
        padding=parameters["padding"],
        data_format=parameters["data_format"])
    input_tensors = [input_tensor, filter_input]
    return input_tensors, [out]

  def build_inputs(parameters, sess, inputs, outputs):
    input_shape, filter_shape = get_tensor_shapes(parameters)
    values = [
        create_tensor_data(np.float32, input_shape),
        create_tensor_data(np.float32, filter_shape)
    ]
    return values, sess.run(outputs, feed_dict=dict(zip(inputs, values)))

  make_zip_of_tests(options, test_parameters, build_graph, build_inputs)
