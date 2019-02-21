# Copyright 2017 The TensorFlow Authors. All Rights Reserved.
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
"""Tests for V2 summary ops from summary_ops_v2."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import os
import time

from tensorflow.core.framework import graph_pb2
from tensorflow.core.framework import node_def_pb2
from tensorflow.core.framework import step_stats_pb2
from tensorflow.core.framework import summary_pb2
from tensorflow.core.protobuf import config_pb2
from tensorflow.core.util import event_pb2
from tensorflow.python.eager import context
from tensorflow.python.eager import def_function
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.framework import tensor_spec
from tensorflow.python.framework import tensor_util
from tensorflow.python.framework import test_util
from tensorflow.python.lib.io import tf_record
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import summary_ops_v2 as summary_ops
from tensorflow.python.ops import variables
from tensorflow.python.platform import gfile
from tensorflow.python.platform import test


class SummaryOpsCoreTest(test_util.TensorFlowTestCase):

  def testWrite(self):
    logdir = self.get_temp_dir()
    with context.eager_mode():
      with summary_ops.create_file_writer(logdir).as_default():
        output = summary_ops.write('tag', 42, step=12)
        self.assertTrue(output.numpy())
    events = events_from_logdir(logdir)
    self.assertEqual(2, len(events))
    self.assertEqual(12, events[1].step)
    value = events[1].summary.value[0]
    self.assertEqual('tag', value.tag)
    self.assertEqual(42, to_numpy(value))

  def testWrite_fromFunction(self):
    logdir = self.get_temp_dir()
    @def_function.function
    def f():
      with summary_ops.create_file_writer(logdir).as_default():
        return summary_ops.write('tag', 42, step=12)
    with context.eager_mode():
      output = f()
      self.assertTrue(output.numpy())
    events = events_from_logdir(logdir)
    self.assertEqual(2, len(events))
    self.assertEqual(12, events[1].step)
    value = events[1].summary.value[0]
    self.assertEqual('tag', value.tag)
    self.assertEqual(42, to_numpy(value))

  def testWrite_metadata(self):
    logdir = self.get_temp_dir()
    metadata = summary_pb2.SummaryMetadata()
    metadata.plugin_data.plugin_name = 'foo'
    with context.eager_mode():
      with summary_ops.create_file_writer(logdir).as_default():
        summary_ops.write('obj', 0, 0, metadata=metadata)
        summary_ops.write('bytes', 0, 0, metadata=metadata.SerializeToString())
        m = constant_op.constant(metadata.SerializeToString())
        summary_ops.write('string_tensor', 0, 0, metadata=m)
    events = events_from_logdir(logdir)
    self.assertEqual(4, len(events))
    self.assertEqual(metadata, events[1].summary.value[0].metadata)
    self.assertEqual(metadata, events[2].summary.value[0].metadata)
    self.assertEqual(metadata, events[3].summary.value[0].metadata)

  def testWrite_name(self):
    @def_function.function
    def f():
      output = summary_ops.write('tag', 42, step=12, name='anonymous')
      self.assertTrue(output.name.startswith('anonymous'))
    f()

  def testWrite_ndarray(self):
    logdir = self.get_temp_dir()
    with context.eager_mode():
      with summary_ops.create_file_writer(logdir).as_default():
        summary_ops.write('tag', [[1, 2], [3, 4]], step=12)
    events = events_from_logdir(logdir)
    value = events[1].summary.value[0]
    self.assertAllEqual([[1, 2], [3, 4]], to_numpy(value))

  def testWrite_tensor(self):
    logdir = self.get_temp_dir()
    with context.eager_mode():
      t = constant_op.constant([[1, 2], [3, 4]])
      with summary_ops.create_file_writer(logdir).as_default():
        summary_ops.write('tag', t, step=12)
      expected = t.numpy()
    events = events_from_logdir(logdir)
    value = events[1].summary.value[0]
    self.assertAllEqual(expected, to_numpy(value))

  def testWrite_tensor_fromFunction(self):
    logdir = self.get_temp_dir()
    @def_function.function
    def f(t):
      with summary_ops.create_file_writer(logdir).as_default():
        summary_ops.write('tag', t, step=12)
    with context.eager_mode():
      t = constant_op.constant([[1, 2], [3, 4]])
      f(t)
      expected = t.numpy()
    events = events_from_logdir(logdir)
    value = events[1].summary.value[0]
    self.assertAllEqual(expected, to_numpy(value))

  def testWrite_stringTensor(self):
    logdir = self.get_temp_dir()
    with context.eager_mode():
      with summary_ops.create_file_writer(logdir).as_default():
        summary_ops.write('tag', [b'foo', b'bar'], step=12)
    events = events_from_logdir(logdir)
    value = events[1].summary.value[0]
    self.assertAllEqual([b'foo', b'bar'], to_numpy(value))

  @test_util.also_run_as_tf_function
  def testWrite_noDefaultWriter(self):
    with context.eager_mode():
      self.assertFalse(summary_ops.write('tag', 42, step=0))

  def testWrite_recordIf_constant(self):
    logdir = self.get_temp_dir()
    with context.eager_mode():
      with summary_ops.create_file_writer(logdir).as_default():
        self.assertTrue(summary_ops.write('default', 1, step=0))
        with summary_ops.record_if(True):
          self.assertTrue(summary_ops.write('set_on', 1, step=0))
        with summary_ops.record_if(False):
          self.assertFalse(summary_ops.write('set_off', 1, step=0))
    events = events_from_logdir(logdir)
    self.assertEqual(3, len(events))
    self.assertEqual('default', events[1].summary.value[0].tag)
    self.assertEqual('set_on', events[2].summary.value[0].tag)

  def testWrite_recordIf_constant_fromFunction(self):
    logdir = self.get_temp_dir()
    @def_function.function
    def f():
      with summary_ops.create_file_writer(logdir).as_default():
        # Use assertAllEqual instead of assertTrue since it works in a defun.
        self.assertAllEqual(summary_ops.write('default', 1, step=0), True)
        with summary_ops.record_if(True):
          self.assertAllEqual(summary_ops.write('set_on', 1, step=0), True)
        with summary_ops.record_if(False):
          self.assertAllEqual(summary_ops.write('set_off', 1, step=0), False)
    with context.eager_mode():
      f()
    events = events_from_logdir(logdir)
    self.assertEqual(3, len(events))
    self.assertEqual('default', events[1].summary.value[0].tag)
    self.assertEqual('set_on', events[2].summary.value[0].tag)

  def testWrite_recordIf_callable(self):
    logdir = self.get_temp_dir()
    with context.eager_mode():
      step = variables.Variable(-1, dtype=dtypes.int64)
      def record_fn():
        step.assign_add(1)
        return int(step % 2) == 0
      with summary_ops.create_file_writer(logdir).as_default():
        with summary_ops.record_if(record_fn):
          self.assertTrue(summary_ops.write('tag', 1, step=step))
          self.assertFalse(summary_ops.write('tag', 1, step=step))
          self.assertTrue(summary_ops.write('tag', 1, step=step))
          self.assertFalse(summary_ops.write('tag', 1, step=step))
          self.assertTrue(summary_ops.write('tag', 1, step=step))
    events = events_from_logdir(logdir)
    self.assertEqual(4, len(events))
    self.assertEqual(0, events[1].step)
    self.assertEqual(2, events[2].step)
    self.assertEqual(4, events[3].step)

  def testWrite_recordIf_callable_fromFunction(self):
    logdir = self.get_temp_dir()
    with context.eager_mode():
      step = variables.Variable(-1, dtype=dtypes.int64)
      @def_function.function
      def record_fn():
        step.assign_add(1)
        return math_ops.equal(step % 2, 0)
      @def_function.function
      def f():
        with summary_ops.create_file_writer(logdir).as_default():
          with summary_ops.record_if(record_fn):
            return [
                summary_ops.write('tag', 1, step=step),
                summary_ops.write('tag', 1, step=step),
                summary_ops.write('tag', 1, step=step)]
      self.assertAllEqual(f(), [True, False, True])
      self.assertAllEqual(f(), [False, True, False])
    events = events_from_logdir(logdir)
    self.assertEqual(4, len(events))
    self.assertEqual(0, events[1].step)
    self.assertEqual(2, events[2].step)
    self.assertEqual(4, events[3].step)

  def testWrite_recordIf_tensorInput_fromFunction(self):
    logdir = self.get_temp_dir()
    @def_function.function(input_signature=[
        tensor_spec.TensorSpec(shape=[], dtype=dtypes.int64)])
    def f(step):
      with summary_ops.create_file_writer(logdir).as_default():
        with summary_ops.record_if(math_ops.equal(step % 2, 0)):
          return summary_ops.write('tag', 1, step=step)
    with context.eager_mode():
      self.assertTrue(f(0))
      self.assertFalse(f(1))
      self.assertTrue(f(2))
      self.assertFalse(f(3))
      self.assertTrue(f(4))
    events = events_from_logdir(logdir)
    self.assertEqual(4, len(events))
    self.assertEqual(0, events[1].step)
    self.assertEqual(2, events[2].step)
    self.assertEqual(4, events[3].step)

  @test_util.also_run_as_tf_function
  def testSummaryScope(self):
    with summary_ops.summary_scope('foo') as (tag, scope):
      self.assertEqual('foo', tag)
      self.assertEqual('foo/', scope)
      with summary_ops.summary_scope('bar') as (tag, scope):
        self.assertEqual('foo/bar', tag)
        self.assertEqual('foo/bar/', scope)
      with summary_ops.summary_scope('with/slash') as (tag, scope):
        self.assertEqual('foo/with/slash', tag)
        self.assertEqual('foo/with/slash/', scope)
      with ops.name_scope(None):
        with summary_ops.summary_scope('unnested') as (tag, scope):
          self.assertEqual('unnested', tag)
          self.assertEqual('unnested/', scope)

  @test_util.also_run_as_tf_function
  def testSummaryScope_defaultName(self):
    with summary_ops.summary_scope(None) as (tag, scope):
      self.assertEqual('summary', tag)
      self.assertEqual('summary/', scope)
    with summary_ops.summary_scope(None, 'backup') as (tag, scope):
      self.assertEqual('backup', tag)
      self.assertEqual('backup/', scope)

  @test_util.also_run_as_tf_function
  def testSummaryScope_handlesCharactersIllegalForScope(self):
    with summary_ops.summary_scope('f?o?o') as (tag, scope):
      self.assertEqual('f?o?o', tag)
      self.assertEqual('foo/', scope)
    # If all characters aren't legal for a scope name, use default name.
    with summary_ops.summary_scope('???', 'backup') as (tag, scope):
      self.assertEqual('???', tag)
      self.assertEqual('backup/', scope)

  @test_util.also_run_as_tf_function
  def testSummaryScope_nameNotUniquifiedForTag(self):
    constant_op.constant(0, name='foo')
    with summary_ops.summary_scope('foo') as (tag, _):
      self.assertEqual('foo', tag)
    with summary_ops.summary_scope('foo') as (tag, _):
      self.assertEqual('foo', tag)
    with ops.name_scope('with'):
      constant_op.constant(0, name='slash')
    with summary_ops.summary_scope('with/slash') as (tag, _):
      self.assertEqual('with/slash', tag)


class SummaryWriterTest(test_util.TensorFlowTestCase):

  def testWriterInitAndClose(self):
    logdir = self.get_temp_dir()
    with context.eager_mode():
      writer = summary_ops.create_file_writer(
          logdir, max_queue=1000, flush_millis=1000000)
      files = gfile.Glob(os.path.join(logdir, '*'))
      self.assertEqual(1, len(files))
      file1 = files[0]
      self.assertEqual(1, len(events_from_file(file1)))  # file_version Event
      # Calling init() again while writer is open has no effect
      writer.init()
      self.assertEqual(1, len(events_from_file(file1)))
      with writer.as_default():
        summary_ops.write('tag', 1, step=0)
        self.assertEqual(1, len(events_from_file(file1)))
        # Calling .close() should do an implicit flush
        writer.close()
        self.assertEqual(2, len(events_from_file(file1)))
        # Calling init() on a closed writer should start a new file
        time.sleep(1.1)  # Ensure filename has a different timestamp
        writer.init()
        files = gfile.Glob(os.path.join(logdir, '*'))
        self.assertEqual(2, len(files))
        files.remove(file1)
        file2 = files[0]
        self.assertEqual(1, len(events_from_file(file2)))  # file_version
        self.assertEqual(2, len(events_from_file(file1)))  # should be unchanged

  def testSharedName(self):
    logdir = self.get_temp_dir()
    with context.eager_mode():
      # Create with default shared name (should match logdir)
      writer1 = summary_ops.create_file_writer(logdir)
      with writer1.as_default():
        summary_ops.write('tag', 1, step=1)
        summary_ops.flush()
      # Create with explicit logdir shared name (should be same resource/file)
      shared_name = 'logdir:' + logdir
      writer2 = summary_ops.create_file_writer(logdir, name=shared_name)
      with writer2.as_default():
        summary_ops.write('tag', 1, step=2)
        summary_ops.flush()
      # Create with different shared name (should be separate resource/file)
      time.sleep(1.1)  # Ensure filename has a different timestamp
      writer3 = summary_ops.create_file_writer(logdir, name='other')
      with writer3.as_default():
        summary_ops.write('tag', 1, step=3)
        summary_ops.flush()

    event_files = iter(sorted(gfile.Glob(os.path.join(logdir, '*'))))

    # First file has tags "one" and "two"
    events = iter(events_from_file(next(event_files)))
    self.assertEqual('brain.Event:2', next(events).file_version)
    self.assertEqual(1, next(events).step)
    self.assertEqual(2, next(events).step)
    self.assertRaises(StopIteration, lambda: next(events))

    # Second file has tag "three"
    events = iter(events_from_file(next(event_files)))
    self.assertEqual('brain.Event:2', next(events).file_version)
    self.assertEqual(3, next(events).step)
    self.assertRaises(StopIteration, lambda: next(events))

    # No more files
    self.assertRaises(StopIteration, lambda: next(event_files))

  def testMaxQueue(self):
    logdir = self.get_temp_dir()
    with context.eager_mode():
      with summary_ops.create_file_writer(
          logdir, max_queue=1, flush_millis=999999).as_default():
        get_total = lambda: len(events_from_logdir(logdir))
        # Note: First tf.Event is always file_version.
        self.assertEqual(1, get_total())
        summary_ops.write('tag', 1, step=0)
        self.assertEqual(1, get_total())
        # Should flush after second summary since max_queue = 1
        summary_ops.write('tag', 1, step=0)
        self.assertEqual(3, get_total())

  def testWriterFlush(self):
    logdir = self.get_temp_dir()
    get_total = lambda: len(events_from_logdir(logdir))
    with context.eager_mode():
      writer = summary_ops.create_file_writer(
          logdir, max_queue=1000, flush_millis=1000000)
      self.assertEqual(1, get_total())  # file_version Event
      with writer.as_default():
        summary_ops.write('tag', 1, step=0)
        self.assertEqual(1, get_total())
        writer.flush()
        self.assertEqual(2, get_total())
        summary_ops.write('tag', 1, step=0)
        self.assertEqual(2, get_total())
      # Exiting the "as_default()" should do an implicit flush
      self.assertEqual(3, get_total())

  def testFlushFunction(self):
    logdir = self.get_temp_dir()
    with context.eager_mode():
      writer = summary_ops.create_file_writer(
          logdir, max_queue=999999, flush_millis=999999)
      with writer.as_default(), summary_ops.always_record_summaries():
        get_total = lambda: len(events_from_logdir(logdir))
        # Note: First tf.Event is always file_version.
        self.assertEqual(1, get_total())
        summary_ops.write('tag', 1, step=0)
        summary_ops.write('tag', 1, step=0)
        self.assertEqual(1, get_total())
        summary_ops.flush()
        self.assertEqual(3, get_total())
        # Test "writer" parameter
        summary_ops.write('tag', 1, step=0)
        self.assertEqual(3, get_total())
        summary_ops.flush(writer=writer)
        self.assertEqual(4, get_total())
        summary_ops.write('tag', 1, step=0)
        self.assertEqual(4, get_total())
        summary_ops.flush(writer=writer._resource)  # pylint:disable=protected-access
        self.assertEqual(5, get_total())

  @test_util.assert_no_new_pyobjects_executing_eagerly
  def testEagerMemory(self):
    logdir = self.get_temp_dir()
    with summary_ops.create_file_writer(logdir).as_default():
      summary_ops.write('tag', 1, step=0)


class SummaryOpsTest(test_util.TensorFlowTestCase):

  def run_metadata(self, *args, **kwargs):
    assert context.executing_eagerly()
    logdir = self.get_temp_dir()
    writer = summary_ops.create_file_writer(logdir)
    with writer.as_default():
      summary_ops.run_metadata(*args, **kwargs)
    writer.close()
    events = events_from_logdir(logdir)
    return events[1].summary

  def run_metadata_graphs(self, *args, **kwargs):
    assert context.executing_eagerly()
    logdir = self.get_temp_dir()
    writer = summary_ops.create_file_writer(logdir)
    with writer.as_default():
      summary_ops.run_metadata_graphs(*args, **kwargs)
    writer.close()
    events = events_from_logdir(logdir)
    return events[1].summary

  def create_run_metadata(self):
    step_stats = step_stats_pb2.StepStats(dev_stats=[
        step_stats_pb2.DeviceStepStats(
            device='cpu:0',
            node_stats=[step_stats_pb2.NodeExecStats(node_name='hello')])
    ])
    return config_pb2.RunMetadata(
        function_graphs=[
            config_pb2.RunMetadata.FunctionGraphs(
                pre_optimization_graph=graph_pb2.GraphDef(
                    node=[node_def_pb2.NodeDef(name='foo')]))
        ],
        step_stats=step_stats)

  @test_util.run_v2_only
  def testRunMetadata_usesNameAsTag(self):
    meta = config_pb2.RunMetadata()

    with ops.name_scope('foo'):
      summary = self.run_metadata(name='my_name', data=meta, step=1)
      first_val = summary.value[0]

    self.assertEqual('foo/my_name', first_val.tag)

  @test_util.run_v2_only
  def testRunMetadata_summaryMetadata(self):
    expected_summary_metadata = """
      plugin_data {
        plugin_name: "graph_run_metadata"
        content: "1"
      }
    """
    meta = config_pb2.RunMetadata()
    summary = self.run_metadata(name='my_name', data=meta, step=1)
    actual_summary_metadata = summary.value[0].metadata
    self.assertProtoEquals(expected_summary_metadata, actual_summary_metadata)

  @test_util.run_v2_only
  def testRunMetadata_wholeRunMetadata(self):
    expected_run_metadata = """
      step_stats {
        dev_stats {
          device: "cpu:0"
          node_stats {
            node_name: "hello"
          }
        }
      }
      function_graphs {
        pre_optimization_graph {
          node {
            name: "foo"
          }
        }
      }
    """
    meta = self.create_run_metadata()
    summary = self.run_metadata(name='my_name', data=meta, step=1)
    first_val = summary.value[0]

    actual_run_metadata = config_pb2.RunMetadata.FromString(
        first_val.tensor.string_val[0])
    self.assertProtoEquals(expected_run_metadata, actual_run_metadata)

  @test_util.run_v2_only
  def testRunMetadataGraph_usesNameAsTag(self):
    meta = config_pb2.RunMetadata()

    with ops.name_scope('foo'):
      summary = self.run_metadata_graphs(name='my_name', data=meta, step=1)
      first_val = summary.value[0]

    self.assertEqual('foo/my_name', first_val.tag)

  @test_util.run_v2_only
  def testRunMetadataGraph_summaryMetadata(self):
    expected_summary_metadata = """
      plugin_data {
        plugin_name: "graph_run_metadata_graph"
        content: "1"
      }
    """
    meta = config_pb2.RunMetadata()
    summary = self.run_metadata_graphs(name='my_name', data=meta, step=1)
    actual_summary_metadata = summary.value[0].metadata
    self.assertProtoEquals(expected_summary_metadata, actual_summary_metadata)

  @test_util.run_v2_only
  def testRunMetadataGraph_runMetadataFragment(self):
    expected_run_metadata = """
      function_graphs {
        pre_optimization_graph {
          node {
            name: "foo"
          }
        }
      }
    """
    meta = self.create_run_metadata()

    summary = self.run_metadata_graphs(name='my_name', data=meta, step=1)
    first_val = summary.value[0]

    actual_run_metadata = config_pb2.RunMetadata.FromString(
        first_val.tensor.string_val[0])
    self.assertProtoEquals(expected_run_metadata, actual_run_metadata)


def events_from_file(filepath):
  """Returns all events in a single event file.

  Args:
    filepath: Path to the event file.

  Returns:
    A list of all tf.Event protos in the event file.
  """
  records = list(tf_record.tf_record_iterator(filepath))
  result = []
  for r in records:
    event = event_pb2.Event()
    event.ParseFromString(r)
    result.append(event)
  return result


def events_from_logdir(logdir):
  """Returns all events in the single eventfile in logdir.

  Args:
    logdir: The directory in which the single event file is sought.

  Returns:
    A list of all tf.Event protos from the single event file.

  Raises:
    AssertionError: If logdir does not contain exactly one file.
  """
  assert gfile.Exists(logdir)
  files = gfile.ListDirectory(logdir)
  assert len(files) == 1, 'Found not exactly one file in logdir: %s' % files
  return events_from_file(os.path.join(logdir, files[0]))


def to_numpy(summary_value):
  return tensor_util.MakeNdarray(summary_value.tensor)


if __name__ == '__main__':
  test.main()
