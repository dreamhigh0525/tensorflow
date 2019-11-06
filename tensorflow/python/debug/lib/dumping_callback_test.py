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
"""Unit tests for tfdbg v2 dumping callback."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import collections
import os
import shutil
import tempfile
import threading

from absl.testing import parameterized
import numpy as np

from tensorflow.core.protobuf import debug_event_pb2
from tensorflow.python.debug.lib import debug_events_reader
from tensorflow.python.debug.lib import dumping_callback
from tensorflow.python.debug.lib import dumping_callback_test_lib
from tensorflow.python.eager import context
from tensorflow.python.eager import def_function
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.framework import tensor_util
from tensorflow.python.framework import test_util
from tensorflow.python.keras import models
from tensorflow.python.keras.applications import mobilenet_v2
from tensorflow.python.keras.layers import core
from tensorflow.python.keras.layers import recurrent_v2
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import variables
from tensorflow.python.platform import googletest


def _create_simple_recurrent_keras_model(input_shape):
  """Create a simple tf.keras model containing a recurrent layer for testing."""
  model = models.Sequential()
  model.add(recurrent_v2.LSTM(
      10,
      input_shape=input_shape,
      kernel_initializer="zeros",
      recurrent_initializer="zeros"))
  model.add(core.Dense(1, kernel_initializer="zeros"))
  model.compile(loss="mse", optimizer="sgd")
  return model


class TracingCallbackTest(
    dumping_callback_test_lib.DumpingCallbackTestBase, parameterized.TestCase):

  def setUp(self):
    super(TracingCallbackTest, self).setUp()
    self.dump_root = tempfile.mkdtemp()

  def tearDown(self):
    if os.path.isdir(self.dump_root):
      shutil.rmtree(self.dump_root, ignore_errors=True)
    dumping_callback.disable_dumping()
    super(TracingCallbackTest, self).tearDown()

  def testInvalidTensorDebugModeCausesError(self):
    with self.assertRaisesRegexp(
        ValueError,
        r"Invalid value in tensor_debug_mode \(\'NONSENSICAL\'\).*"
        r"Valid options.*NO_TENSOR.*"):
      dumping_callback.enable_dumping(
          self.dump_root, tensor_debug_mode="NONSENSICAL")

  def testDisablingTracingCallbackWithoutEnablingFirstIsTolerated(self):
    dumping_callback.disable_dumping()

  @parameterized.named_parameters(
      ("NoTensor", "NO_TENSOR"),
      ("FullTensor", "FULL_TENSOR"),
  )
  def testPureEagerOpExecution(self, tensor_debug_mode):
    """Test catching Infinity in eager op execution: float32."""
    writer = dumping_callback.enable_dumping(
        self.dump_root, tensor_debug_mode=tensor_debug_mode)

    x = constant_op.constant(10.0)
    zero = constant_op.constant(0.0)
    one = constant_op.constant(1.0)
    two = constant_op.constant(2.0)
    three = constant_op.constant(3.0)
    # Use Collatz conjecture as a test case.
    while x > one:
      if math_ops.equal(x % two, zero):
        x = x / two
      else:
        x = x * three + one

    writer.FlushNonExecutionFiles()
    self._readAndCheckMetadataFile()
    stack_frame_by_id = self._readAndCheckSourceFilesAndStackFrames()

    # Before FlushExecutionFiles() is called, the .execution file should be
    # empty.
    reader = debug_events_reader.DebugEventsReader(self.dump_root)
    execution_iter = reader.execution_iterator()
    with self.assertRaises(StopIteration):
      next(execution_iter)

    # After the flushing, the .execution file should hold the appropriate
    # contents.
    writer.FlushExecutionFiles()
    execution_iter = reader.execution_iterator()
    prev_wall_time = 1
    executed_op_types = []
    tensor_values = collections.defaultdict(lambda: [])
    for debug_event in execution_iter:
      self.assertGreaterEqual(debug_event.wall_time, prev_wall_time)
      prev_wall_time = debug_event.wall_time
      execution = debug_event.execution
      executed_op_types.append(execution.op_type)
      self.assertTrue(execution.input_tensor_ids)
      self.assertTrue(execution.output_tensor_ids)
      if tensor_debug_mode == "NO_TENSOR":
        # Due to the NO_TENSOR tensor debug mode, tensor_protos ought to
        # be empty.
        self.assertFalse(execution.tensor_protos)
      elif tensor_debug_mode == "FULL_TENSOR":
        # Under the FULL_TENSOR mode, the value of the tensor should be
        # available through `tensor_protos`.
        tensor_value = float(
            tensor_util.MakeNdarray(execution.tensor_protos[0]))
        tensor_values[execution.op_type].append(tensor_value)
      # Verify the code_location field.
      self.assertTrue(execution.code_location.stack_frame_ids)
      for stack_frame_id in execution.code_location.stack_frame_ids:
        self.assertIn(stack_frame_id, stack_frame_by_id)
    if tensor_debug_mode == "FULL_TENSOR":
      self.assertAllClose(tensor_values["Greater"], [1, 1, 1, 1, 1, 1, 0])
      self.assertAllClose(tensor_values["RealDiv"], [5, 8, 4, 2, 1])
      self.assertAllClose(tensor_values["Mul"], [15])
      self.assertAllClose(tensor_values["AddV2"], [16])

    self.assertEqual(
        executed_op_types,
        [
            "Greater",
            "FloorMod",
            "Equal",
            "RealDiv",  # 10 --> 5
            "Greater",
            "FloorMod",
            "Equal",
            "Mul",
            "AddV2",  # 5 --> 16
            "Greater",
            "FloorMod",
            "Equal",
            "RealDiv",  # 16 --> 8
            "Greater",
            "FloorMod",
            "Equal",
            "RealDiv",  # 8 --> 4
            "Greater",
            "FloorMod",
            "Equal",
            "RealDiv",  # 4 --> 2
            "Greater",
            "FloorMod",
            "Equal",
            "RealDiv",  # 2 --> 1
            "Greater"
        ])

    # Due to the pure eager op execution, the .graph file and the
    # .graph_execution_traces file ought to be empty.
    graphs_iterator = reader.graphs_iterator()
    with self.assertRaises(StopIteration):
      next(graphs_iterator)
    graph_trace_iter = reader.graph_execution_traces_iterator()
    with self.assertRaises(StopIteration):
      next(graph_trace_iter)

  @parameterized.named_parameters(
      ("NoTensor", "NO_TENSOR"),
      ("FullTensor", "FULL_TENSOR"),
  )
  @test_util.run_in_graph_and_eager_modes
  def testNestedFunctionExecutionWithoutControlFlow(self, tensor_debug_mode):
    writer = dumping_callback.enable_dumping(
        self.dump_root, tensor_debug_mode=tensor_debug_mode)

    @def_function.function
    def log_sum(x, y):
      return math_ops.log(x + y)

    @def_function.function
    def sin1p_log_sum(x, y):
      return math_ops.sin(1.0 + log_sum(x, y))

    x = constant_op.constant(2.0)
    y = constant_op.constant(3.0)
    self.assertAllClose(sin1p_log_sum(x, y), np.sin(1.0 + np.log(5.0)))
    writer.FlushNonExecutionFiles()
    writer.FlushExecutionFiles()

    if context.executing_eagerly():
      # NOTE(b/142486213): Execution of the TF function happens with
      # Session.run() in v1 graph mode, so doesn't get logged to the
      # .execution file.
      executed_op_types, _, _, _, _ = self._readAndCheckExecutionFile()
      self.assertLen(executed_op_types, 1)
      self.assertIn("sin1p_log_sum", executed_op_types[0])

    stack_frame_by_id = self._readAndCheckSourceFilesAndStackFrames()
    (context_ids, op_types,
     op_name_to_op_type) = self._readAndCheckGraphsFile(stack_frame_by_id)
    self.assertIn("AddV2", op_types)
    self.assertIn("Log", op_types)
    self.assertIn("Sin", op_types)

    (op_names, _, _,
     tensor_values) = self._readAndCheckGraphExecutionTracesFile(context_ids)
    executed_op_types = [op_name_to_op_type[op_name] for op_name in op_names]
    self.assertEqual(executed_op_types, ["AddV2", "Log", "AddV2", "Sin"])

    if tensor_debug_mode == "NO_TENSOR":
      # Under the default NO_TENSOR tensor-debug mode, the tensor_proto ought to
      # be an empty float32 tensor.
      for tensor_value in tensor_values:
        self.assertEqual(tensor_value.dtype, np.float32)
        self.assertEqual(tensor_value.shape, (0,))
    elif tensor_debug_mode == "FULL_TENSOR":
      self.assertAllClose(tensor_values[0], 5.0)  # 1st AddV2 op.
      self.assertAllClose(tensor_values[1], np.log(5.0))  # Log op.
      self.assertAllClose(tensor_values[2], np.log(5.0) + 1.0)  # 2nd AddV2 op.
      self.assertAllClose(tensor_values[3],
                          np.sin(np.log(5.0) + 1.0))  # Sin op.

  @parameterized.named_parameters(
      ("NoTensor", "NO_TENSOR"),
      ("FullTensor", "FULL_TENSOR"),
  )
  @test_util.run_in_graph_and_eager_modes
  def testFunctionExecutionWithControlFlow(self, tensor_debug_mode):
    writer = dumping_callback.enable_dumping(
        self.dump_root, tensor_debug_mode=tensor_debug_mode)

    @def_function.function
    def iterative_doubling(x, times):
      i = constant_op.constant(0, dtype=dtypes.int32)
      while i < times:
        x = x * 2.0
        i += 1
      return x

    x = constant_op.constant(0.5, dtype=dtypes.float32)
    times = constant_op.constant(4, dtype=dtypes.int32)
    self.assertAllClose(self.evaluate(iterative_doubling(x, times)), 8.0)

    writer.FlushNonExecutionFiles()
    stack_frame_by_id = self._readAndCheckSourceFilesAndStackFrames()

    # Verify the content of the .graphs file.
    context_ids, op_types, op_name_to_op_type = (
        self._readAndCheckGraphsFile(stack_frame_by_id))
    self.assertIn("Less", op_types)
    self.assertIn("Mul", op_types)
    self.assertIn("AddV2", op_types)

    # Before FlushExecutionFiles() is called, the .execution and
    # .graph_execution_traces files should be both empty.
    reader = debug_events_reader.DebugEventsReader(self.dump_root)
    execution_iter = reader.execution_iterator()
    graph_execution_traces_iter = reader.graph_execution_traces_iterator()
    with self.assertRaises(StopIteration):
      next(execution_iter)
    with self.assertRaises(StopIteration):
      next(graph_execution_traces_iter)

    # TODO(cais): Backport execution instrumentation to tf.Session.
    writer.FlushExecutionFiles()
    # After the flushing, the .execution file should hold the appropriate
    # contents.
    if context.executing_eagerly():
      (executed_op_types, input_tensor_ids, output_tensor_ids,
       tensor_debug_modes, tensor_values) = self._readAndCheckExecutionFile()
      # NOTE(b/142486213): Execution of the TF function happens with
      # Session.run() in v1 graph mode, hence it doesn't get logged to the
      # .execution file.
      self.assertLen(executed_op_types, 1)
      self.assertIn("iterative_doubling", executed_op_types[0])
      self.assertLen(input_tensor_ids[0], 2)
      self.assertLen(output_tensor_ids[0], 1)
      self.assertEqual(tensor_debug_modes[0],
                       debug_event_pb2.TensorDebugMode.Value(tensor_debug_mode))
      if tensor_debug_mode == "FULL_TENSOR":
        self.assertAllClose(tensor_values, [[8.0]])

    (op_names, _, output_slots,
     tensor_values) = self._readAndCheckGraphExecutionTracesFile(context_ids)
    executed_op_types = [op_name_to_op_type[op_name] for op_name in op_names]
    # The Less op should have been executed 5 times.
    self.assertEqual(executed_op_types.count("Less"), 5)
    # The last executed op should be Less.
    self.assertEqual(executed_op_types[-1], "Less")
    # The Mul op should have been executed 4 times.
    self.assertEqual(executed_op_types.count("Mul"), 4)
    # The AddV2 op should have been run, but we refrain from asserting on how
    # many times it's executed.
    self.assertIn("AddV2", executed_op_types)
    for output_slot in output_slots:
      self.assertEqual(output_slot, 0)
    if tensor_debug_mode == "NO_TENSOR":
      # Under the default NO_TENSOR tensor-debug mode, the tensor_proto ought to
      # be an empty float32 tensor.
      for tensor_value in tensor_values:
        self.assertEqual(tensor_value.dtype, np.float32)
        self.assertEqual(tensor_value.shape, (0,))
    elif tensor_debug_mode == "FULL_TENSOR":
      less_values = [
          tensor_values[i]
          for i, op_type in enumerate(executed_op_types)
          if op_type == "Less"
      ]
      self.assertAllClose(less_values, [True, True, True, True, False])
      mul_values = [
          tensor_values[i]
          for i, op_type in enumerate(executed_op_types)
          if op_type == "Mul"
      ]
      self.assertAllClose(mul_values, [1.0, 2.0, 4.0, 8.0])

  def testCallingEnableTracingTwiceWithTheSameDumpRootIsIdempotent(self):
    dumping_callback.enable_dumping(self.dump_root)
    writer = dumping_callback.enable_dumping(self.dump_root)

    x = constant_op.constant([10.0, 12.0, 10.0])
    for _ in range(2):
      array_ops.unique(x)

    writer.FlushNonExecutionFiles()
    writer.FlushExecutionFiles()

    reader = debug_events_reader.DebugEventsReader(self.dump_root)
    execution_iter = reader.execution_iterator()
    for _ in range(2):
      debug_event = next(execution_iter)
      self.assertGreater(debug_event.wall_time, 0)
      execution = debug_event.execution
      self.assertEqual(execution.op_type, "Unique")
      self.assertEqual(execution.num_outputs, 2)
      self.assertTrue(execution.code_location)
    with self.assertRaises(StopIteration):
      next(execution_iter)

  def testCallingEnableTracingTwiceWithDifferentDumpRootsOverwrites(self):
    dumping_callback.enable_dumping(self.dump_root)
    new_dump_root = self.dump_root + "_new_dump_root"
    writer = dumping_callback.enable_dumping(new_dump_root)

    x = constant_op.constant([10.0, 12.0, 10.0])
    for _ in range(2):
      array_ops.unique(x)

    writer.FlushNonExecutionFiles()
    writer.FlushExecutionFiles()

    reader = debug_events_reader.DebugEventsReader(new_dump_root)
    execution_iter = reader.execution_iterator()
    for _ in range(2):
      debug_event = next(execution_iter)
      self.assertGreater(debug_event.wall_time, 0)
      execution = debug_event.execution
      self.assertEqual(execution.op_type, "Unique")
      self.assertEqual(execution.num_outputs, 2)
      self.assertTrue(execution.code_location)
    with self.assertRaises(StopIteration):
      next(execution_iter)

    old_dump_root_reader = debug_events_reader.DebugEventsReader(self.dump_root)
    execution_iter = old_dump_root_reader.execution_iterator()
    # The old dump root shouldn't have been written to.
    with self.assertRaises(StopIteration):
      next(execution_iter)

  def testCallingEnableRepeatedlyWithDifferentTensorDebugMode(self):
    """Assert that calling enable_dumping() with different tensor-debug modes.

    It should lead to overwriting of the previously-configured mode.
    """
    writer = dumping_callback.enable_dumping(
        self.dump_root, tensor_debug_mode="NO_TENSOR")

    @def_function.function
    def add_1_divide_by_2(x):
      return (x + 1.0) / 2.0

    self.assertAllClose(add_1_divide_by_2(constant_op.constant(4.0)), 2.5)
    writer.FlushNonExecutionFiles()
    writer.FlushExecutionFiles()
    stack_frame_by_id = self._readAndCheckSourceFilesAndStackFrames()
    context_ids, _, _ = self._readAndCheckGraphsFile(stack_frame_by_id)
    _, _, _, _, tensor_values = self._readAndCheckExecutionFile()
    self.assertEqual(tensor_values, [[]])
    (_, _, _,
     tensor_values) = self._readAndCheckGraphExecutionTracesFile(context_ids)
    self.assertLen(tensor_values, 2)
    for tensor_value in tensor_values:
      self.assertEqual(tensor_value.dtype, np.float32)
      self.assertEqual(tensor_value.shape, (0,))

    with self.assertRaisesRegexp(
        ValueError, r"already.*NO_TENSOR.*FULL_TENSOR.*not be honored"):
      dumping_callback.enable_dumping(
          self.dump_root, tensor_debug_mode="FULL_TENSOR")

  @parameterized.named_parameters(
      ("NoTensor", "NO_TENSOR"),
      ("FullTensor", "FULL_TENSOR"),
  )
  def testDisableTracingWorks(self, tensor_debug_mode):
    writer = dumping_callback.enable_dumping(
        self.dump_root, tensor_debug_mode=tensor_debug_mode)
    dumping_callback.disable_dumping()

    x = constant_op.constant([10.0, 12.0, 10.0])
    for _ in range(2):
      array_ops.unique(x)

    writer.FlushNonExecutionFiles()
    writer.FlushExecutionFiles()

    reader = debug_events_reader.DebugEventsReader(self.dump_root)
    source_files_iter = reader.source_files_iterator()
    stack_frames_iter = reader.stack_frames_iterator()
    execution_iter = reader.execution_iterator()
    # No source-file, stack-frame or execution data should have been dumped.
    with self.assertRaises(StopIteration):
      next(source_files_iter)
    with self.assertRaises(StopIteration):
      next(stack_frames_iter)
    with self.assertRaises(StopIteration):
      next(execution_iter)

  @parameterized.named_parameters(
      ("NoTensor", "NO_TENSOR"),
      ("FullTensor", "FULL_TENSOR"),
  )
  def testMultiThreadedExecution(self, tensor_debug_mode):
    writer = dumping_callback.enable_dumping(
        self.dump_root, tensor_debug_mode=tensor_debug_mode)
    x = variables.Variable(10.0, dtype=dtypes.float32)
    y = variables.Variable(3.0, dtype=dtypes.float32)

    @def_function.function
    def increase_x():
      return x.assign_add(y * 2.0)

    increase_x()

    num_threads = 3
    threads = []
    for _ in range(num_threads):
      threads.append(threading.Thread(target=increase_x))
    for thread in threads:
      thread.start()
    for thread in threads:
      thread.join()
    # 10 --> 16 --> 22 --> 28 --> 34.
    self.assertAllClose(x.read_value(), 34.0)

    writer.FlushNonExecutionFiles()
    writer.FlushExecutionFiles()

    stack_frame_by_id = self._readAndCheckSourceFilesAndStackFrames()
    reader = debug_events_reader.DebugEventsReader(self.dump_root)
    execution_iter = reader.execution_iterator()
    prev_wall_time = 1
    for debug_event in execution_iter:
      self.assertGreaterEqual(debug_event.wall_time, prev_wall_time)
      prev_wall_time = debug_event.wall_time

    (context_ids, _,
     op_name_to_op_type) = self._readAndCheckGraphsFile(stack_frame_by_id)

    (op_names, _, output_slots,
     tensor_values) = self._readAndCheckGraphExecutionTracesFile(context_ids)
    executed_op_types = [op_name_to_op_type[op_name] for op_name in op_names]
    self.assertEqual(executed_op_types.count("Mul"), 1 + num_threads)
    self.assertEqual(
        executed_op_types.count("ReadVariableOp"), 2 * (1 + num_threads))
    for output_slot in output_slots:
      self.assertEqual(output_slot, 0)
    if tensor_debug_mode == "NO_TENSOR":
      for tensor_value in tensor_values:
        self.assertEqual(tensor_value.dtype, np.float32)
        self.assertEqual(tensor_value.shape, (0,))
    elif tensor_debug_mode == "FULL_TENSOR":
      mul_values = [
          tensor_values[i]
          for i, op_type in enumerate(executed_op_types)
          if op_type == "Mul"
      ]
      self.assertAllClose(mul_values, [6.0, 6.0, 6.0, 6.0])

  @parameterized.named_parameters(
      ("NoTensor", "NO_TENSOR"),
      ("FullTensor", "FULL_TENSOR"),
  )
  @test_util.run_in_graph_and_eager_modes
  def testSimpleKerasRecurrentModelPredict(self, tensor_debug_mode):
    writer = dumping_callback.enable_dumping(
        self.dump_root, tensor_debug_mode=tensor_debug_mode)
    model = _create_simple_recurrent_keras_model([3, 4])
    batch_size = 5
    xs = np.ones([batch_size, 3, 4])
    self.assertAllClose(model.predict(xs), np.zeros([batch_size, 1]))

    writer.FlushNonExecutionFiles()
    writer.FlushExecutionFiles()

    stack_frame_by_id = self._readAndCheckSourceFilesAndStackFrames()
    (context_ids, op_types,
     op_name_to_op_type) = self._readAndCheckGraphsFile(stack_frame_by_id)
    # Simply assert that graph are recorded and refrain from asserting on the
    # internal details of the Keras model.
    self.assertTrue(context_ids)
    self.assertTrue(op_types)
    self.assertTrue(op_name_to_op_type)

    if context.executing_eagerly():
      # NOTE(b/142486213): Execution of the TF function happens with
      # Session.run() in v1 graph mode, hence it doesn't get logged to the
      # .execution file.
      (executed_op_types, _, _, _,
       tensor_values) = self._readAndCheckExecutionFile()
      self.assertTrue(executed_op_types)

      for value_list in tensor_values:
        if tensor_debug_mode == "NO_TENSOR":
          self.assertFalse(value_list)

    (op_names, _, _,
     tensor_values) = self._readAndCheckGraphExecutionTracesFile(context_ids)
    executed_op_types = [op_name_to_op_type[op_name] for op_name in op_names]
    # These are the ops that we can safely assume to have been executed during
    # the model prediction.
    self.assertIn("MatMul", executed_op_types)
    self.assertIn("BiasAdd", executed_op_types)
    # On the GPU, CudnnRNN is used in lieu of the default op-by-op
    # implementation.
    self.assertTrue(
        ("Sigmoid" in executed_op_types and "Tanh" in executed_op_types or
         "CudnnRNN" in executed_op_types))
    # Under the default NO_TENSOR tensor-debug mode, the tensor_proto ought to
    # be an empty float32 tensor.
    if tensor_debug_mode == "NO_TENSOR":
      for tensor_value in tensor_values:
        self.assertEqual(tensor_value.dtype, np.float32)
        self.assertEqual(tensor_value.shape, (0,))
    else:
      # Refrain from asserting the internal implementation details of the LSTM
      # layer.
      concrete_tensor_values = [
          value for value in tensor_values
          if value is not None and value.size > 0
      ]
      self.assertTrue(concrete_tensor_values)

  @parameterized.named_parameters(
      ("NoTensor", "NO_TENSOR"),
      ("FullTensor", "FULL_TENSOR"),
  )
  @test_util.run_in_graph_and_eager_modes
  def testSimpleKerasRecurrentModelFit(self, tensor_debug_mode):
    writer = dumping_callback.enable_dumping(
        self.dump_root, tensor_debug_mode=tensor_debug_mode)
    model = _create_simple_recurrent_keras_model([3, 4])
    xs = np.ones([5, 3, 4])
    ys = np.ones([5, 1])

    history = model.fit(xs, ys, epochs=3, verbose=0)
    self.assertAllClose(
        history.history["loss"], [1.0, 0.9603999853134155, 0.9223681688308716])

    writer.FlushNonExecutionFiles()
    writer.FlushExecutionFiles()

    stack_frame_by_id = self._readAndCheckSourceFilesAndStackFrames()
    (context_ids, op_types,
     op_name_to_op_type) = self._readAndCheckGraphsFile(stack_frame_by_id)
    # Simply assert that graph are recorded and refrain from asserting on the
    # internal details of the Keras model.
    self.assertTrue(context_ids)
    self.assertTrue(op_types)
    self.assertTrue(op_name_to_op_type)

    if context.executing_eagerly():
      # NOTE(b/142486213): Execution of the TF function happens with
      # Session.run() in v1 graph mode, hence it doesn't get logged to the
      # .execution file.
      (executed_op_types, _, _, _,
       tensor_values) = self._readAndCheckExecutionFile()
      self.assertTrue(executed_op_types)
      if tensor_debug_mode == "NO_TENSOR":
        for value_list in tensor_values:
          self.assertFalse(value_list)

    (op_names, _, _,
     tensor_values) = self._readAndCheckGraphExecutionTracesFile(context_ids)
    executed_op_types = [op_name_to_op_type[op_name] for op_name in op_names]
    # These are the ops that we can safely assume to have been executed during
    # the recurrent model's fit() call.
    self.assertIn("MatMul", executed_op_types)
    self.assertIn("BiasAdd", executed_op_types)
    # On the GPU, CudnnRNN is used in lieu of the default op-by-op
    # implementation.
    self.assertTrue(
        ("Sigmoid" in executed_op_types and "Tanh" in executed_op_types or
         "CudnnRNN" in executed_op_types))
    self.assertTrue(
        ("SigmoidGrad" in executed_op_types and
         "TanhGrad" in executed_op_types or
         "CudnnRNNBackprop" in executed_op_types))
    if tensor_debug_mode == "NO_TENSOR":
      # Under the default NO_TENSOR tensor-debug mode, the tensor_proto ought
      # to be an empty float32 tensor.
      for tensor_value in tensor_values:
        self.assertEqual(tensor_value.dtype, np.float32)
        self.assertEqual(tensor_value.shape, (0,))

  @parameterized.named_parameters(
      ("NoTensor", "NO_TENSOR"),
      ("FullTensor", "FULL_TENSOR"),
  )
  @test_util.run_in_graph_and_eager_modes
  def testMobiletNetV2Fit(self, tensor_debug_mode):
    """Test training Keras MobileNetV2 works with dumping."""
    # Use a large circular-buffer to make sure we capture all the executed ops.
    writer = dumping_callback.enable_dumping(
        self.dump_root,
        tensor_debug_mode=tensor_debug_mode,
        circular_buffer_size=100000)
    model = mobilenet_v2.MobileNetV2(
        input_shape=(32, 32, 3), alpha=0.1, weights=None)
    y = model.layers[22].output
    y = core.Flatten()(y)
    y = core.Dense(1)(y)
    model = models.Model(inputs=model.inputs, outputs=y)

    batch_size = 2
    xs = np.zeros([batch_size] + list(model.input_shape[1:]))
    ys = np.zeros([batch_size] + list(model.output_shape[1:]))
    model.compile(optimizer="sgd", loss="mse")
    epochs = 1
    history = model.fit(xs, ys, epochs=epochs, verbose=0)
    self.assertLen(history.history["loss"], epochs)

    writer.FlushNonExecutionFiles()
    writer.FlushExecutionFiles()

    stack_frame_by_id = self._readAndCheckSourceFilesAndStackFrames()
    (context_ids, op_types,
     op_name_to_op_type) = self._readAndCheckGraphsFile(stack_frame_by_id)
    # Simply assert that graph are recorded and refrain from asserting on the
    # internal details of the Keras model.
    self.assertTrue(context_ids)
    self.assertTrue(op_types)
    self.assertTrue(op_name_to_op_type)

    if context.executing_eagerly():
      # NOTE(b/142486213): Execution of the TF function happens with
      # Session.run() in v1 graph mode, hence it doesn't get logged to the
      # .execution file.
      executed_op_types, _, _, _, _ = self._readAndCheckExecutionFile()
      self.assertTrue(executed_op_types)

    (op_names, _, _,
     tensor_values) = self._readAndCheckGraphExecutionTracesFile(context_ids)
    executed_op_types = [op_name_to_op_type[op_name] for op_name in op_names]
    # These are the ops that we can safely assume to have been executed during
    # the model's fit() call.
    self.assertIn("Conv2D", executed_op_types)
    self.assertIn("Relu6", executed_op_types)
    self.assertIn("Conv2DBackpropFilter", executed_op_types)
    self.assertIn("Relu6Grad", executed_op_types)
    if tensor_debug_mode == "NO_TENSOR":
      # Under the default NO_TENSOR tensor-debug mode, the tensor_proto ought to
      # be an empty float32 tensor.
      for tensor_value in tensor_values:
        self.assertEqual(tensor_value.dtype, np.float32)
        self.assertEqual(tensor_value.shape, (0,))
    elif tensor_debug_mode == "FULL_TENSOR":
      conv2d_values = [
          tensor_values[i]
          for i, op_type in enumerate(executed_op_types)
          if op_type == "Conv2D"
      ]
      self.assertTrue(conv2d_values)
      for conv2d_value in conv2d_values:
        self.assertGreater(len(conv2d_value.shape), 1)
        self.assertEqual(conv2d_value.shape[0], batch_size)
      relu6_values = [
          tensor_values[i]
          for i, op_type in enumerate(executed_op_types)
          if op_type == "Relu6"
      ]
      self.assertTrue(relu6_values)
      for relu6_value in relu6_values:
        self.assertGreater(len(relu6_value.shape), 1)
        self.assertEqual(relu6_value.shape[0], batch_size)
      conv2d_bp_filter_values = [
          tensor_values[i]
          for i, op_type in enumerate(executed_op_types)
          if op_type == "Conv2DBackpropFilter"
      ]
      self.assertTrue(conv2d_bp_filter_values)
      for conv2d_bp_filter_value in conv2d_bp_filter_values:
        self.assertGreater(len(conv2d_bp_filter_value.shape), 1)
      relu6_grad_values = [
          tensor_values[i]
          for i, op_type in enumerate(executed_op_types)
          if op_type == "Relu6Grad"
      ]
      self.assertTrue(relu6_grad_values)
      for relu6_grad_value in relu6_grad_values:
        self.assertGreater(len(relu6_grad_value.shape), 1)


if __name__ == "__main__":
  ops.enable_eager_execution()
  googletest.main()
