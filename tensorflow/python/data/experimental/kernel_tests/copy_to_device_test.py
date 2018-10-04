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
"""Tests for `tf.data.experimental.copy_to_device()`."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.core.protobuf import config_pb2
from tensorflow.python.compat import compat
from tensorflow.python.data.experimental.ops import prefetching_ops
from tensorflow.python.data.kernel_tests import test_base
from tensorflow.python.data.ops import dataset_ops
from tensorflow.python.data.ops import iterator_ops
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import errors
from tensorflow.python.framework import ops
from tensorflow.python.framework import sparse_tensor
from tensorflow.python.framework import test_util
from tensorflow.python.platform import test


class CopyToDeviceTest(test_base.DatasetTestBase):

  def testCopyToDevice(self):
    host_dataset = dataset_ops.Dataset.range(10)
    device_dataset = host_dataset.apply(
        prefetching_ops.copy_to_device("/cpu:1"))

    with ops.device("/cpu:1"):
      iterator = device_dataset.make_one_shot_iterator()
      next_element = iterator.get_next()

    self.assertEqual(host_dataset.output_types, device_dataset.output_types)
    self.assertEqual(host_dataset.output_types, iterator.output_types)
    self.assertEqual(host_dataset.output_shapes, device_dataset.output_shapes)
    self.assertEqual(host_dataset.output_shapes, iterator.output_shapes)
    self.assertEqual(host_dataset.output_classes, device_dataset.output_classes)
    self.assertEqual(host_dataset.output_classes, iterator.output_classes)

    self.assertEqual(dtypes.int64, next_element.dtype)
    self.assertEqual([], next_element.shape)

    worker_config = config_pb2.ConfigProto(device_count={"CPU": 2})
    with self.test_session(config=worker_config) as sess:
      for i in range(10):
        self.assertEqual(i, sess.run(next_element))
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element)

  def testCopyToDeviceInt32(self):
    host_dataset = dataset_ops.Dataset.from_tensors([0, 1, 2, 3])
    device_dataset = host_dataset.apply(
        prefetching_ops.copy_to_device("/cpu:1"))

    with ops.device("/cpu:1"):
      iterator = device_dataset.make_one_shot_iterator()
      next_element = iterator.get_next()

    self.assertEqual(host_dataset.output_types, device_dataset.output_types)
    self.assertEqual(host_dataset.output_types, iterator.output_types)
    self.assertEqual(host_dataset.output_shapes, device_dataset.output_shapes)
    self.assertEqual(host_dataset.output_shapes, iterator.output_shapes)
    self.assertEqual(host_dataset.output_classes, device_dataset.output_classes)
    self.assertEqual(host_dataset.output_classes, iterator.output_classes)

    self.assertEqual(dtypes.int32, next_element.dtype)
    self.assertEqual((4,), next_element.shape)

    worker_config = config_pb2.ConfigProto(device_count={"CPU": 2})
    with self.test_session(config=worker_config) as sess:
      self.assertAllEqual([0, 1, 2, 3], sess.run(next_element))
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element)

  def testCopyToSameDevice(self):
    host_dataset = dataset_ops.Dataset.range(10)
    device_dataset = host_dataset.apply(
        prefetching_ops.copy_to_device("/cpu:0"))

    with ops.device("/cpu:0"):
      iterator = device_dataset.make_one_shot_iterator()
      next_element = iterator.get_next()

    self.assertEqual(host_dataset.output_types, device_dataset.output_types)
    self.assertEqual(host_dataset.output_types, iterator.output_types)
    self.assertEqual(host_dataset.output_shapes, device_dataset.output_shapes)
    self.assertEqual(host_dataset.output_shapes, iterator.output_shapes)
    self.assertEqual(host_dataset.output_classes, device_dataset.output_classes)
    self.assertEqual(host_dataset.output_classes, iterator.output_classes)

    self.assertEqual(dtypes.int64, next_element.dtype)
    self.assertEqual([], next_element.shape)

    worker_config = config_pb2.ConfigProto(device_count={"CPU": 2})
    with self.test_session(config=worker_config) as sess:
      for i in range(10):
        self.assertEqual(i, sess.run(next_element))
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element)

  def testCopyToDeviceWithPrefetch(self):
    host_dataset = dataset_ops.Dataset.range(10)
    device_dataset = host_dataset.apply(
        prefetching_ops.copy_to_device("/cpu:1")).prefetch(1)

    with ops.device("/cpu:1"):
      iterator = device_dataset.make_one_shot_iterator()
      next_element = iterator.get_next()

    self.assertEqual(host_dataset.output_types, device_dataset.output_types)
    self.assertEqual(host_dataset.output_types, iterator.output_types)
    self.assertEqual(host_dataset.output_shapes, device_dataset.output_shapes)
    self.assertEqual(host_dataset.output_shapes, iterator.output_shapes)
    self.assertEqual(host_dataset.output_classes, device_dataset.output_classes)
    self.assertEqual(host_dataset.output_classes, iterator.output_classes)

    self.assertEqual(dtypes.int64, next_element.dtype)
    self.assertEqual([], next_element.shape)

    worker_config = config_pb2.ConfigProto(device_count={"CPU": 2})
    with self.test_session(config=worker_config) as sess:
      for i in range(10):
        self.assertEqual(i, sess.run(next_element))
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element)

  def testCopyDictToDevice(self):
    host_dataset = dataset_ops.Dataset.range(10).map(lambda x: {"a": x})
    device_dataset = host_dataset.apply(
        prefetching_ops.copy_to_device("/cpu:1"))

    with ops.device("/cpu:1"):
      iterator = device_dataset.make_one_shot_iterator()
      next_element = iterator.get_next()

    self.assertEqual(host_dataset.output_types, device_dataset.output_types)
    self.assertEqual(host_dataset.output_types, iterator.output_types)
    self.assertEqual(host_dataset.output_shapes, device_dataset.output_shapes)
    self.assertEqual(host_dataset.output_shapes, iterator.output_shapes)
    self.assertEqual(host_dataset.output_classes, device_dataset.output_classes)
    self.assertEqual(host_dataset.output_classes, iterator.output_classes)

    self.assertEqual(dtypes.int64, next_element["a"].dtype)
    self.assertEqual([], next_element["a"].shape)

    worker_config = config_pb2.ConfigProto(device_count={"CPU": 2})
    with self.test_session(config=worker_config) as sess:
      for i in range(10):
        self.assertEqual({"a": i}, sess.run(next_element))
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element)

  def testCopyDictToDeviceWithPrefetch(self):
    host_dataset = dataset_ops.Dataset.range(10).map(lambda x: {"a": x})
    device_dataset = host_dataset.apply(
        prefetching_ops.copy_to_device("/cpu:1")).prefetch(1)

    with ops.device("/cpu:1"):
      iterator = device_dataset.make_one_shot_iterator()
      next_element = iterator.get_next()

    self.assertEqual(host_dataset.output_types, device_dataset.output_types)
    self.assertEqual(host_dataset.output_types, iterator.output_types)
    self.assertEqual(host_dataset.output_shapes, device_dataset.output_shapes)
    self.assertEqual(host_dataset.output_shapes, iterator.output_shapes)
    self.assertEqual(host_dataset.output_classes, device_dataset.output_classes)
    self.assertEqual(host_dataset.output_classes, iterator.output_classes)

    self.assertEqual(dtypes.int64, next_element["a"].dtype)
    self.assertEqual([], next_element["a"].shape)

    worker_config = config_pb2.ConfigProto(device_count={"CPU": 2})
    with self.test_session(config=worker_config) as sess:
      for i in range(10):
        self.assertEqual({"a": i}, sess.run(next_element))
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element)

  def testCopySparseTensorsToDevice(self):

    def make_tensor(i):
      return sparse_tensor.SparseTensorValue(
          indices=[[0, 0]], values=(i * [1]), dense_shape=[2, 2])

    host_dataset = dataset_ops.Dataset.range(10).map(make_tensor)

    device_dataset = host_dataset.apply(
        prefetching_ops.copy_to_device("/cpu:1"))

    with ops.device("/cpu:1"):
      iterator = device_dataset.make_one_shot_iterator()
      next_element = iterator.get_next()

    self.assertEqual(host_dataset.output_types, device_dataset.output_types)
    self.assertEqual(host_dataset.output_types, iterator.output_types)
    self.assertEqual(host_dataset.output_shapes, device_dataset.output_shapes)
    self.assertEqual(host_dataset.output_shapes, iterator.output_shapes)
    self.assertEqual(host_dataset.output_classes, device_dataset.output_classes)
    self.assertEqual(host_dataset.output_classes, iterator.output_classes)

    self.assertEqual(dtypes.int64, next_element.dtype)

    worker_config = config_pb2.ConfigProto(device_count={"CPU": 2})
    with self.test_session(config=worker_config) as sess:
      for i in range(10):
        actual = sess.run(next_element)
        self.assertAllEqual([i], actual.values)
        self.assertAllEqual([[0, 0]], actual.indices)
        self.assertAllEqual([2, 2], actual.dense_shape)
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element)

  def testCopySparseTensorsToDeviceWithPrefetch(self):

    def make_tensor(i):
      return sparse_tensor.SparseTensorValue(
          indices=[[0, 0]], values=(i * [1]), dense_shape=[2, 2])

    host_dataset = dataset_ops.Dataset.range(10).map(make_tensor)

    device_dataset = host_dataset.apply(
        prefetching_ops.copy_to_device("/cpu:1")).prefetch(1)

    with ops.device("/cpu:1"):
      iterator = device_dataset.make_one_shot_iterator()
      next_element = iterator.get_next()

    self.assertEqual(host_dataset.output_types, device_dataset.output_types)
    self.assertEqual(host_dataset.output_types, iterator.output_types)
    self.assertEqual(host_dataset.output_shapes, device_dataset.output_shapes)
    self.assertEqual(host_dataset.output_shapes, iterator.output_shapes)
    self.assertEqual(host_dataset.output_classes, device_dataset.output_classes)
    self.assertEqual(host_dataset.output_classes, iterator.output_classes)

    self.assertEqual(dtypes.int64, next_element.dtype)

    worker_config = config_pb2.ConfigProto(device_count={"CPU": 2})
    with self.test_session(config=worker_config) as sess:
      for i in range(10):
        actual = sess.run(next_element)
        self.assertAllEqual([i], actual.values)
        self.assertAllEqual([[0, 0]], actual.indices)
        self.assertAllEqual([2, 2], actual.dense_shape)
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element)

  def testCopyToDeviceGpu(self):
    if not test_util.is_gpu_available():
      self.skipTest("No GPU available")

    host_dataset = dataset_ops.Dataset.range(10)
    device_dataset = host_dataset.apply(
        prefetching_ops.copy_to_device("/gpu:0"))

    with ops.device("/gpu:0"):
      iterator = device_dataset.make_initializable_iterator()
      next_element = iterator.get_next()

    with self.cached_session() as sess:
      sess.run(iterator.initializer)
      for i in range(10):
        self.assertEqual(i, sess.run(next_element))
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element)

  def testCopyToDeviceGpuWithPrefetch(self):
    if not test_util.is_gpu_available():
      self.skipTest("No GPU available")

    host_dataset = dataset_ops.Dataset.range(10)
    device_dataset = host_dataset.apply(
        prefetching_ops.copy_to_device("/gpu:0")).prefetch(1)

    with ops.device("/gpu:0"):
      iterator = device_dataset.make_initializable_iterator()
      next_element = iterator.get_next()

    with self.cached_session() as sess:
      sess.run(iterator.initializer)
      for i in range(10):
        self.assertEqual(i, sess.run(next_element))
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element)

  def testCopyToDeviceGpuInt32(self):
    if not test_util.is_gpu_available():
      self.skipTest("No GPU available")

    host_dataset = dataset_ops.Dataset.from_tensors([0, 1, 2, 3])
    device_dataset = host_dataset.apply(
        prefetching_ops.copy_to_device("/gpu:0"))

    with ops.device("/gpu:0"):
      iterator = device_dataset.make_initializable_iterator()
      next_element = iterator.get_next()

    with self.cached_session() as sess:
      sess.run(iterator.initializer)
      self.assertAllEqual([0, 1, 2, 3], sess.run(next_element))
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element)

  def testCopyToDeviceGpuInt32AndPrefetch(self):
    if not test_util.is_gpu_available():
      self.skipTest("No GPU available")

    host_dataset = dataset_ops.Dataset.from_tensors([0, 1, 2, 3])
    device_dataset = host_dataset.apply(
        prefetching_ops.copy_to_device("/gpu:0")).prefetch(1)

    with ops.device("/gpu:0"):
      iterator = device_dataset.make_initializable_iterator()
      next_element = iterator.get_next()

    with self.cached_session() as sess:
      sess.run(iterator.initializer)
      self.assertAllEqual([0, 1, 2, 3], sess.run(next_element))
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element)

  def testCopyToDeviceGpuStrings(self):
    if not test_util.is_gpu_available():
      self.skipTest("No GPU available")

    host_dataset = dataset_ops.Dataset.from_tensors(["a", "b", "c"])
    device_dataset = host_dataset.apply(
        prefetching_ops.copy_to_device("/gpu:0"))

    with ops.device("/gpu:0"):
      iterator = device_dataset.make_initializable_iterator()
      next_element = iterator.get_next()

    with self.cached_session() as sess:
      sess.run(iterator.initializer)
      self.assertAllEqual([b"a", b"b", b"c"], sess.run(next_element))
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element)

  def testCopyToDeviceGpuStringsAndPrefetch(self):
    if not test_util.is_gpu_available():
      self.skipTest("No GPU available")

    host_dataset = dataset_ops.Dataset.from_tensors(["a", "b", "c"])
    device_dataset = host_dataset.apply(
        prefetching_ops.copy_to_device("/gpu:0"))

    with ops.device("/gpu:0"):
      iterator = device_dataset.make_initializable_iterator()
      next_element = iterator.get_next()

    with self.cached_session() as sess:
      sess.run(iterator.initializer)
      self.assertAllEqual([b"a", b"b", b"c"], sess.run(next_element))
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element)

  def testCopyToDevicePingPongCPUGPU(self):
    if not test_util.is_gpu_available():
      self.skipTest("No GPU available")

    with compat.forward_compatibility_horizon(2018, 8, 4):
      host_dataset = dataset_ops.Dataset.range(10)
      device_dataset = host_dataset.apply(
          prefetching_ops.copy_to_device("/gpu:0", source_device="/cpu:0"))
      back_to_cpu_dataset = device_dataset.apply(
          prefetching_ops.copy_to_device("/cpu:0", source_device="/gpu:0"))

      with ops.device("/cpu:0"):
        iterator = back_to_cpu_dataset.make_initializable_iterator()
        next_element = iterator.get_next()

      with self.cached_session() as sess:
        sess.run(iterator.initializer)
        for i in range(10):
          self.assertEqual(i, sess.run(next_element))
        with self.assertRaises(errors.OutOfRangeError):
          sess.run(next_element)

  def testCopyToDeviceWithReInit(self):
    host_dataset = dataset_ops.Dataset.range(10)
    device_dataset = host_dataset.apply(
        prefetching_ops.copy_to_device("/cpu:1"))

    with ops.device("/cpu:1"):
      iterator = device_dataset.make_initializable_iterator()
      next_element = iterator.get_next()

    self.assertEqual(host_dataset.output_types, device_dataset.output_types)
    self.assertEqual(host_dataset.output_types, iterator.output_types)
    self.assertEqual(host_dataset.output_shapes, device_dataset.output_shapes)
    self.assertEqual(host_dataset.output_shapes, iterator.output_shapes)
    self.assertEqual(host_dataset.output_classes, device_dataset.output_classes)
    self.assertEqual(host_dataset.output_classes, iterator.output_classes)

    self.assertEqual(dtypes.int64, next_element.dtype)
    self.assertEqual([], next_element.shape)

    worker_config = config_pb2.ConfigProto(device_count={"CPU": 2})
    with self.test_session(config=worker_config) as sess:
      sess.run(iterator.initializer)
      for i in range(5):
        self.assertEqual(i, sess.run(next_element))
      sess.run(iterator.initializer)
      for i in range(10):
        self.assertEqual(i, sess.run(next_element))
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element)

  def testCopyToDeviceWithReInitAndPrefetch(self):
    host_dataset = dataset_ops.Dataset.range(10)
    device_dataset = host_dataset.apply(
        prefetching_ops.copy_to_device("/cpu:1")).prefetch(1)

    with ops.device("/cpu:1"):
      iterator = device_dataset.make_initializable_iterator()
      next_element = iterator.get_next()

    self.assertEqual(host_dataset.output_types, device_dataset.output_types)
    self.assertEqual(host_dataset.output_types, iterator.output_types)
    self.assertEqual(host_dataset.output_shapes, device_dataset.output_shapes)
    self.assertEqual(host_dataset.output_shapes, iterator.output_shapes)
    self.assertEqual(host_dataset.output_classes, device_dataset.output_classes)
    self.assertEqual(host_dataset.output_classes, iterator.output_classes)

    self.assertEqual(dtypes.int64, next_element.dtype)
    self.assertEqual([], next_element.shape)

    worker_config = config_pb2.ConfigProto(device_count={"CPU": 2})
    with self.test_session(config=worker_config) as sess:
      sess.run(iterator.initializer)
      for i in range(5):
        self.assertEqual(i, sess.run(next_element))
      sess.run(iterator.initializer)
      for i in range(10):
        self.assertEqual(i, sess.run(next_element))
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element)

  def testCopyToDeviceGpuWithReInit(self):
    if not test_util.is_gpu_available():
      self.skipTest("No GPU available")

    host_dataset = dataset_ops.Dataset.range(10)
    device_dataset = host_dataset.apply(
        prefetching_ops.copy_to_device("/gpu:0"))

    with ops.device("/gpu:0"):
      iterator = device_dataset.make_initializable_iterator()
      next_element = iterator.get_next()

    with self.cached_session() as sess:
      sess.run(iterator.initializer)
      for i in range(5):
        self.assertEqual(i, sess.run(next_element))
      sess.run(iterator.initializer)
      for i in range(10):
        self.assertEqual(i, sess.run(next_element))
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element)

  def testCopyToDeviceGpuWithReInitAndPrefetch(self):
    if not test_util.is_gpu_available():
      self.skipTest("No GPU available")

    host_dataset = dataset_ops.Dataset.range(10)
    device_dataset = host_dataset.apply(
        prefetching_ops.copy_to_device("/gpu:0")).prefetch(1)

    with ops.device("/gpu:0"):
      iterator = device_dataset.make_initializable_iterator()
      next_element = iterator.get_next()

    with self.cached_session() as sess:
      sess.run(iterator.initializer)
      for i in range(5):
        self.assertEqual(i, sess.run(next_element))
      sess.run(iterator.initializer)
      for i in range(10):
        self.assertEqual(i, sess.run(next_element))
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element)

  def testIteratorGetNextAsOptionalOnGPU(self):
    if not test_util.is_gpu_available():
      self.skipTest("No GPU available")

    host_dataset = dataset_ops.Dataset.range(3)
    device_dataset = host_dataset.apply(
        prefetching_ops.copy_to_device("/gpu:0"))
    with ops.device("/gpu:0"):
      iterator = device_dataset.make_initializable_iterator()
      next_elem = iterator_ops.get_next_as_optional(iterator)
      elem_has_value_t = next_elem.has_value()
      elem_value_t = next_elem.get_value()

    with self.cached_session() as sess:
      # Before initializing the iterator, evaluating the optional fails with
      # a FailedPreconditionError.
      with self.assertRaises(errors.FailedPreconditionError):
        sess.run(elem_has_value_t)
      with self.assertRaises(errors.FailedPreconditionError):
        sess.run(elem_value_t)

      # For each element of the dataset, assert that the optional evaluates to
      # the expected value.
      sess.run(iterator.initializer)
      for i in range(3):
        elem_has_value, elem_value = sess.run([elem_has_value_t, elem_value_t])
        self.assertTrue(elem_has_value)
        self.assertEqual(i, elem_value)

      # After exhausting the iterator, `next_elem.has_value()` will evaluate to
      # false, and attempting to get the value will fail.
      for _ in range(2):
        self.assertFalse(sess.run(elem_has_value_t))
        with self.assertRaises(errors.InvalidArgumentError):
          sess.run(elem_value_t)


if __name__ == "__main__":
  test.main()
