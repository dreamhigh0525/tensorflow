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
"""Test conversion of graphs involving INT32 tensors and operations."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np

from tensorflow.python.compiler.tensorrt.test import tf_trt_integration_test_base as trt_test
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import dtypes
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import nn
from tensorflow.python.platform import test


class ExcludeUnsupportedInt32Test(trt_test.TfTrtIntegrationTestBase):
  """Test exclusion of ops which are not supported in INT32 mode by TF-TRT"""

  def _ConstOp(self, shape, dtype):
    return constant_op.constant(np.random.randn(*shape), dtype=dtype)

  def GraphFn(self, x):
    dtype = x.dtype
    b = self._ConstOp((4, 10), dtype)
    x = math_ops.matmul(x, b)
    b = self._ConstOp((10,), dtype)
    x = nn.bias_add(x, b)
    return array_ops.identity(x, name='output_0')

  def GetParams(self):
    return self.BuildParams(self.GraphFn, dtypes.int32, [[100, 4]], [[100, 10]])

  def GetConversionParams(self, run_params):
    """Return a ConversionParams for test."""
    conversion_params = super(ExcludeUnsupportedInt32Test,
                              self).GetConversionParams(run_params)
    conversion_params._replace(max_batch_size=100, maximum_cached_engines=1)
    rewrite_config_with_trt = self.GetTrtRewriterConfig(
        run_params=run_params,
        conversion_params=conversion_params,
        # Disable layout optimizer, since it will convert BiasAdd with NHWC
        # format to NCHW format under four dimensional input.
        disable_non_trt_optimizers=True)
    return conversion_params._replace(
        rewriter_config_template=rewrite_config_with_trt)

  def ExpectedEnginesToBuild(self, run_params):
    """Return the expected engines to build."""
    return []


class CalibrationInt32Support(trt_test.TfTrtIntegrationTestBase):
  """Test execution of calibration with int32 input"""

  def GraphFn(self, inp):
    # Can use any op that is converted to TRT with int32 inputs
    inp_transposed = array_ops.transpose(inp, [0, 3, 2, 1], name='transpose_0')
    return array_ops.identity(inp_transposed, name='output_0')

  def GetParams(self):
    return self.BuildParams(self.GraphFn, dtypes.int32, [[3, 4, 5, 6]],
                            [[3, 6, 5, 4]])

  def ShouldRunTest(self, run_params):
    # Although test passes with all configurations but only
    # execute INT8 with use_calibration=True because
    # that is the purpose of the test.
    return trt_test.IsQuantizationWithCalibration(
        run_params), 'test calibration and INT8'

  def ExpectedEnginesToBuild(self, run_params):
    return ['TRTEngineOp_0']


if __name__ == '__main__':
  test.main()
