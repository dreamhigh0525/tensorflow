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
"""Tests for AdaMax."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np

from tensorflow.contrib.opt.python.training import adamax
from tensorflow.python.client import session
from tensorflow.python.eager import context
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.framework import test_util
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import resource_variable_ops
from tensorflow.python.ops import variables
from tensorflow.python.platform import test


def adamax_update_numpy(param,
                      g_t,
                      t,
                      m,
                      v,
                      alpha=0.001,
                      beta1=0.9,
                      beta2=0.999,
                      epsilon=1e-8):
  m_t = beta1 * m + (1 - beta1) * g_t
  v_t = np.maximum(beta2 * v, np.abs(g_t))
  param_t = param - (alpha / (1 - beta1**t)) * m_t / v_t
  return param_t, m_t, v_t


class AdaMaxOptimizerTest(test.TestCase):

  def doTestBasic(self, use_resource=False):
    for i, dtype in enumerate([dtypes.half, dtypes.float32, dtypes.float64]):
      with self.test_session(graph=ops.Graph()):
        # Initialize variables for numpy implementation.
        m0, v0, m1, v1 = 0.0, 0.0, 0.0, 0.0
        var0_np = np.array([1.0, 2.0], dtype=dtype.as_numpy_dtype)
        grads0_np = np.array([0.1, 0.1], dtype=dtype.as_numpy_dtype)
        var1_np = np.array([3.0, 4.0], dtype=dtype.as_numpy_dtype)
        grads1_np = np.array([0.01, 0.01], dtype=dtype.as_numpy_dtype)

        if use_resource:
          var0 = resource_variable_ops.ResourceVariable(
              var0_np, name="var0_%d" % i)
          var1 = resource_variable_ops.ResourceVariable(
              var1_np, name="var1_%d" % i)
        else:
          var0 = variables.Variable(var0_np)
          var1 = variables.Variable(var1_np)
        grads0 = constant_op.constant(grads0_np)
        grads1 = constant_op.constant(grads1_np)

        opt = adamax.AdaMaxOptimizer()
        update = opt.apply_gradients(zip([grads0, grads1], [var0, var1]))
        opt_variables = opt.variables()
        beta1_power, beta2_power = opt._get_beta_accumulators()
        self.assertTrue(beta1_power is not None)
        self.assertTrue(beta2_power is not None)
        self.assertIn(beta1_power, opt_variables)
        self.assertIn(beta2_power, opt_variables)

        with ops.Graph().as_default():
          # Shouldn't return non-slot variables from other graphs.
          self.assertEqual(0, len(opt.variables()))

        if context.in_graph_mode():
          self.evaluate(variables.global_variables_initializer())
          # Fetch params to validate initial values
          self.assertAllClose([1.0, 2.0], self.evaluate(var0))
          self.assertAllClose([3.0, 4.0], self.evaluate(var1))

        beta1_power, beta2_power = opt._get_beta_accumulators()

        # Run 3 steps of Adam
        for t in range(1, 4):
          if context.in_graph_mode():
            self.evaluate(update)
          elif t > 1:
            opt.apply_gradients(zip([grads0, grads1], [var0, var1]))

          self.assertAllCloseAccordingToType(0.9**(t + 1),
                                             self.evaluate(beta1_power))
          self.assertAllCloseAccordingToType(0.999**(t + 1),
                                             self.evaluate(beta2_power))

          var0_np, m0, v0 = adamax_update_numpy(var0_np, grads0_np, t, m0, v0)
          var1_np, m1, v1 = adamax_update_numpy(var1_np, grads1_np, t, m1, v1)

          # Validate updated params
          self.assertAllCloseAccordingToType(var0_np, self.evaluate(var0))
          self.assertAllCloseAccordingToType(var1_np, self.evaluate(var1))
          if use_resource:
            self.assertEqual("var0_%d/Adam:0" % (i,),
                             opt.get_slot(var=var0, name="m").name)

  def testBasic(self):
    with self.test_session():
      self.doTestBasic(use_resource=False)


if __name__ == "__main__":
  test.main()
