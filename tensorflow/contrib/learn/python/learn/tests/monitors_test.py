# pylint: disable=g-bad-file-header
# Copyright 2016 The TensorFlow Authors. All Rights Reserved.
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

"""Monitors tests."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from six.moves import xrange  # pylint: disable=redefined-builtin
import tensorflow as tf

from tensorflow.contrib import testing
from tensorflow.contrib.learn.python import learn


class _MyEveryN(learn.monitors.EveryN):

  def __init__(self, every_n_steps=100, first_n_steps=1):
    super(_MyEveryN, self).__init__(
        every_n_steps=every_n_steps, first_n_steps=first_n_steps)
    self._steps_begun = []
    self._steps_ended = []

  @property
  def steps_begun(self):
    return self._steps_begun

  @property
  def steps_ended(self):
    return self._steps_ended

  def every_n_step_begin(self, step):
    super(_MyEveryN, self).every_n_step_begin(step)
    self._steps_begun.append(step)
    return []

  def every_n_step_end(self, step, outputs):
    super(_MyEveryN, self).every_n_step_end(step, outputs)
    self._steps_ended.append(step)
    return False


class MonitorsTest(tf.test.TestCase):
  """Monitors tests."""

  def _run_monitor(self, monitor, num_epochs=3, num_steps_per_epoch=10):
    monitor.begin(max_steps=(num_epochs * num_steps_per_epoch) - 1)
    for epoch in xrange(num_epochs):
      monitor.epoch_begin(epoch)
      should_stop = False
      step = epoch * num_steps_per_epoch
      next_epoch_step = step + num_steps_per_epoch
      while (not should_stop) and (step < next_epoch_step):
        tensors = monitor.step_begin(step)
        output = tf.get_default_session().run(tensors) if tensors else {}
        output = dict(zip(
            [t.name if isinstance(t, tf.Tensor) else t for t in tensors],
            output))
        should_stop = monitor.step_end(step=step, output=output)
        step += 1
      monitor.epoch_end(epoch)
    monitor.end()

  def test_base_monitor(self):
    with tf.Graph().as_default() as g, self.test_session(g):
      self._run_monitor(learn.monitors.BaseMonitor())

  def test_every_n(self):
    monitor = _MyEveryN(every_n_steps=8, first_n_steps=2)
    with tf.Graph().as_default() as g, self.test_session(g):
      self._run_monitor(monitor, num_epochs=3, num_steps_per_epoch=10)
      expected_steps = [0, 1, 2, 10, 18, 26, 29]
      self.assertEqual(expected_steps, monitor.steps_begun)
      self.assertEqual(expected_steps, monitor.steps_ended)

  # TODO(b/29293803): This is just a sanity check for now, add better tests with
  # a mocked logger.
  def test_print(self):
    with tf.Graph().as_default() as g, self.test_session(g):
      t = tf.constant(42.0, name='foo')
      self._run_monitor(learn.monitors.PrintTensor(tensor_names=[t.name]))

  def test_summary_saver(self):
    with tf.Graph().as_default() as g, self.test_session(g):
      log_dir = 'log/dir'
      summary_writer = testing.FakeSummaryWriter(log_dir, g)
      var = tf.Variable(0.0)
      var.initializer.run()
      tensor = tf.assign_add(var, 1.0)
      summary_op = tf.scalar_summary('my_summary', tensor)
      self._run_monitor(
          learn.monitors.SummarySaver(
              summary_op=summary_op, save_steps=8,
              summary_writer=summary_writer),
          num_epochs=3, num_steps_per_epoch=10)
      summary_writer.assert_summaries(
          test_case=self, expected_logdir=log_dir, expected_graph=g,
          expected_summaries={
              0: {'my_summary': 1.0},
              1: {'my_summary': 2.0},
              9: {'my_summary': 3.0},
              17: {'my_summary': 4.0},
              25: {'my_summary': 5.0},
              29: {'my_summary': 6.0},
          })

  # TODO(b/29293803): Add better tests with a mocked estimator.
  def test_validation_monitor(self):
    monitor = learn.monitors.ValidationMonitor(x=tf.constant(2.0))
    with tf.Graph().as_default() as g, self.test_session(g):
      with self.assertRaisesRegexp(ValueError, 'set_estimator'):
        self._run_monitor(monitor)

  def test_graph_dump(self):
    monitor0 = learn.monitors.GraphDump()
    monitor1 = learn.monitors.GraphDump()
    with tf.Graph().as_default() as g, self.test_session(g):
      const_var = tf.Variable(42.0, name='my_const')
      counter_var = tf.Variable(0.0, name='my_counter')
      assign_add = tf.assign_add(counter_var, 1.0, name='my_assign_add')
      tf.initialize_all_variables().run()

      self._run_monitor(monitor0, num_epochs=3, num_steps_per_epoch=10)
      self.assertEqual({
          step: {
              const_var.name: 42.0,
              counter_var.name: step + 1.0,
              assign_add.name: step + 1.0,
          } for step in xrange(30)
      }, monitor0.data)

      self._run_monitor(monitor1, num_epochs=3, num_steps_per_epoch=10)
      self.assertEqual({
          step: {
              const_var.name: 42.0,
              counter_var.name: step + 31.0,
              assign_add.name: step + 31.0,
          } for step in xrange(30)
      }, monitor1.data)

      for step in xrange(30):
        matched, non_matched = monitor1.compare(monitor0, step=step)
        self.assertEqual([const_var.name], matched)
        self.assertEqual({
            assign_add.name: (step + 31.0, step + 1.0),
            counter_var.name: (step + 31.0, step + 1.0),
        }, non_matched)
        matched, non_matched = monitor0.compare(monitor1, step=step)
        self.assertEqual([const_var.name], matched)
        self.assertEqual({
            assign_add.name: (step + 1.0, step + 31.0),
            counter_var.name: (step + 1.0, step + 31.0),
        }, non_matched)

  def test_capture_variable(self):
    monitor = learn.monitors.CaptureVariable(
        var_name='my_assign_add:0', every_n=8, first_n=2)
    with tf.Graph().as_default() as g, self.test_session(g):
      var = tf.Variable(0.0, name='my_var')
      var.initializer.run()
      tf.assign_add(var, 1.0, name='my_assign_add')
      self._run_monitor(monitor, num_epochs=3, num_steps_per_epoch=10)
      self.assertEqual({
          0: 1.0,
          1: 2.0,
          2: 3.0,
          10: 4.0,
          18: 5.0,
          26: 6.0,
          29: 7.0,
      }, monitor.values)


if __name__ == '__main__':
  tf.test.main()
