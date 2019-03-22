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
"""Test DistributionStrategy, ReplicaContext, and supporting APIs."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.python.distribute import distribute_lib
from tensorflow.python.distribute import distribution_strategy_context as ds_context
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.ops import variable_scope
from tensorflow.python.platform import test


class _TestReplicaContext(distribute_lib.ReplicaContext):

  def merge_call(self, fn, *args, **kwargs):
    return kwargs["test_arg"]


def _get_test_variable(name, synchronization, aggregation):
  return {
      "name": name,
      "synchronization": synchronization,
      "aggregation": aggregation
  }


class _TestStrategy(distribute_lib.DistributionStrategy):

  def __init__(self):
    super(_TestStrategy, self).__init__(_TestExtended(self))


class _TestExtended(distribute_lib.DistributionStrategyExtended):

  def _call_for_each_replica(self, fn, args, kwargs):
    with _TestReplicaContext(
        self._container_strategy(),
        replica_id_in_sync_group=constant_op.constant(0, dtypes.int32)):
      return fn(*args, **kwargs)

  def _create_variable(self, next_creator, *args, **kwargs):
    return _get_test_variable(kwargs["name"], kwargs["synchronization"],
                              kwargs["aggregation"])


def _assert_in_default_state(t):
  t.assertIs(ds_context._get_default_replica_context(),
             ds_context.get_replica_context())
  t.assertIs(None, ds_context.get_cross_replica_context())
  t.assertFalse(ds_context.in_cross_replica_context())
  t.assertIs(ds_context._get_default_strategy(), ds_context.get_strategy())
  t.assertFalse(ds_context.has_strategy())


class TestStrategyTest(test.TestCase):

  def testCallForEachReplica(self):
    _assert_in_default_state(self)
    dist = _TestStrategy()

    def run_fn():
      replica_context = ds_context.get_replica_context()
      self.assertTrue(replica_context is not None)
      self.assertIs(None, ds_context.get_cross_replica_context())
      self.assertFalse(ds_context.in_cross_replica_context())
      self.assertTrue(ds_context.has_strategy())
      self.assertIs(dist, ds_context.get_strategy())
      self.assertEqual("foo", replica_context.merge_call(None, test_arg="foo"))
      expected_value = _get_test_variable(
          "bar", variable_scope.VariableSynchronization.AUTO,
          variable_scope.VariableAggregation.NONE)
      self.assertDictEqual(expected_value,
                           variable_scope.variable(1.0, name="bar"))

    dist.extended.call_for_each_replica(run_fn)
    with dist.scope():
      dist.extended.call_for_each_replica(run_fn)
    _assert_in_default_state(self)

  def testScope(self):
    _assert_in_default_state(self)
    dist = _TestStrategy()
    with dist.scope():
      self.assertIs(None, ds_context.get_replica_context())
      self.assertIs(dist, ds_context.get_cross_replica_context())
      self.assertTrue(ds_context.in_cross_replica_context())
      self.assertTrue(ds_context.has_strategy())
      self.assertIs(dist, ds_context.get_strategy())
      expected_value = _get_test_variable(
          "baz", variable_scope.VariableSynchronization.AUTO,
          variable_scope.VariableAggregation.NONE)
      self.assertDictEqual(expected_value,
                           variable_scope.variable(1.0, name="baz"))
    _assert_in_default_state(self)

  def testScopeDeviceNestingError(self):
    _assert_in_default_state(self)
    dist = _TestStrategy()
    # Open a device scope with dist.scope().
    dist.extended._default_device = "/device:GPU:0"
    scope = dist.scope()
    scope.__enter__()
    self.assertIs(dist, ds_context.get_strategy())
    with ops.device("/device:CPU:0"):
      with self.assertRaisesRegexp(RuntimeError, "Device scope nesting error"):
        scope.__exit__(None, None, None)
    scope.__exit__(None, None, None)
    _assert_in_default_state(self)

  def testScopeVarCreatorNestingError(self):

    def creator(next_creator, **kwargs):
      return next_creator(**kwargs)

    _assert_in_default_state(self)
    dist = _TestStrategy()
    scope = dist.scope()
    scope.__enter__()
    self.assertIs(dist, ds_context.get_strategy())
    with variable_scope.variable_creator_scope(creator):
      with self.assertRaisesRegexp(RuntimeError,
                                   "Variable creator scope nesting error"):
        scope.__exit__(None, None, None)
    scope.__exit__(None, None, None)
    _assert_in_default_state(self)

  def testScopeVarScopeNestingError(self):
    # We create a new graph here to simplify clean-up, since the error
    # we are triggering happens in the middle of scope.__exit__() and
    # leaves us in a weird state.
    with ops.Graph().as_default():
      _assert_in_default_state(self)
      dist = _TestStrategy()
      scope = dist.scope()
      scope.__enter__()
      self.assertIs(dist, ds_context.get_strategy())
      with variable_scope.variable_scope("AA"):
        with self.assertRaisesRegexp(RuntimeError,
                                     "Variable scope nesting error"):
          scope.__exit__(None, None, None)
    _assert_in_default_state(self)

  def testSettingSynchronizationAndAggregation(self):
    _assert_in_default_state(self)
    dist = _TestStrategy()
    with dist.scope():
      expected_value = _get_test_variable(
          "baz", variable_scope.VariableSynchronization.ON_WRITE,
          variable_scope.VariableAggregation.MEAN)
      self.assertDictEqual(
          expected_value,
          variable_scope.variable(
              1.0,
              name="baz",
              synchronization=variable_scope.VariableSynchronization.ON_WRITE,
              aggregation=variable_scope.VariableAggregation.MEAN))
    _assert_in_default_state(self)

  def testSetStrategy(self):
    _assert_in_default_state(self)
    dist = _TestStrategy()
    dist2 = _TestStrategy()
    ds_context.experimental_set_strategy(dist)
    self.assertIs(None, ds_context.get_replica_context())
    self.assertIs(dist, ds_context.get_cross_replica_context())
    self.assertTrue(ds_context.in_cross_replica_context())
    self.assertTrue(ds_context.has_strategy())
    self.assertIs(dist, ds_context.get_strategy())
    expected_value = _get_test_variable(
        "baz", variable_scope.VariableSynchronization.AUTO,
        variable_scope.VariableAggregation.NONE)
    self.assertDictEqual(expected_value,
                         variable_scope.variable(1.0, name="baz"))
    ds_context.experimental_set_strategy(dist2)
    self.assertIs(dist2, ds_context.get_strategy())
    ds_context.experimental_set_strategy(None)
    _assert_in_default_state(self)

  def testSetStrategyInScope(self):
    _assert_in_default_state(self)
    dist = _TestStrategy()
    with dist.scope():
      with self.assertRaisesRegexp(
          RuntimeError,
          "Must not be called inside a `tf.distribute.Strategy` scope"):
        ds_context.experimental_set_strategy(_TestStrategy())
      with self.assertRaisesRegexp(
          RuntimeError,
          "Must not be called inside a `tf.distribute.Strategy` scope"):
        ds_context.experimental_set_strategy(dist)
      with self.assertRaisesRegexp(
          RuntimeError,
          "Must not be called inside a `tf.distribute.Strategy` scope"):
        ds_context.experimental_set_strategy(None)
    _assert_in_default_state(self)

  def testSameScopeNesting(self):
    _assert_in_default_state(self)
    dist = _TestStrategy()
    scope_a = dist.scope()
    with scope_a:
      self.assertIs(dist, ds_context.get_strategy())
      scope_b = dist.scope()
      with scope_b:
        self.assertIs(dist, ds_context.get_strategy())
        with scope_a:
          self.assertIs(dist, ds_context.get_strategy())
        self.assertIs(dist, ds_context.get_strategy())
      self.assertIs(dist, ds_context.get_strategy())
      dist2 = _TestStrategy()
      scope2 = dist2.scope()
      with self.assertRaisesRegexp(
          RuntimeError,
          "Mixing different tf.distribute.Strategy objects"):
        with scope2:
          pass
    _assert_in_default_state(self)
    with scope_b:
      self.assertIs(dist, ds_context.get_strategy())
    _assert_in_default_state(self)


class DefaultDistributionStrategyTest(test.TestCase):

  def testMergeCall(self):
    _assert_in_default_state(self)

    def merge_fn(dist, s):
      self.assertIs(ds_context._get_default_strategy(), dist)
      self.assertIs(None, ds_context.get_replica_context())
      self.assertIs(dist, ds_context.get_cross_replica_context())
      self.assertTrue(ds_context.in_cross_replica_context())
      self.assertIs(dist, ds_context.get_strategy())
      self.assertFalse(ds_context.has_strategy())
      return "foo_" + s

    replica_ctx = ds_context.get_replica_context()
    self.assertIs(ds_context._get_default_replica_context(), replica_ctx)
    self.assertEqual("foo_bar", replica_ctx.merge_call(merge_fn, args=("bar",)))
    _assert_in_default_state(self)


class InputContextTest(test.TestCase):

  def testProperties(self):
    input_context = distribute_lib.InputContext(
        num_input_pipelines=2, input_pipeline_id=1, num_replicas_in_sync=6)
    self.assertEqual(6, input_context.num_replicas_in_sync)
    self.assertEqual(1, input_context.input_pipeline_id)
    self.assertEqual(2, input_context.num_input_pipelines)

  def testPerReplicaBatchSize(self):
    input_context = distribute_lib.InputContext(
        num_input_pipelines=2, input_pipeline_id=1, num_replicas_in_sync=6)
    self.assertEqual(2, input_context.get_per_replica_batch_size(12))
    with self.assertRaises(ValueError):
      input_context.get_per_replica_batch_size(13)


if __name__ == "__main__":
  test.main()
