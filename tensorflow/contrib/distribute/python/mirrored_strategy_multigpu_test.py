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
"""Multi-GPU tests for MirroredStrategy."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import sys

import numpy as np

from tensorflow.contrib.distribute.python import mirrored_strategy
from tensorflow.contrib.distribute.python import multi_worker_test_base
from tensorflow.contrib.distribute.python import strategy_test_lib
from tensorflow.core.protobuf import config_pb2
from tensorflow.python.data.ops import dataset_ops
from tensorflow.python.distribute import reduce_util
from tensorflow.python.distribute import values
from tensorflow.python.eager import backprop
from tensorflow.python.eager import context
from tensorflow.python.eager import function
from tensorflow.python.eager import test
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.framework import test_util
from tensorflow.python.keras.engine import training as keras_training
from tensorflow.python.keras.layers import core as keras_core
from tensorflow.python.layers import core
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import rnn
from tensorflow.python.ops import rnn_cell_impl
from tensorflow.python.ops import state_ops
from tensorflow.python.ops import variable_scope
from tensorflow.python.ops import variables
from tensorflow.python.training import device_util
from tensorflow.python.training import distribution_strategy_context as ds_context
from tensorflow.python.training import gradient_descent
from tensorflow.python.training import optimizer as optimizer_lib
from tensorflow.python.training import server_lib


GPU_TEST = "test_gpu" in sys.argv[0]


class MirroredTwoDeviceDistributionTest(strategy_test_lib.DistributionTestBase):

  def _get_distribution_strategy(self):
    devices = ["/device:CPU:0", "/device:GPU:0"]
    if GPU_TEST:
      self.assertGreater(context.num_gpus(), 0)
      if context.num_gpus() > 1:
        devices = ["/device:GPU:0", "/device:GPU:1"]
    print(self.id().split(".")[-1], "devices:", ", ".join(devices))
    return mirrored_strategy.MirroredStrategy(devices)

  def testMinimizeLossEager(self):
    if not GPU_TEST:
      self.skipTest("Not GPU test")
    self._test_minimize_loss_eager(self._get_distribution_strategy())

  def testMinimizeLossGraph(self):
    soft_placement = not GPU_TEST
    print("testMinimizeLossGraph soft_placement:", soft_placement)
    self._test_minimize_loss_graph(
        self._get_distribution_strategy(), soft_placement=soft_placement)

  def testReplicaId(self):
    if not GPU_TEST:
      self.skipTest("Not GPU test")
    self._test_replica_id(self._get_distribution_strategy())

  def testNumReplicasInSync(self):
    if not GPU_TEST:
      self.skipTest("Not GPU test")
    self.assertEqual(2, self._get_distribution_strategy().
                     num_replicas_in_sync)

  @test_util.run_in_graph_and_eager_modes
  def testCallAndMergeExceptions(self):
    if not GPU_TEST:
      self.skipTest("Not GPU test")
    self._test_call_and_merge_exceptions(self._get_distribution_strategy())

  @test_util.run_in_graph_and_eager_modes
  def testRunRegroupError(self):

    def run_fn():
      replica_id = int(self.evaluate(_replica_id()))
      # Generates a list with different lengths on different devices.
      # Will fail in _regroup() (if more than one device).
      return list(range(replica_id))

    dist = self._get_distribution_strategy()
    with dist.scope(), self.assertRaises(AssertionError):
      dist.call_for_each_replica(run_fn)

  @test_util.run_in_graph_and_eager_modes
  def testReduceToCpu(self):
    if not GPU_TEST:
      self.skipTest("Not GPU test")

    dist = self._get_distribution_strategy()
    with dist.scope():
      result = dist.call_for_each_replica(_replica_id)
      reduced = dist.reduce(
          reduce_util.ReduceOp.SUM,
          result,
          destinations="/device:CPU:0")
      unwrapped = dist.unwrap(reduced)
      self.assertEqual(1, len(unwrapped))
      expected = sum(range(dist.num_replicas_in_sync))
      self.assertEqual(expected, self.evaluate(unwrapped[0]))

  @test_util.run_in_graph_and_eager_modes()
  def testReduceToMultipleDestinations(self):
    if not GPU_TEST:
      self.skipTest("Not GPU test")

    devices = ["/device:GPU:0"]
    if GPU_TEST:
      self.assertGreater(context.num_gpus(), 0)
    print(self.id().split(".")[-1], "devices:", ", ".join(devices))

    dist = mirrored_strategy.MirroredStrategy(devices)
    with dist.scope():
      reduced = dist.reduce(
          reduce_util.ReduceOp.SUM,
          1.0,
          destinations=["/device:CPU:0", "/device:GPU:0"])
      unwrapped = dist.unwrap(reduced)
      self.assertEqual(2, len(unwrapped))
      self.assertEqual(1.0, self.evaluate(unwrapped[0]))


class MirroredStrategyVariableCreationTest(test.TestCase):

  config = config_pb2.ConfigProto()
  config.allow_soft_placement = True

  def _skip_eager_if_gpus_less_than(self, num_gpus):
    if context.num_gpus() < num_gpus and context.executing_eagerly():
      self.skipTest("Enough GPUs not available for this test in eager mode.")

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testSingleVariable(self):
    self._skip_eager_if_gpus_less_than(1)

    def model_fn():
      # This variable should be created only once across the threads because of
      # special variable_creator functions used by `dist.call_for_each_replica`.
      v = variable_scope.variable(1.0, name="foo")
      ds_context.get_replica_context().merge_call(lambda _: _)
      return v

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with dist.scope():
      result = dist.call_for_each_replica(model_fn)
      self.assertIsInstance(result, values.MirroredVariable)
      self.assertEquals("foo:0", result.name)

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testUnnamedVariable(self):
    self._skip_eager_if_gpus_less_than(1)

    def model_fn():
      v = variable_scope.variable(1.0)
      ds_context.get_replica_context().merge_call(lambda _: _)
      return v

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with dist.scope():
      result = dist.call_for_each_replica(model_fn)
      self.assertIsInstance(result, values.MirroredVariable)
      # Default name of "Variable" will be used.
      self.assertEquals("Variable:0", result.name)

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testMultipleVariables(self):
    self._skip_eager_if_gpus_less_than(1)

    def model_fn():
      vs = []
      for i in range(5):
        vs.append(variable_scope.variable(1.0, name="foo" + str(i)))
      ds_context.get_replica_context().merge_call(lambda _: _)
      return vs

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with dist.scope():
      result = dist.call_for_each_replica(model_fn)
      for i, v in enumerate(result):
        self.assertIsInstance(v, values.MirroredVariable)
        self.assertEquals("foo" + str(i) + ":0", v.name)

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testMultipleVariablesWithSameCanonicalName(self):
    self._skip_eager_if_gpus_less_than(1)

    def model_fn():
      vs = []
      vs.append(variable_scope.variable(1.0, name="foo/bar"))
      vs.append(variable_scope.variable(1.0, name="foo_1/bar"))
      vs.append(variable_scope.variable(1.0, name="foo_1/bar_1"))
      vs.append(variable_scope.variable(1.0, name="foo/bar_1"))
      ds_context.get_replica_context().merge_call(lambda _: _)
      return vs

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with dist.scope():
      result = dist.call_for_each_replica(model_fn)
      for v in result:
        self.assertIsInstance(v, values.MirroredVariable)
      self.assertEquals(4, len(result))
      self.assertEquals("foo/bar:0", result[0].name)
      self.assertEquals("foo_1/bar:0", result[1].name)
      self.assertEquals("foo_1/bar_1:0", result[2].name)
      self.assertEquals("foo/bar_1:0", result[3].name)

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testVariableWithSameCanonicalNameAcrossThreads(self):
    self._skip_eager_if_gpus_less_than(1)

    def model_fn():
      replica_id = self.evaluate(_replica_id())
      v = variable_scope.variable(1.0, name="foo_" + str(replica_id))
      ds_context.get_replica_context().merge_call(lambda _: _)
      return v

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with dist.scope():
      result = dist.call_for_each_replica(model_fn)
      self.assertIsInstance(result, values.MirroredVariable)
      # The resulting mirrored variable will use the name from the first device.
      self.assertEquals("foo_0:0", result.name)

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testWithLayers(self):
    self._skip_eager_if_gpus_less_than(1)
    def model_fn(features):
      with variable_scope.variable_scope("common"):
        layer1 = core.Dense(1)
        layer1(features)
        layer2 = core.Dense(1)
        layer2(features)
        # This will pause the current thread, and execute the other thread.
        ds_context.get_replica_context().merge_call(lambda _: _)
        layer3 = core.Dense(1)
        layer3(features)
        return [(layer1.kernel, layer1.bias),
                (layer2.kernel, layer2.bias),
                (layer3.kernel, layer3.bias)]

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])
    ds = dist.distribute_dataset(
        lambda: dataset_ops.Dataset.from_tensors([[1.]]).repeat(10))
    if context.executing_eagerly():
      iterator = ds.make_one_shot_iterator()
    else:
      iterator = ds.make_initializable_iterator()
      self.evaluate([iterator.initializer])

    features = iterator.get_next()

    with dist.scope():
      result = dist.call_for_each_replica(model_fn, args=(features,))
      suffixes = ["", "_1", "_2"]
      for (kernel, bias), suffix in zip(result, suffixes):
        self.assertIsInstance(kernel, values.MirroredVariable)
        self.assertEquals("common/dense" + suffix + "/kernel:0", kernel.name)
        self.assertIsInstance(bias, values.MirroredVariable)
        self.assertEquals("common/dense" + suffix + "/bias:0", bias.name)

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testWithVariableAndVariableScope(self):
    self._skip_eager_if_gpus_less_than(1)

    def model_fn():
      v0 = variable_scope.variable(1.0, name="var0", aggregation=None)
      with variable_scope.variable_scope("common"):
        v1 = variable_scope.variable(1.0, name="var1")
        # This will pause the current thread, and execute the other thread.
        ds_context.get_replica_context().merge_call(lambda _: _)
        v2 = variable_scope.variable(
            1.0,
            name="var2",
            synchronization=variable_scope.VariableSynchronization.ON_READ,
            aggregation=variable_scope.VariableAggregation.SUM)
        v3 = variable_scope.variable(
            1.0,
            name="var3",
            synchronization=variable_scope.VariableSynchronization.ON_WRITE,
            aggregation=variable_scope.VariableAggregation.MEAN)

      return v0, v1, v2, v3

    devices = ["/device:CPU:0", "/device:GPU:0"]
    dist = mirrored_strategy.MirroredStrategy(devices)
    with dist.scope():
      v = variable_scope.variable(1.0, name="var-main0")
      self.assertEquals("var-main0:0", v.name)

      result = dist.call_for_each_replica(model_fn)
      self.assertEquals(4, len(result))
      v0, v1, v2, v3 = result
      self.assertIsInstance(v0, values.MirroredVariable)
      self.assertEquals("var0:0", v0.name)
      self.assertIsInstance(v1, values.MirroredVariable)
      self.assertEquals("common/var1:0", v1.name)
      self.assertIsInstance(v2, values.ReplicaLocalVariable)
      self.assertEquals("common/var2:0", v2.name)
      self.assertEquals(variable_scope.VariableAggregation.SUM, v2.aggregation)
      self.assertIsInstance(v3, values.MirroredVariable)
      self.assertEquals("common/var3:0", v3.name)
      self.assertEquals(variable_scope.VariableAggregation.MEAN, v3.aggregation)

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testWithGetVariableAndVariableScope(self):
    self._skip_eager_if_gpus_less_than(1)

    def model_fn():
      v0 = variable_scope.get_variable("var0", [1])
      with variable_scope.variable_scope("common"):
        v1 = variable_scope.get_variable("var1", [1])
        # This will pause the current thread, and execute the other thread.
        ds_context.get_replica_context().merge_call(lambda _: _)
        v2 = variable_scope.get_variable(
            "var2", [1],
            synchronization=variable_scope.VariableSynchronization.ON_READ,
            aggregation=variable_scope.VariableAggregation.SUM)
        v3 = variable_scope.get_variable(
            "var3", [1],
            synchronization=variable_scope.VariableSynchronization.ON_WRITE,
            aggregation=variable_scope.VariableAggregation.MEAN)

      return v0, v1, v2, v3

    devices = ["/device:CPU:0", "/device:GPU:0"]
    dist = mirrored_strategy.MirroredStrategy(devices)
    with dist.scope():
      with variable_scope.variable_scope("main"):
        v = variable_scope.get_variable("var-main0", [1])
        self.assertEquals("main/var-main0:0", v.name)

        result = dist.call_for_each_replica(model_fn)
        self.assertEquals(4, len(result))
        v0, v1, v2, v3 = result
        self.assertIsInstance(v0, values.MirroredVariable)
        self.assertEquals("main/var0:0", v0.name)
        self.assertIsInstance(v1, values.MirroredVariable)
        self.assertEquals("main/common/var1:0", v1.name)
        self.assertIsInstance(v2, values.ReplicaLocalVariable)
        self.assertEquals("main/common/var2:0", v2.name)
        self.assertEquals(variable_scope.VariableAggregation.SUM,
                          v2.aggregation)
        self.assertIsInstance(v3, values.MirroredVariable)
        self.assertEquals("main/common/var3:0", v3.name)
        self.assertEquals(variable_scope.VariableAggregation.MEAN,
                          v3.aggregation)

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testOnlyFirstReplicaUpdatesVariables(self):
    self._skip_eager_if_gpus_less_than(1)

    def create_fn():
      aggregation = variable_scope.VariableAggregation.ONLY_FIRST_REPLICA
      v0 = variable_scope.variable(
          2.0,
          name="on_read",
          synchronization=variable_scope.VariableSynchronization.ON_READ,
          aggregation=aggregation)
      v1 = variable_scope.variable(
          3.0,
          name="on_write",
          synchronization=variable_scope.VariableSynchronization.ON_WRITE,
          aggregation=aggregation)
      return v0, v1

    devices = ["/device:GPU:0", "/device:CPU:0"]
    dist = mirrored_strategy.MirroredStrategy(devices)
    with dist.scope():
      v0, v1 = dist.call_for_each_replica(create_fn)
      self.evaluate(v0.initializer)
      self.assertEqual(2.0, self.evaluate(v0.get(devices[0])))
      self.assertEqual(2.0, self.evaluate(v0.get(devices[1])))
      self.assertEqual(2.0, self.evaluate(dist.read_var(v0)))
      self.evaluate(v1.initializer)
      self.assertEqual(3.0, self.evaluate(v1.get(devices[0])))
      self.assertEqual(3.0, self.evaluate(v1.get(devices[1])))
      self.assertEqual(3.0, self.evaluate(dist.read_var(v1)))

      def replica_id_plus_one():
        return math_ops.cast(_replica_id() + 1, dtype=dtypes.float32)

      # Update using the assign_add member function.
      def update_member_fn():
        update0 = v0.assign_add(5.0 * replica_id_plus_one())
        update1 = v1.assign_add(7.0 * replica_id_plus_one())
        return update0, update1

      update0a, update1a = dist.call_for_each_replica(update_member_fn)

      # Update "sync on read" variable.
      self.evaluate(dist.group(update0a))
      self.assertEqual(2.0 + 5.0, self.evaluate(v0.get(devices[0])))
      # Writes are not synchronized for "sync on read" variables,
      # so device[1] can end up with a different value.
      self.assertEqual(2.0 + 2*5.0, self.evaluate(v0.get(devices[1])))
      # Always reads from device 0.
      self.assertEqual(2.0 + 5.0, self.evaluate(dist.read_var(v0)))

      # Update "sync on write" variable.
      self.evaluate(dist.group(update1a))
      self.assertEqual(3.0 + 7.0, self.evaluate(v1.get(devices[0])))
      # Writes are synchronized for v1, only the argument to assign_add on
      # device[0] is used.
      self.assertEqual(3.0 + 7.0, self.evaluate(v1.get(devices[1])))
      self.assertEqual(3.0 + 7.0, self.evaluate(dist.read_var(v1)))

      # Update using state_ops.assign_add global function.
      def update_state_ops_fn():
        update0 = state_ops.assign_add(v0, 11.0 * replica_id_plus_one())
        update1 = state_ops.assign_add(v1, 13.0 * replica_id_plus_one())
        return update0, update1

      update0b, update1b = dist.call_for_each_replica(update_state_ops_fn)
      self.evaluate(dist.group(update0b))

      # Update "sync on read" variable.
      self.assertEqual(2.0 + 5.0 + 11.0, self.evaluate(v0.get(devices[0])))
      self.assertEqual(2.0 + 2*5.0 + 2*11.0, self.evaluate(v0.get(devices[1])))
      self.assertEqual(2.0 + 5.0 + 11.0, self.evaluate(dist.read_var(v0)))

      # Update "sync on write" variable.
      self.evaluate(dist.group(update1b))
      self.assertEqual(3.0 + 7.0 + 13.0, self.evaluate(v1.get(devices[0])))
      self.assertEqual(3.0 + 7.0 + 13.0, self.evaluate(v1.get(devices[1])))
      self.assertEqual(3.0 + 7.0 + 13.0, self.evaluate(dist.read_var(v1)))

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testNoneSynchronizationWithGetVariable(self):
    self._skip_eager_if_gpus_less_than(1)
    devices = ["/device:CPU:0", "/device:GPU:0"]
    dist = mirrored_strategy.MirroredStrategy(devices)
    with dist.scope():
      with self.assertRaisesRegexp(
          ValueError, "`NONE` variable synchronization mode is not "
          "supported with `Mirrored` distribution strategy. Please change "
          "the `synchronization` for variable: v"):
        variable_scope.get_variable(
            "v", [1],
            synchronization=variable_scope.VariableSynchronization.NONE)

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testNoneSynchronizationWithVariable(self):
    self._skip_eager_if_gpus_less_than(1)
    devices = ["/device:CPU:0", "/device:GPU:0"]
    dist = mirrored_strategy.MirroredStrategy(devices)
    with dist.scope():
      with self.assertRaisesRegexp(
          ValueError, "`NONE` variable synchronization mode is not "
          "supported with `Mirrored` distribution strategy. Please change "
          "the `synchronization` for variable: v"):
        variable_scope.variable(
            1.0,
            name="v",
            synchronization=variable_scope.VariableSynchronization.NONE)

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testInvalidSynchronizationWithVariable(self):
    self._skip_eager_if_gpus_less_than(1)
    devices = ["/device:CPU:0", "/device:GPU:0"]
    dist = mirrored_strategy.MirroredStrategy(devices)
    with dist.scope():
      with self.assertRaisesRegexp(
          ValueError, "Invalid variable synchronization mode: Invalid for "
          "variable: v"):
        variable_scope.variable(1.0, name="v", synchronization="Invalid")

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testInvalidAggregationWithGetVariable(self):
    self._skip_eager_if_gpus_less_than(1)
    devices = ["/device:CPU:0", "/device:GPU:0"]
    dist = mirrored_strategy.MirroredStrategy(devices)
    with dist.scope():
      with self.assertRaisesRegexp(
          ValueError, "Invalid variable aggregation mode: invalid for "
          "variable: v"):
        variable_scope.get_variable(
            "v", [1],
            synchronization=variable_scope.VariableSynchronization.ON_WRITE,
            aggregation="invalid")

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testInvalidAggregationWithVariable(self):
    self._skip_eager_if_gpus_less_than(1)
    devices = ["/device:CPU:0", "/device:GPU:0"]
    dist = mirrored_strategy.MirroredStrategy(devices)
    with dist.scope():
      with self.assertRaisesRegexp(
          ValueError, "Invalid variable aggregation mode: invalid for "
          "variable: v"):
        variable_scope.variable(
            1.0,
            name="v",
            synchronization=variable_scope.VariableSynchronization.ON_WRITE,
            aggregation="invalid")

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testThreeDevices(self):
    self._skip_eager_if_gpus_less_than(2)

    def model_fn():
      v = variable_scope.variable(1.0, name="foo")
      ds_context.get_replica_context().merge_call(lambda _: _)
      return v

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:GPU:1", "/device:CPU:0"])

    with dist.scope():
      result = dist.call_for_each_replica(model_fn)
      self.assertIsInstance(result, values.MirroredVariable)
      self.assertEquals("foo:0", result.name)

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testNonMatchingVariableCreation(self):
    self._skip_eager_if_gpus_less_than(1)

    def model_fn(name):
      v = variable_scope.variable(1.0, name=name)
      ds_context.get_replica_context().merge_call(lambda _: _)
      return v

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with dist.scope():
      names = values.DistributedValues({
          "/device:CPU:0": "foo",
          "/device:GPU:0": "bar"
      })
      with self.assertRaises(RuntimeError):
        _ = dist.call_for_each_replica(model_fn, args=(names,))

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testReplicaLocalVariable(self):
    self._skip_eager_if_gpus_less_than(1)

    all_v_sum = {}
    all_v_mean = {}
    components_sum = {}
    components_mean = {}

    def model_fn():
      replica_id = self.evaluate(_replica_id())
      v_sum = variable_scope.variable(
          1.0,
          synchronization=variable_scope.VariableSynchronization.ON_READ,
          aggregation=variable_scope.VariableAggregation.SUM)
      v_mean = variable_scope.variable(
          4.0,
          synchronization=variable_scope.VariableSynchronization.ON_READ,
          aggregation=variable_scope.VariableAggregation.MEAN)
      self.assertTrue(isinstance(v_sum, values.ReplicaLocalVariable))
      self.assertTrue(isinstance(v_mean, values.ReplicaLocalVariable))
      updates = [v_sum.assign_add(2.0 + replica_id),
                 v_mean.assign(6.0 * replica_id)]
      all_v_sum[replica_id] = v_sum
      all_v_mean[replica_id] = v_mean
      c_sum = v_sum.get()
      c_mean = v_mean.get()
      components_sum[replica_id] = c_sum
      components_mean[replica_id] = c_mean
      self.assertIsNot(v_sum, c_sum)
      self.assertIsNot(v_mean, c_mean)
      return updates, v_sum, v_mean, c_sum, c_mean

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with dist.scope():
      # Create "sum" and "mean" versions of ReplicaLocalVariables.
      ret_ops, ret_v_sum, ret_v_mean, regrouped_sum, regrouped_mean = (
          dist.call_for_each_replica(model_fn))
      # Should see the same wrapping instance in all replicas.
      self.assertIs(all_v_sum[0], ret_v_sum)
      self.assertIs(all_v_mean[0], ret_v_mean)
      self.assertIs(all_v_sum[0], all_v_sum[1])
      self.assertIs(all_v_mean[0], all_v_mean[1])

      # Regroup should recover the same wrapper.
      self.assertIs(ret_v_sum, regrouped_sum)
      self.assertIs(ret_v_mean, regrouped_mean)
      self.assertIsNot(components_sum[0], components_sum[1])
      self.assertIsNot(components_mean[0], components_mean[1])

      # Apply updates
      self.evaluate(variables.global_variables_initializer())
      self.evaluate([y for x in ret_ops for y in dist.unwrap(x)])
      expected_sum = 0.0
      expected_mean = 0.0
      for i, d in enumerate(dist.extended.worker_devices):
        # Should see different values on different devices.
        v_sum_value = self.evaluate(ret_v_sum.get(d).read_value())
        v_mean_value = self.evaluate(ret_v_mean.get(d).read_value())
        expected = i + 3.0
        self.assertEqual(expected, v_sum_value)
        expected_sum += expected
        expected = i * 6.0
        self.assertEqual(expected, v_mean_value)
        expected_mean += expected
      expected_mean /= len(dist.extended.worker_devices)

      # Without get(device), should return the value you get by
      # applying the reduction across all replicas (whether you use
      # read_var(), get(), or nothing).
      self.assertEqual(expected_sum, self.evaluate(dist.read_var(ret_v_sum)))
      self.assertEqual(expected_mean, self.evaluate(dist.read_var(ret_v_mean)))
      self.assertEqual(expected_sum, self.evaluate(ret_v_sum.get()))
      self.assertEqual(expected_mean, self.evaluate(ret_v_mean.get()))
      self.assertEqual(expected_sum, self.evaluate(ret_v_sum))
      self.assertEqual(expected_mean, self.evaluate(ret_v_mean))

  # NOTE(priyag): Names and name scopes are ignored in eager, hence we are not
  # testing this in eager mode.

  def testNameScope(self):
    def model_fn():
      with ops.name_scope("foo"):
        a = constant_op.constant(1.0, name="a")
        ds_context.get_replica_context().merge_call(lambda _: _)
        b = constant_op.constant(1.0, name="b")
      return a, b

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with context.graph_mode(), dist.scope():
      with ops.name_scope("main"):
        result = dist.call_for_each_replica(model_fn)
        self.assertEquals(2, len(result))
        for v, name in zip(result, ["a", "b"]):
          self.assertIsInstance(v, values.DistributedValues)
          v0, v1 = dist.unwrap(v)
          self.assertEquals("main/foo/" + name + ":0", v0.name)
          self.assertEquals("main/replica_1/foo/" + name + ":0", v1.name)

  def testWithDefaultName(self):
    def model_fn():
      with ops.name_scope(None, "foo"):
        a = constant_op.constant(1.0, name="a")
        ds_context.get_replica_context().merge_call(lambda _: _)
        b = constant_op.constant(2.0, name="b")
      return a, b

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with context.graph_mode(), dist.scope():
      result = dist.call_for_each_replica(model_fn)
      self.assertEquals(2, len(result))
      for v, name in zip(result, ["a", "b"]):
        self.assertIsInstance(v, values.DistributedValues)
        v0, v1 = dist.unwrap(v)
        self.assertEquals("foo/" + name + ":0", v0.name)
        self.assertEquals("replica_1/foo/" + name + ":0", v1.name)

  # variable_scope.variable() respects name scopes when creating
  # variables. On the other hand variable_scope.get_variable() ignores name
  # scopes when creating variables. We test both methods of creating variables
  # to make sure that we have the same variable names in both cases.
  def testNameScopeWithVariable(self):
    def in_cross_replica(_):
      c = variable_scope.variable(1.0, name="c")
      return c

    def model_fn():
      b = variable_scope.variable(1.0, name="b")
      with ops.name_scope("foo"):
        c = ds_context.get_replica_context().merge_call(in_cross_replica)
      return b, c

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with context.graph_mode(), dist.scope():
      with ops.name_scope("main"):
        a = variable_scope.variable(1.0, name="a")
        result = dist.call_for_each_replica(model_fn)
      result_b = result[0]
      result_c = result[1]
      self.assertIsInstance(result_b, values.DistributedValues)
      self.assertIsInstance(result_c, values.DistributedValues)
      a0, a1 = dist.unwrap(a)
      b0, b1 = dist.unwrap(result_b)
      c0, c1 = dist.unwrap(result_c)
      self.assertEquals("main/a:0", a0.name)
      self.assertEquals("main/a/replica_1:0", a1.name)
      self.assertEquals("main/b:0", b0.name)
      self.assertEquals("main/b/replica_1:0", b1.name)
      self.assertEquals("main/foo/c:0", c0.name)
      self.assertEquals("main/foo/c/replica_1:0", c1.name)

  def testNameScopeWithGetVariable(self):
    def in_cross_replica(_):
      c = variable_scope.get_variable("c", [1])
      return c

    def model_fn():
      b = variable_scope.get_variable("b", [1])
      with ops.name_scope("foo"):
        c = ds_context.get_replica_context().merge_call(in_cross_replica)
      return b, c

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with context.graph_mode(), dist.scope():
      with ops.name_scope("main"):
        a = variable_scope.get_variable("a", [1])
        result = dist.call_for_each_replica(model_fn)
      result_b = result[0]
      result_c = result[1]
      self.assertIsInstance(result_b, values.DistributedValues)
      self.assertIsInstance(result_c, values.DistributedValues)
      a0, a1 = dist.unwrap(a)
      b0, b1 = dist.unwrap(result_b)
      c0, c1 = dist.unwrap(result_c)
      self.assertEquals("a:0", a0.name)
      self.assertEquals("a/replica_1:0", a1.name)
      self.assertEquals("b:0", b0.name)
      self.assertEquals("b/replica_1:0", b1.name)
      self.assertEquals("c:0", c0.name)
      self.assertEquals("c/replica_1:0", c1.name)

  def testDynamicRnnVariables(self):
    def model_fn():
      inputs = constant_op.constant(2 * [2 * [[0.0, 1.0, 2.0, 3.0, 4.0]]])
      cell_fw = rnn_cell_impl.LSTMCell(300)
      cell_bw = rnn_cell_impl.LSTMCell(300)
      (outputs, _) = rnn.bidirectional_dynamic_rnn(
          cell_fw,
          cell_bw,
          inputs,
          dtype=dtypes.float32)
      return outputs

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with context.graph_mode(), dist.scope():
      result = dist.call_for_each_replica(model_fn)
      # Two variables are created by the RNN layer.
      self.assertEquals(2, len(result))
      for v in result:
        self.assertIsInstance(v, values.DistributedValues)
        _, v1 = dist.unwrap(v)
        self.assertStartsWith(v1.name, "replica_1/")

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testReplicaLocalVariableUpdate(self):
    with context.graph_mode():

      def model_fn():
        v_sum = variable_scope.variable(
            1.0,
            synchronization=variable_scope.VariableSynchronization.ON_READ,
            aggregation=variable_scope.VariableAggregation.SUM)
        self.assertTrue(isinstance(v_sum, values.ReplicaLocalVariable))
        return v_sum

      dist = mirrored_strategy.MirroredStrategy(
          ["/device:GPU:0", "/device:GPU:1"])

      def update(var, value):
        return var.assign(value)

      with dist.scope():
        ret_v_sum = dist.call_for_each_replica(model_fn)
        update_ops = dist.update(ret_v_sum, update, 5.0, grouped=False)

        # Initialize variables.
        self.evaluate(variables.global_variables_initializer())
        # Assert that the aggregated value of the replica local vars is the sum
        # of the individual values before running the update ops.
        self.assertEquals(1.0, self.evaluate(
            ret_v_sum.get(dist.extended.worker_devices[0]).read_value()))
        self.assertEquals(2.0, self.evaluate(ret_v_sum))

        # Apply updates.
        self.evaluate(update_ops)
        # Assert that the aggregated value of the replica local vars is the sum
        # of the individual values after running the update ops.
        self.assertEquals(5.0, self.evaluate(
            ret_v_sum.get(dist.extended.worker_devices[0]).read_value()))
        self.assertEquals(10.0, self.evaluate(ret_v_sum))


class MirroredVariableUpdateTest(test.TestCase):
  # The following tests check assign, assign_add and assign_sub on Mirrored
  # variables in replica and cross replica context.
  config = config_pb2.ConfigProto()
  config.allow_soft_placement = True

  def _skip_eager_if_gpus_less_than(self, num_gpus):
    if context.num_gpus() < num_gpus and context.executing_eagerly():
      self.skipTest("Enough GPUs not available for this test in eager mode.")

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testAssignMirroredVarReplicaContextWithoutAggregationType(self):
    # Test that we always have an aggregation type set on the mirrored variable
    # if we assign to it in replica mode.
    self._skip_eager_if_gpus_less_than(1)
    def var_fn():
      v = variable_scope.variable(1.0, name="foo")
      return v

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with dist.scope():
      mirrored_var = dist.call_for_each_replica(var_fn)
      self.assertIsInstance(mirrored_var, values.MirroredVariable)
      self.evaluate(variables.global_variables_initializer())

      def model_fn():
        return mirrored_var.assign(5.0)

      with self.assertRaisesRegexp(
          ValueError, "You must specify an aggregation method to update a "
                      "MirroredVariable in Replica Context."):
        self.evaluate(dist.unwrap(dist.call_for_each_replica(model_fn)))

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testAssignMirroredVarReplicaContextWithSum(self):
    # Test that we don't reduce a non-per-replica value with the "sum"
    # aggregation type.
    self._skip_eager_if_gpus_less_than(1)
    def var_fn():
      v = variable_scope.variable(
          1.0, name="foo", aggregation=variable_scope.VariableAggregation.SUM)
      return v

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with dist.scope():
      mirrored_var = dist.call_for_each_replica(var_fn)
      self.assertIsInstance(mirrored_var, values.MirroredVariable)
      self.evaluate(variables.global_variables_initializer())

      def model_fn():
        return mirrored_var.assign(5.0)

      with self.assertRaisesRegexp(
          ValueError, "A non-DistributedValues value 5.0 cannot be reduced "
          "with the given reduce op ReduceOp.SUM."):
        self.evaluate(dist.unwrap(dist.call_for_each_replica(model_fn)))

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testAssignMirroredVarCrossDeviceContext(self):
    self._skip_eager_if_gpus_less_than(1)
    def var_fn():
      return variable_scope.variable(1.0, name="foo")

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with dist.scope():
      mirrored_var = dist.call_for_each_replica(var_fn)
      self.assertIsInstance(mirrored_var, values.MirroredVariable)
      self.evaluate(variables.global_variables_initializer())
      self.assertEquals(1.0, self.evaluate(mirrored_var))
      mirrored_var_result = self.evaluate(mirrored_var.assign(6.0))
      self.assertEquals(6.0, mirrored_var_result)

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testAssignMirroredVarReplicaContext(self):
    self._skip_eager_if_gpus_less_than(1)
    def var_fn():
      return variable_scope.variable(
          1.0, name="foo", aggregation=variable_scope.VariableAggregation.MEAN)

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with dist.scope():
      mirrored_var = dist.call_for_each_replica(var_fn)
      self.assertIsInstance(mirrored_var, values.MirroredVariable)
      self.evaluate(variables.global_variables_initializer())
      self.assertEquals(1.0, self.evaluate(mirrored_var))

      def model_fn():
        value = math_ops.cast(
            ds_context.get_replica_context().replica_id_in_sync_group,
            mirrored_var.dtype)
        return mirrored_var.assign(value)

      self.evaluate(dist.unwrap(dist.call_for_each_replica(model_fn)))
      self.assertEquals(0.5, self.evaluate(mirrored_var))

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testAssignMirroredVarReplicaContextWithSingleValue(self):
    self._skip_eager_if_gpus_less_than(1)
    def var_fn():
      return variable_scope.variable(
          1.0, name="foo", aggregation=variable_scope.VariableAggregation.MEAN)

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with dist.scope():
      mirrored_var = dist.call_for_each_replica(var_fn)
      self.assertIsInstance(mirrored_var, values.MirroredVariable)
      self.evaluate(variables.global_variables_initializer())
      self.assertEquals(1.0, self.evaluate(mirrored_var))

      def model_fn():
        return mirrored_var.assign(5.0)

      self.evaluate(dist.unwrap(dist.call_for_each_replica(model_fn)))
      self.assertEquals(5.0, self.evaluate(mirrored_var))

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testAssignAddMirroredVarCrossDeviceContext(self):
    self._skip_eager_if_gpus_less_than(1)
    def var_fn():
      return variable_scope.variable(1.0, name="foo")

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with dist.scope():
      mirrored_var = dist.call_for_each_replica(var_fn)
      self.assertIsInstance(mirrored_var, values.MirroredVariable)
      self.evaluate(variables.global_variables_initializer())
      self.assertEquals(1.0, self.evaluate(mirrored_var))

      # read_value == True
      mirrored_var_result = self.evaluate(
          mirrored_var.assign_add(6.0, read_value=True))
      self.assertEquals(7.0, mirrored_var_result)
      self.assertEquals(7.0, self.evaluate(mirrored_var.get("/device:CPU:0")))
      self.assertEquals(7.0, self.evaluate(mirrored_var.get("/device:GPU:0")))

      # read_value == False
      self.evaluate(mirrored_var.assign_add(2.0, read_value=False))
      self.assertEquals(9.0, self.evaluate(mirrored_var.get("/device:CPU:0")))
      self.assertEquals(9.0, self.evaluate(mirrored_var.get("/device:GPU:0")))

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testAssignAddMirroredVarReplicaContext(self):
    self._skip_eager_if_gpus_less_than(1)
    def var_fn():
      return variable_scope.variable(
          1.0, name="foo", aggregation=variable_scope.VariableAggregation.MEAN)

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with dist.scope():
      mirrored_var = dist.call_for_each_replica(var_fn)
      self.assertIsInstance(mirrored_var, values.MirroredVariable)
      self.evaluate(variables.global_variables_initializer())
      self.assertEquals(1.0, self.evaluate(mirrored_var))

      def model_fn():
        value = math_ops.cast(
            ds_context.get_replica_context().replica_id_in_sync_group,
            mirrored_var.dtype)
        return mirrored_var.assign_add(value)

      self.evaluate(dist.unwrap(dist.call_for_each_replica(model_fn)))
      self.assertEquals(1.5, self.evaluate(mirrored_var))

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testAssignAddMirroredVarReplicaContextWithSingleValue(self):
    self._skip_eager_if_gpus_less_than(1)
    def var_fn():
      return variable_scope.variable(
          1.0, name="foo", aggregation=variable_scope.VariableAggregation.MEAN)

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with dist.scope():
      mirrored_var = dist.call_for_each_replica(var_fn)
      self.assertIsInstance(mirrored_var, values.MirroredVariable)
      self.evaluate(variables.global_variables_initializer())
      self.assertEquals(1.0, self.evaluate(mirrored_var))

      def model_fn():
        return mirrored_var.assign_add(5.0)

      self.evaluate(dist.unwrap(dist.call_for_each_replica(model_fn)))
      self.assertEquals(6.0, self.evaluate(mirrored_var))

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testAssignSubMirroredVarCrossDeviceContext(self):
    self._skip_eager_if_gpus_less_than(1)
    def var_fn():
      return variable_scope.variable(5.0, name="foo")

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with dist.scope():
      mirrored_var = dist.call_for_each_replica(var_fn)
      self.assertIsInstance(mirrored_var, values.MirroredVariable)
      self.evaluate(variables.global_variables_initializer())
      self.assertEquals(5.0, self.evaluate(mirrored_var))
      mirrored_var_result = self.evaluate(mirrored_var.assign_sub(2.0))
      self.assertEquals(3.0, mirrored_var_result)
      self.assertEquals(3.0, self.evaluate(mirrored_var.get("/device:GPU:0")))
      self.assertEquals(3.0, self.evaluate(mirrored_var.get("/device:CPU:0")))

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testAssignSubMirroredVarReplicaContext(self):
    self._skip_eager_if_gpus_less_than(1)
    def var_fn():
      return variable_scope.variable(
          5.0, name="foo", aggregation=variable_scope.VariableAggregation.MEAN)

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with dist.scope():
      mirrored_var = dist.call_for_each_replica(var_fn)
      self.assertIsInstance(mirrored_var, values.MirroredVariable)
      self.evaluate(variables.global_variables_initializer())
      self.assertEquals(5.0, self.evaluate(mirrored_var))

      def model_fn():
        value = math_ops.cast(
            ds_context.get_replica_context().replica_id_in_sync_group,
            mirrored_var.dtype)
        return mirrored_var.assign_sub(value)

      self.evaluate(dist.unwrap(dist.call_for_each_replica(model_fn)))
      self.assertEquals(4.5, self.evaluate(mirrored_var))

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testAssignSubMirroredVarReplicaContextWithSingleValue(self):
    self._skip_eager_if_gpus_less_than(1)
    def var_fn():
      return variable_scope.variable(
          5.0, name="foo", aggregation=variable_scope.VariableAggregation.MEAN)

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with dist.scope():
      mirrored_var = dist.call_for_each_replica(var_fn)
      self.assertIsInstance(mirrored_var, values.MirroredVariable)
      self.evaluate(variables.global_variables_initializer())
      self.assertEquals(5.0, self.evaluate(mirrored_var))

      def model_fn():
        return mirrored_var.assign_sub(1.0)

      self.evaluate(dist.unwrap(dist.call_for_each_replica(model_fn)))
      self.assertEquals(4.0, self.evaluate(mirrored_var))


class MirroredAndReplicaLocalVariableInitializerTest(test.TestCase):
  config = config_pb2.ConfigProto()
  config.allow_soft_placement = True

  def testAssignMirroredVarInitializer(self):
    # This test is not eager compatible since in eager variables are initialized
    # upon construction instead of once the initialization op is run.
    with context.graph_mode():
      def var_fn():
        v = variable_scope.variable(1.0, name="foo")
        return v

      dist = mirrored_strategy.MirroredStrategy(
          ["/device:GPU:0", "/device:CPU:0"])

      with dist.scope():
        mirrored_var = dist.call_for_each_replica(var_fn)
        self.assertIsInstance(mirrored_var, values.MirroredVariable)
        self.assertFalse(self.evaluate(mirrored_var.is_initialized()))
        self.evaluate(mirrored_var.initializer)
        self.assertTrue(self.evaluate(mirrored_var.is_initialized()))

  def testAssignReplicaLocalVarInitializer(self):
    # This test is not eager compatible since in eager variables are initialized
    # upon construction instead of once the initialization op is run.
    with context.graph_mode():
      def model_fn():
        v_sum = variable_scope.variable(
            1.0,
            synchronization=variable_scope.VariableSynchronization.ON_READ,
            aggregation=variable_scope.VariableAggregation.SUM)
        self.assertTrue(isinstance(v_sum, values.ReplicaLocalVariable))
        return v_sum

      dist = mirrored_strategy.MirroredStrategy(
          ["/device:GPU:0", "/device:CPU:0"])

      with dist.scope():
        replica_local_var = dist.call_for_each_replica(model_fn)
        self.assertTrue(isinstance(replica_local_var,
                                   values.ReplicaLocalVariable))
        self.assertFalse(self.evaluate(replica_local_var.is_initialized()))
        self.evaluate(replica_local_var.initializer)
        self.assertTrue(self.evaluate(replica_local_var.is_initialized()))


class ReplicaLocalVariableAssignTest(test.TestCase):
  config = config_pb2.ConfigProto()
  config.allow_soft_placement = True

  def _skip_eager_if_gpus_less_than(self, num_gpus):
    if context.num_gpus() < num_gpus and context.executing_eagerly():
      self.skipTest("Not enough GPUs available for this test in eager mode.")

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testAssignReplicaLocalVarSumAggregation(self):
    self._skip_eager_if_gpus_less_than(1)
    def model_fn():
      v_sum = variable_scope.variable(
          1.0,
          synchronization=variable_scope.VariableSynchronization.ON_READ,
          aggregation=variable_scope.VariableAggregation.SUM)
      return v_sum

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with dist.scope():
      replica_local_var = dist.call_for_each_replica(model_fn)
      self.assertTrue(isinstance(replica_local_var,
                                 values.ReplicaLocalVariable))
      self.evaluate(variables.global_variables_initializer())
      # Each replica has a value of 1.0 assigned to it in replica context.
      # When we read the value using `read_var` we should see the SUM of each of
      # values on each of the replicas.
      self.assertEqual(2.0, self.evaluate(dist.read_var(replica_local_var)))
      # Assigning 6.0 in cross replica context will assign a value of
      # 6.0/num_replicas to each replica.
      tlv_ops = replica_local_var.assign(6.0)
      self.evaluate(tlv_ops)
      # On reading the replica local var we should get the assigned value back.
      # The value on all the replicas are added before being returned by
      # `read_var`.
      self.assertEqual(6.0, self.evaluate(dist.read_var(replica_local_var)))

  @test_util.run_in_graph_and_eager_modes(config=config)
  def testAssignReplicaLocalVarMeanAggregation(self):
    self._skip_eager_if_gpus_less_than(1)
    def model_fn():
      v_sum = variable_scope.variable(
          1.0,
          synchronization=variable_scope.VariableSynchronization.ON_READ,
          aggregation=variable_scope.VariableAggregation.MEAN)
      return v_sum

    dist = mirrored_strategy.MirroredStrategy(
        ["/device:GPU:0", "/device:CPU:0"])

    with dist.scope():
      replica_local_var = dist.call_for_each_replica(model_fn)
      self.assertTrue(isinstance(replica_local_var,
                                 values.ReplicaLocalVariable))
      self.evaluate(variables.global_variables_initializer())
      # Each replica has a value of 1.0 assigned to it in replica context.
      # When we read the value using `read_var` we should see the MEAN of values
      # on all replicas which is the value assigned in replica context.
      self.assertEqual(1.0, self.evaluate(dist.read_var(replica_local_var)))
      tlv_ops = replica_local_var.assign(6.0)
      self.evaluate(tlv_ops)
      # On reading the replica local var we should get the MEAN of all values
      # which is equal to the value assigned.
      self.assertEqual(6.0, self.evaluate(dist.read_var(replica_local_var)))


class MockModel(object):

  def __init__(self, two_variables=False):
    self.variables = []
    self.variables.append(variable_scope.variable(1.25, name="dummy_var1"))
    if two_variables:
      self.variables.append(variable_scope.variable(2.0, name="dummy_var2"))

  def __call__(self, factor=2):
    x = factor * self.variables[0]
    if len(self.variables) > 1:
      x += self.variables[1]
    return x


class MiniModel(keras_training.Model):
  """Minimal model for mnist.

  Useful for testing and debugging on slow TPU simulators.
  """

  def __init__(self):
    super(MiniModel, self).__init__(name="")
    self.fc = keras_core.Dense(1, name="fc", kernel_initializer="ones",
                               bias_initializer="ones")

  def call(self, inputs, training=True):
    inputs = array_ops.ones([1, 10])
    return self.fc(inputs)


class MirroredStrategyDefunTest(test.TestCase):

  def _skip_eager_if_gpus_less_than(self, num_gpus):
    if context.num_gpus() < num_gpus and context.executing_eagerly():
      self.skipTest("Not enough GPUs available for this test in eager mode.")

  def _call_and_check(self, model_fn, inputs, expected_result, defuns,
                      two_variables=False):
    cpu_dev = device_util.canonicalize("CPU:0")
    gpu_dev = device_util.canonicalize("GPU:0")
    devices = [cpu_dev, gpu_dev]
    dist = mirrored_strategy.MirroredStrategy(devices)

    with dist.scope():
      mock_model = MockModel(two_variables)
      self.evaluate(variables.global_variables_initializer())

      result = dist.call_for_each_replica(model_fn, args=[mock_model] + inputs)
      for device in devices:
        device_result = values.select_device(device, result)
        device_expected_result = values.select_device(device, expected_result)
        self.assertAllClose(device_expected_result,
                            self.evaluate(device_result))

      for defun in defuns:
        # PolymorphicFunctions are specialized to the current device stack, so
        # call_for_each has one trace per device. To check that the expected set
        # of variables was accessed on each trace, we first retrieve each
        # device-specific graph function.
        per_replica_graph_functions = dist.call_for_each_replica(
            defun.get_concrete_function, args=[mock_model] + inputs)
        for device in devices:
          graph_function = per_replica_graph_functions.get(device=device)
          self.assertEqual(set(mock_model.variables),
                           set(graph_function.graph.variables))

  @test_util.run_in_graph_and_eager_modes()
  def testVariableInDefun(self):
    self._skip_eager_if_gpus_less_than(1)

    @function.defun
    def times_two(mock_model):
      return mock_model()

    def model_fn(mock_model):
      return times_two(mock_model)

    self._call_and_check(model_fn, [], 2.5, [times_two])

  @test_util.run_in_graph_and_eager_modes()
  def testVariableInNestedDefun(self):
    self._skip_eager_if_gpus_less_than(1)

    @function.defun
    def times_two(mock_model):
      return mock_model()

    @function.defun
    def two_x_plus_one(mock_model):
      return times_two(mock_model) + 1

    def model_fn(mock_model):
      return two_x_plus_one(mock_model)

    self._call_and_check(model_fn, [], 3.5, [times_two, two_x_plus_one])

  @test_util.run_in_graph_and_eager_modes()
  def testTwoVariablesInNestedDefun(self):
    self._skip_eager_if_gpus_less_than(1)

    @function.defun
    def fn1(mock_model):
      return mock_model()

    @function.defun
    def fn2(mock_model):
      return fn1(mock_model) + 1

    def model_fn(mock_model):
      return fn2(mock_model)

    self._call_and_check(model_fn, [], 5.5, [fn1, fn2], two_variables=True)

  @test_util.run_in_graph_and_eager_modes()
  def testGradientTapeOverNestedDefuns(self):
    self._skip_eager_if_gpus_less_than(1)

    @function.defun
    def fn1(mock_model):
      return mock_model()

    @function.defun
    def fn2(mock_model):
      return fn1(mock_model) + 1

    def model_fn(mock_model):
      with backprop.GradientTape(persistent=True) as gtape:
        result = fn2(mock_model)
      grads = gtape.gradient(result,
                             [v.get() for v in mock_model.variables])
      return grads

    self._call_and_check(model_fn, [], [2.0, 1.0], [fn1, fn2],
                         two_variables=True)

  @test_util.run_in_graph_and_eager_modes()
  def testPassPerReplica(self):
    self._skip_eager_if_gpus_less_than(1)

    @function.defun
    def fn1(mock_model, factor):
      return mock_model(factor)

    factors = values.PerReplica({"CPU:0": 5.0, "GPU:0": 3.0})
    expected_result = values.PerReplica({"CPU:0": 5.0 * 1.25,
                                         "GPU:0": 3.0 * 1.25})
    self._call_and_check(fn1, [factors], expected_result, [fn1])

  @test_util.run_in_graph_and_eager_modes()
  def testTrain(self):
    self._skip_eager_if_gpus_less_than(1)

    cpu_dev = device_util.canonicalize("CPU:0")
    gpu_dev = device_util.canonicalize("GPU:0")
    devices = [cpu_dev, gpu_dev]
    dist = mirrored_strategy.MirroredStrategy(devices)

    with dist.scope():
      mock_model = MiniModel()
      mock_model.call = function.defun(mock_model.call)

      def loss_fn(ctx):
        del ctx
        return mock_model(array_ops.ones([1, 10]))

      gradients_fn = backprop.implicit_grad(loss_fn)
      gradients_fn = optimizer_lib.get_filtered_grad_fn(gradients_fn)
      grads_and_vars = dist.call_for_each_replica(gradients_fn, args=(None,))

      optimizer = gradient_descent.GradientDescentOptimizer(0.25)
      update_ops = optimizer._distributed_apply(dist, grads_and_vars)  # pylint: disable=protected-access

      if not context.executing_eagerly():
        self.evaluate(variables.global_variables_initializer())
        self.evaluate(update_ops)

      updated_var_values = self.evaluate(mock_model.variables)
      # All variables start at 1.0 and get two updates of 0.25.
      self.assertAllEqual(0.5 * np.ones([10, 1]), updated_var_values[0])
      self.assertAllEqual([0.5], updated_var_values[1])


class MultiWorkerMirroredStrategyTest(
    multi_worker_test_base.MultiWorkerTestBase,
    strategy_test_lib.DistributionTestBase):

  def _get_distribution_strategy(self):
    cluster_spec = server_lib.ClusterSpec({
        "worker": ["/job:worker/task:0", "/job:worker/task:1"]
    })
    strategy = mirrored_strategy.MirroredStrategy(num_gpus=context.num_gpus())
    strategy.configure(cluster_spec=cluster_spec)
    return strategy

  def test_num_replicas_in_sync(self):
    if not GPU_TEST:
      self.skipTest("Not GPU test")

    strategy = self._get_distribution_strategy()
    # We calculate the total number of gpus across the workers(2) specified in
    # the cluster spec.
    self.assertEqual(context.num_gpus() * 2, strategy.num_replicas_in_sync)

  def testMinimizeLossGraph(self):
    self._test_minimize_loss_graph(self._get_distribution_strategy(),
                                   learning_rate=0.05)


class MultiWorkerMirroredStrategyTestWithChief(
    multi_worker_test_base.MultiWorkerTestBase,
    strategy_test_lib.DistributionTestBase):

  @classmethod
  def setUpClass(cls):
    """Create a local cluster with 2 workers and 1 chief."""
    cls._cluster_spec = multi_worker_test_base.create_in_process_cluster(
        num_workers=2, num_ps=0, has_chief=True)
    cls._default_target = "grpc://" + cls._cluster_spec["chief"][0]

  def testMinimizeLossGraph(self):
    strategy = mirrored_strategy.MirroredStrategy(
        num_gpus_per_worker=context.num_gpus())
    strategy.configure(cluster_spec=self._cluster_spec)
    self._test_minimize_loss_graph(strategy, learning_rate=0.05)


def _replica_id():
  replica_id = ds_context.get_replica_context().replica_id_in_sync_group
  if not isinstance(replica_id, ops.Tensor):
    replica_id = constant_op.constant(replica_id)
  return replica_id


if __name__ == "__main__":
  test.main()
