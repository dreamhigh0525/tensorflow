# Copyright 2020 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================
"""Tests for V2 Collective Operations."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.python.eager import context
from tensorflow.python.eager import def_function
from tensorflow.python.framework import config
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import ops
from tensorflow.python.framework import test_util
from tensorflow.python.ops import gen_collective_ops
from tensorflow.python.platform import test


class CollectiveOpsTest(test.TestCase):

  def _setup_context(self, num_cpus=2):
    context._reset_context()
    cpus = config.list_physical_devices('CPU')
    self.assertEqual(len(cpus), 1)
    config.set_logical_device_configuration(cpus[0], [
        context.LogicalDeviceConfiguration(),
        context.LogicalDeviceConfiguration()
    ])
    context.ensure_initialized()

  @test_util.run_v2_only
  def testReduceV2(self):
    self._setup_context()

    @def_function.function
    def single_all_reduce(in_value, group_size, group_key, instance_key):
      return gen_collective_ops.collective_reduce_v2(
          in_value, group_size, group_key, instance_key, merge_op='Add',
          final_op='Id', communication_hint='auto')

    @def_function.function
    def run_all_reduce_1cpu():
      with ops.device('/device:CPU:0'):
        in_value = constant_op.constant([1.])
        group_size = constant_op.constant(1)
        group_key = constant_op.constant(1)
        instance_key = constant_op.constant(1)
        return single_all_reduce(in_value, group_size, group_key, instance_key)

    @def_function.function
    def run_all_reduce_2cpus():
      in_value = constant_op.constant([1.])
      group_size = constant_op.constant(2)
      group_key = constant_op.constant(2)
      instance_key = constant_op.constant(2)
      collectives = []
      with ops.device('/device:CPU:0'):
        collectives.append(single_all_reduce(in_value, group_size, group_key,
                                             instance_key))
      with ops.device('/device:CPU:1'):
        collectives.append(single_all_reduce(in_value, group_size, group_key,
                                             instance_key))
      return collectives

    self.assertAllClose(run_all_reduce_1cpu(), [1.], rtol=1e-5, atol=1e-5)
    for result in run_all_reduce_2cpus():
      self.assertAllClose(result, [2.], rtol=1e-5, atol=1e-5)


if __name__ == '__main__':
  test.main()
