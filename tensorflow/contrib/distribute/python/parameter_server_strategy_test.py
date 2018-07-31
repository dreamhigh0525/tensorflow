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
"""Tests for ParameterServerStrategy."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import contextlib
import json
import threading
from absl.testing import parameterized

from tensorflow.contrib.distribute.python import combinations
from tensorflow.contrib.distribute.python import multi_worker_test_base
from tensorflow.contrib.distribute.python import parameter_server_strategy
from tensorflow.core.protobuf import config_pb2
from tensorflow.python.client import session
from tensorflow.python.eager import context
from tensorflow.python.estimator import run_config
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import ops
from tensorflow.python.layers import core
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import control_flow_ops
from tensorflow.python.ops import gradients
from tensorflow.python.ops import variable_scope
from tensorflow.python.ops import variables
from tensorflow.python.platform import test
from tensorflow.python.training import device_util
from tensorflow.python.training import distribute as distribute_lib


class ParameterServerStrategyTest(test.TestCase, parameterized.TestCase):

  @classmethod
  def setUpClass(cls):
    cls._workers, cls._ps = multi_worker_test_base.create_in_process_cluster(
        num_workers=3, num_ps=2)

  def setUp(self):
    self._result = 0
    self._lock = threading.Lock()
    self._init_condition = threading.Condition()
    self._init_reached = 0
    self._finish_condition = threading.Condition()
    self._finish_reached = 0

  def _get_ps_distribution_strategy(self, task_type, task_index, num_gpus=0):
    tf_config = {
        'cluster': {
            run_config.TaskType.WORKER: [
                'fake_worker_0', 'fake_worker_1', 'fake_worker_2'
            ],
            run_config.TaskType.PS: ['fake_ps_0', 'fake_ps_1']
        },
        'task': {
            'type': task_type,
            'index': task_index
        }
    }
    distribution = parameter_server_strategy.ParameterServerStrategy(
        num_gpus_per_worker=num_gpus)
    with self._lock:
      # Accessing environment variables should be protected by locks because
      # environment variables are shared by all threads.
      with test.mock.patch.dict('os.environ',
                                {'TF_CONFIG': json.dumps(tf_config)}):
        distribution.configure()
    return distribution

  @contextlib.contextmanager
  def _test_session(self, target):
    config = config_pb2.ConfigProto(allow_soft_placement=True)
    config.graph_options.optimizer_options.opt_level = -1
    with session.Session(graph=None, config=config, target=target) as sess:
      yield sess

  def _test_device_assignment_distributed(self, d, num_gpus=0):
    with ops.Graph().as_default(), \
         self._test_session(target=self._workers[0].target) as sess, \
         d.scope():

      # Define a variable outside the call_for_each_tower scope. This is not
      # recommended.
      n = variable_scope.get_variable('n', initializer=10.0)
      self.assertEqual(n.device, '/job:ps/task:0')

      def model_fn():
        if num_gpus == 0:
          last_part_device = 'device:CPU:0'
        else:
          last_part_device = (
              'device:GPU:%d' % distribute_lib.get_tower_context().tower_id)

        a = constant_op.constant(1.0)
        b = constant_op.constant(2.0)
        c = a + b
        self.assertEqual(a.device,
                         '/job:worker/replica:0/task:1/%s' % last_part_device)
        self.assertEqual(b.device,
                         '/job:worker/replica:0/task:1/%s' % last_part_device)
        self.assertEqual(c.device,
                         '/job:worker/replica:0/task:1/%s' % last_part_device)

        # The device scope is ignored for variables but not for normal ops.
        with ops.device('/job:worker/task:0'):
          x = variable_scope.get_variable('x', initializer=10.0)
          x_add = x.assign_add(c)
          e = a + c
        # The variable x is on the task 1 since the device_function has been
        # called once before the model_fn.
        self.assertEqual(x.device, '/job:ps/task:1')
        self.assertEqual(x_add.device, x.device)
        self.assertEqual(e.device,
                         '/job:worker/replica:0/task:0/%s' % last_part_device)

        # The colocate_vars_with can override the distribution's device.
        with d.colocate_vars_with(x):
          y = variable_scope.get_variable('y', initializer=20.0)
        y_add = y.assign_add(x_add)
        self.assertEqual(y.device, '/job:ps/task:1')
        self.assertEqual(y_add.device, y.device)
        self.assertEqual(y.device, x.device)

        z = variable_scope.get_variable('z', initializer=10.0)
        self.assertEqual(z.device, '/job:ps/task:0')
        self.assertNotEqual(z.device, x.device)

        with ops.control_dependencies([y_add]):
          z_add = z.assign_add(y)
        with ops.control_dependencies([z_add]):
          f = z + c
        self.assertEqual(f.device,
                         '/job:worker/replica:0/task:1/%s' % last_part_device)

        # The device scope would merge with the default worker device.
        with ops.device('/CPU:1'):
          g = e + 1.0
        self.assertEqual(g.device, '/job:worker/replica:0/task:1/device:CPU:1')

        # Ths ops.colocate_with will be ignored when defining a variale but not
        # for a normal tensor.
        with ops.colocate_with(x):
          u = variable_scope.get_variable('u', initializer=30.0)
          v = variable_scope.get_variable('v', initializer=30.0)
          h = f + 1.0
        self.assertIn('/job:ps/', u.device)
        self.assertIn('/job:ps/', v.device)
        # u and v are on different parameter servers.
        self.assertTrue(u.device != x.device or v.device != x.device)
        self.assertTrue(u.device == x.device or v.device == x.device)
        # Here h is not on one worker. Note h.device is canonical while x.device
        # is not but.
        self.assertIn('/job:ps/', h.device)
        return y_add, z_add, f

      y, z, f = d.call_for_each_tower(model_fn)
      self.assertNotEqual(y, None)
      self.assertNotEqual(z, None)
      self.assertNotEqual(f, None)

      if context.num_gpus() >= 1 and num_gpus <= 1:
        variables.global_variables_initializer().run()
        y_val, z_val, f_val = sess.run([y, z, f])
        self.assertEqual(y_val, 33.0)
        self.assertEqual(z_val, 43.0)
        self.assertEqual(f_val, 46.0)

  @combinations.generate(
      combinations.combine(mode=['graph'], num_gpus=[0, 1, 2]))
  def testDeviceAssignmentDistributed(self, num_gpus):
    d = self._get_ps_distribution_strategy('worker', 1, num_gpus=num_gpus)
    self._test_device_assignment_distributed(d, num_gpus=num_gpus)

  def _test_device_assignment_local(self,
                                    d,
                                    compute_device='CPU',
                                    variable_device='CPU',
                                    num_gpus=0):
    with ops.Graph().as_default(), \
         self._test_session(target=self._workers[0].target) as sess, \
         d.scope():

      def model_fn():
        if 'CPU' in compute_device:
          tower_compute_device = '/device:CPU:0'
        else:
          tower_compute_device = (
              '/device:GPU:%d' % distribute_lib.get_tower_context().tower_id)
        tower_compute_device = device_util.canonicalize(tower_compute_device)

        if 'CPU' in variable_device:
          tower_variable_device = '/device:CPU:0'
        else:
          tower_variable_device = (
              '/device:GPU:%d' % distribute_lib.get_tower_context().tower_id)
        tower_variable_device = device_util.canonicalize(tower_variable_device)

        a = constant_op.constant(1.0)
        b = constant_op.constant(2.0)
        c = a + b
        self.assertEqual(a.device, tower_compute_device)
        self.assertEqual(b.device, tower_compute_device)
        self.assertEqual(c.device, tower_compute_device)

        # The device scope is ignored for variables but not for normal ops.
        with ops.device('/device:GPU:2'):
          x = variable_scope.get_variable('x', initializer=10.0)
          x_add = x.assign_add(c)
          e = a + c
        self.assertEqual(
            device_util.canonicalize(x.device), tower_variable_device)
        self.assertEqual(x_add.device, x.device)
        self.assertEqual(e.device, device_util.canonicalize('/device:GPU:2'))

        # The colocate_vars_with can override the distribution's device.
        with d.colocate_vars_with(x):
          y = variable_scope.get_variable('y', initializer=20.0)
        y_add = y.assign_add(x_add)
        self.assertEqual(
            device_util.canonicalize(y.device), tower_variable_device)
        self.assertEqual(y_add.device, y.device)
        self.assertEqual(y.device, x.device)

        z = variable_scope.get_variable('z', initializer=10.0)
        self.assertEqual(
            device_util.canonicalize(z.device), tower_variable_device)

        with ops.control_dependencies([y_add]):
          z_add = z.assign_add(y)
        with ops.control_dependencies([z_add]):
          f = z + c
        self.assertEqual(f.device, tower_compute_device)

        # The device scope would merge with the default worker device.
        with ops.device('/CPU:1'):
          g = e + 1.0
        self.assertEqual(g.device, device_util.canonicalize('/device:CPU:1'))

        # Ths ops.colocate_with will be ignored when defining a variale but not
        # for a normal tensor.
        with ops.colocate_with(x):
          u = variable_scope.get_variable('u', initializer=30.0)
          h = f + 1.0
        self.assertEqual(
            device_util.canonicalize(u.device), tower_variable_device)
        self.assertEqual(device_util.canonicalize(x.device), h.device)
        return y_add, z_add, f

      y, z, f = d.call_for_each_tower(model_fn)
      self.assertNotEqual(y, None)
      self.assertNotEqual(z, None)
      self.assertNotEqual(f, None)

      if context.num_gpus() >= 1 and num_gpus <= 1:
        variables.global_variables_initializer().run()
        y_val, z_val, f_val = sess.run([y, z, f])
        self.assertEqual(y_val, 33.0)
        self.assertEqual(z_val, 43.0)
        self.assertEqual(f_val, 46.0)

  def testDeviceAssignmentLocal(self):
    distribution = parameter_server_strategy.ParameterServerStrategy(
        num_gpus_per_worker=0)
    self._test_device_assignment_local(
        distribution, compute_device='CPU', variable_device='CPU', num_gpus=0)

    distribution = parameter_server_strategy.ParameterServerStrategy(
        num_gpus_per_worker=1)
    self._test_device_assignment_local(
        distribution, compute_device='GPU', variable_device='GPU', num_gpus=1)

    distribution = parameter_server_strategy.ParameterServerStrategy(
        num_gpus_per_worker=2)
    self._test_device_assignment_local(
        distribution, compute_device='GPU', variable_device='CPU', num_gpus=2)

  def _test_simple_increment(self, d, task_type, task_index, master_target):
    if hasattr(d, '_cluster_spec') and d._cluster_spec:
      num_workers = len(d._cluster_spec.as_dict().get('worker',
                                                      ['dummy_worker']))
    else:
      num_workers = 1
    with ops.Graph().as_default(), \
         self._test_session(target=master_target) as sess, \
         d.scope():

      def model_fn():
        x = variable_scope.get_variable('x', initializer=10.0)
        y = variable_scope.get_variable('y', initializer=20.0)

        x_add = x.assign_add(1.0, use_locking=True)
        y_add = y.assign_add(1.0, use_locking=True)

        train_op = control_flow_ops.group([x_add, y_add])
        return x, y, train_op

      x, y, train_op = d.call_for_each_tower(model_fn)
      train_op = d.group(d.unwrap(train_op))

      if context.num_gpus() < d._num_gpus_per_worker:
        return True

      if task_index == 0:
        variables.global_variables_initializer().run()

      # Workers waiting for chief worker's initializing variables.
      self._init_condition.acquire()
      self._init_reached += 1
      while self._init_reached != num_workers:
        self._init_condition.wait()
      self._init_condition.notify_all()
      self._init_condition.release()

      sess.run(train_op)

      # Wait for other workers to finish training.
      self._finish_condition.acquire()
      self._finish_reached += 1
      while self._finish_reached != num_workers:
        self._finish_condition.wait()
      self._finish_condition.notify_all()
      self._finish_condition.release()

      x_val, y_val = sess.run([x, y])
      self.assertEqual(x_val, 10.0 + 1.0 * num_workers * d.num_towers)
      self.assertEqual(y_val, 20.0 + 1.0 * num_workers * d.num_towers)
      return (x_val == 10.0 + 1.0 * num_workers * d.num_towers and
              y_val == 20.0 + 1.0 * num_workers * d.num_towers)

  def _test_minimize_loss_graph(self, d, task_type, task_index, master_target):
    with ops.Graph().as_default(), \
         self._test_session(target=master_target) as sess, \
         d.scope():
      l = core.Dense(1, use_bias=False)

      def loss_fn(x):
        y = array_ops.reshape(l(x), []) - constant_op.constant(1.)
        return y * y

      # TODO(yuefengz, apassos): eager.backprop.implicit_grad is not safe for
      # multiple graphs (b/111216820).
      def grad_fn(x):
        loss = loss_fn(x)
        var_list = (
            variables.trainable_variables() + ops.get_collection(
                ops.GraphKeys.TRAINABLE_RESOURCE_VARIABLES))
        grads = gradients.gradients(loss, var_list)
        ret = list(zip(grads, var_list))
        return ret

      def update(v, g):
        return v.assign_sub(0.05 * g, use_locking=True)

      one = d.broadcast(constant_op.constant([[1.]]))

      def step():
        """Perform one optimization step."""
        # Run forward & backward to get gradients, variables list.
        g_v = d.call_for_each_tower(grad_fn, one)
        # Update the variables using the gradients and the update() function.
        before_list = []
        after_list = []
        for g, v in g_v:
          fetched = d.read_var(v)
          before_list.append(fetched)
          with ops.control_dependencies([fetched]):
            # TODO(yuefengz): support non-Mirrored variable as destinations.
            g = d.reduce(
                variable_scope.VariableAggregation.SUM, g, destinations=v)
            with ops.control_dependencies(d.unwrap(d.update(v, update, g))):
              after_list.append(d.read_var(v))
        return before_list, after_list

      before_out, after_out = step()

      if context.num_gpus() < d._num_gpus_per_worker:
        return True

      if task_index == 0:
        variables.global_variables_initializer().run()

      # Workers waiting for chief worker's initializing variables.
      self._init_condition.acquire()
      self._init_reached += 1
      while self._init_reached != 3:
        self._init_condition.wait()
      self._init_condition.notify_all()
      self._init_condition.release()

      for i in range(10):
        b, a = sess.run((before_out, after_out))
        if i == 0:
          before, = b
        after, = a

      error_before = abs(before - 1)
      error_after = abs(after - 1)
      # Error should go down
      self.assertLess(error_after, error_before)
      return error_after < error_before

  def _run_client(self, index, model_fn, num_gpus):
    task_type = run_config.TaskType.WORKER
    result = model_fn(
        self._get_ps_distribution_strategy(task_type, index, num_gpus=num_gpus),
        task_type, index, self._workers[index].target)
    if result:
      with self._lock:
        self._result += 1

  def _run_multiple_clients(self, num_clients, model_fn, num_gpus=0):
    threads = []
    for i in range(num_clients):
      t = threading.Thread(
          target=self._run_client, args=(i, model_fn, num_gpus))
      t.start()
      threads.append(t)
    for t in threads:
      t.join()

  def testSimpleBetweenGraph(self):
    self._run_multiple_clients(3, self._test_simple_increment)
    self.assertEqual(self._result, 3)

  @combinations.generate(
      combinations.combine(mode=['graph'], num_gpus=[0, 1, 2]))
  def testLocalSimpleIncrement(self, num_gpus):
    d = parameter_server_strategy.ParameterServerStrategy(
        num_gpus_per_worker=num_gpus)
    self._test_simple_increment(d, 'dummy_worker', 0, '')

  @combinations.generate(
      combinations.combine(mode=['graph'], num_gpus=[0, 1, 2]))
  def testMinimizeLossGraph(self, num_gpus):
    self._run_multiple_clients(
        3, self._test_minimize_loss_graph, num_gpus=num_gpus)
    self.assertEqual(self._result, 3)


if __name__ == '__main__':
  test.main()
