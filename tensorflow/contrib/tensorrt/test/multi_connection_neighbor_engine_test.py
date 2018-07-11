# Copyright 2018 The TensorFlow Authors. All Rights Reserved.
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
"""Model script to test TF-TensorRT integration."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np

from tensorflow.core.protobuf import config_pb2
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import nn
from tensorflow.python.ops import gen_math_ops
from tensorflow.python.ops import math_ops
from tensorflow.contrib.tensorrt.test.base_unit_test import BaseUnitTest


class MultiConnectionNeighborEngineTest(BaseUnitTest):
  """Multi connection neighboring nodes wiring tests in TF-TRT"""

  def __init__(self, log_file='log.txt'):
    super(MultiConnectionNeighborEngineTest, self).__init__()
    self.static_mode_list = {"FP32", "FP16"}
    self.debug = True
    self.dynamic_mode_list = {}
    self.inp_dims = (2, 3, 7, 5)
    self.dummy_input = np.random.normal(1.0, 0.5, self.inp_dims)
    self.get_network = self.neighboring_tensor_test
    self.expect_nb_nodes = 7
    self.log_file = log_file
    self.test_name = self.__class__.__name__
    self.allclose_rtol = 0.05
    self.allclose_atol = 0.05

  def neighboring_tensor_test(self):
    g = ops.Graph()
    gpu_options = config_pb2.GPUOptions(per_process_gpu_memory_fraction=0.50)
    sessconfig = config_pb2.ConfigProto(gpu_options=gpu_options)
    with g.as_default():
      x = array_ops.placeholder(
          dtype=dtypes.float32, shape=self.inp_dims, name="input")
      e = constant_op.constant(
          np.random.normal(.05, .005, [3, 2, 3, 4]),
          name="weights",
          dtype=dtypes.float32)
      conv = nn.conv2d(
          input=x,
          filter=e,
          data_format="NCHW",
          strides=[1, 1, 1, 1],
          padding="VALID",
          name="conv")
      b = constant_op.constant(
          np.random.normal(2.0, 1.0, [1, 4, 1, 1]),
          name="bias",
          dtype=dtypes.float32)
      t = conv + b

      b = constant_op.constant(
          np.random.normal(5.0, 1.0, [1, 4, 1, 1]),
          name="bias",
          dtype=dtypes.float32)
      q = conv - b
      edge = math_ops.sigmoid(q)

      b = constant_op.constant(
          np.random.normal(5.0, 1.0, [1, 4, 1, 1]),
          name="bias",
          dtype=dtypes.float32)
      d = b + conv
      edge3 = math_ops.sigmoid(d)

      c = constant_op.constant(
          np.random.normal(1.0, 1.0, [1, 4, 1, 1]),
          name="bias",
          dtype=dtypes.float32)
      edge1 = gen_math_ops.tan(conv)
      t = t - edge1
      q = q + edge
      t = t + q
      t = t + d
      t = t - edge3
      array_ops.squeeze(t, name="output")

    return g.as_graph_def()
