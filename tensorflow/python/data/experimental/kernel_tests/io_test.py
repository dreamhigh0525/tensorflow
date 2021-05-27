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
"""Tests for the `tf.data.experimental.{save,load}` operations."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import os
import shutil

from absl.testing import parameterized
import numpy as np

from tensorflow.python.data.experimental.ops import io
from tensorflow.python.data.kernel_tests import test_base
from tensorflow.python.data.ops import dataset_ops
from tensorflow.python.eager import def_function
from tensorflow.python.framework import combinations
from tensorflow.python.framework import errors
from tensorflow.python.platform import test
from tensorflow.python.training.tracking import util as trackable_utils


class IOTest(test_base.DatasetTestBase, parameterized.TestCase):

  def setUp(self):
    super(IOTest, self).setUp()
    tmpdir = self.get_temp_dir()
    tmpdir = os.path.join(tmpdir, "io_test")
    os.mkdir(tmpdir)
    self._test_dir = tmpdir

    self._checkpoint_prefix = os.path.join(self.get_temp_dir(), "ckpt")
    os.mkdir(self._checkpoint_prefix)
    self._save_dir = os.path.join(self.get_temp_dir(), "save")
    os.mkdir(self._save_dir)

  def tearDown(self):
    super(IOTest, self).tearDown()
    shutil.rmtree(self._test_dir)
    shutil.rmtree(self._checkpoint_prefix)
    shutil.rmtree(self._save_dir)

  @combinations.generate(
      combinations.times(test_base.eager_only_combinations(),
                         combinations.combine(compression=[None, "GZIP"])))
  def testBasic(self, compression):
    dataset = dataset_ops.Dataset.range(42)
    io.save(dataset, self._test_dir, compression=compression)
    dataset2 = io.load(
        self._test_dir, dataset.element_spec, compression=compression)
    self.assertDatasetProduces(dataset2, range(42))

  @combinations.generate(test_base.eager_only_combinations())
  def testCardinality(self):
    dataset = dataset_ops.Dataset.range(42)
    io.save(dataset, self._test_dir)
    dataset2 = io.load(self._test_dir, dataset.element_spec)
    self.assertEqual(self.evaluate(dataset2.cardinality()), 42)

  @combinations.generate(test_base.eager_only_combinations())
  def testCustomShardFunction(self):
    dataset = dataset_ops.Dataset.range(42)
    io.save(dataset, self._test_dir, shard_func=lambda x: x // 21)
    dataset2 = io.load(self._test_dir, dataset.element_spec)
    expected = []
    for i in range(21):
      expected.extend([i, i + 21])
    self.assertDatasetProduces(dataset2, expected)

  @combinations.generate(test_base.eager_only_combinations())
  def testCustomReaderFunction(self):
    dataset = dataset_ops.Dataset.range(42)
    io.save(dataset, self._test_dir, shard_func=lambda x: x % 7)
    dataset2 = io.load(
        self._test_dir,
        dataset.element_spec,
        reader_func=lambda x: x.flat_map(lambda y: y))
    expected = []
    for i in range(7):
      expected.extend(range(i, 42, 7))
    self.assertDatasetProduces(dataset2, expected)

  @combinations.generate(
      combinations.times(test_base.eager_only_combinations(),
                         combinations.combine(compression=[None, "GZIP"])))
  def testSaveInsideFunction(self, compression):

    dataset = dataset_ops.Dataset.range(42)

    @def_function.function
    def save_fn():
      io.save(dataset, self._test_dir, compression=compression)

    save_fn()
    dataset = io.load(
        self._test_dir, dataset.element_spec, compression=compression)
    self.assertDatasetProduces(dataset, range(42))

  @combinations.generate(test_base.eager_only_combinations())
  def testOptionalElementSpec(self):
    range_dataset = dataset_ops.Dataset.range(42)
    dict_dataset = dataset_ops.Dataset.from_tensor_slices({"a": [1, 2],
                                                           "b": [3, 4]})
    tuple_dataset = dataset_ops.Dataset.from_tensor_slices(([1, 2], [3, 4]))
    dataset = dataset_ops.Dataset.zip((range_dataset, dict_dataset,
                                       tuple_dataset))
    io.save(dataset, self._test_dir)
    dataset_loaded = io.load(self._test_dir)
    self.assertDatasetsEqual(dataset, dataset_loaded)

  @combinations.generate(test_base.eager_only_combinations())
  def testRepeatAndPrefetch(self):
    """This test reproduces github.com/tensorflow/tensorflow/issues/49165."""
    dataset1 = dataset_ops.Dataset.from_tensor_slices(np.random.rand(16, 32))
    io.save(dataset1, self._test_dir)
    dataset = io.load(self._test_dir)
    dataset = dataset.shuffle(buffer_size=16)
    dataset = dataset.batch(16)
    dataset = dataset.repeat()
    dataset = dataset.prefetch(1)
    next_element = self.getNext(dataset)
    for _ in range(30):
      self.evaluate(next_element())

  @combinations.generate(test_base.eager_only_combinations())
  def testLoadCheckpointUnusedIterator(self):
    dataset = dataset_ops.Dataset.range(3)
    io.save(dataset, self._save_dir)
    loaded_dataset = io.load(self._save_dir)
    iterator = iter(loaded_dataset)
    get_next = iterator.get_next

    checkpoint = trackable_utils.Checkpoint(iterator=iterator)
    save_path = checkpoint.save(self._checkpoint_prefix)
    self.assertAllEqual(0, get_next())
    self.assertAllEqual(1, get_next())
    checkpoint.restore(save_path).run_restore_ops()
    self.assertAllEqual(0, get_next())
    self.assertAllEqual(1, get_next())
    self.assertAllEqual(2, get_next())
    with self.assertRaises(errors.OutOfRangeError):
      get_next()

  @combinations.generate(test_base.eager_only_combinations())
  def testLoadCheckpointFullyUsedIterator(self):
    dataset = dataset_ops.Dataset.range(3)
    io.save(dataset, self._save_dir)
    loaded_dataset = io.load(self._save_dir)
    iterator = iter(loaded_dataset)
    get_next = iterator.get_next

    checkpoint = trackable_utils.Checkpoint(iterator=iterator)
    self.assertAllEqual(0, get_next())
    self.assertAllEqual(1, get_next())
    self.assertAllEqual(2, get_next())
    save_path = checkpoint.save(self._checkpoint_prefix)
    checkpoint.restore(save_path).run_restore_ops()
    with self.assertRaises(errors.OutOfRangeError):
      get_next()

  @combinations.generate(test_base.eager_only_combinations())
  def testLoadCheckpointExhaustedIterator(self):
    dataset = dataset_ops.Dataset.range(3)
    io.save(dataset, self._save_dir)
    loaded_dataset = io.load(self._save_dir)
    iterator = iter(loaded_dataset)
    get_next = iterator.get_next

    checkpoint = trackable_utils.Checkpoint(iterator=iterator)
    self.assertAllEqual(0, get_next())
    self.assertAllEqual(1, get_next())
    self.assertAllEqual(2, get_next())
    with self.assertRaises(errors.OutOfRangeError):
      get_next()
    save_path = checkpoint.save(self._checkpoint_prefix)
    checkpoint.restore(save_path).run_restore_ops()
    with self.assertRaises(errors.OutOfRangeError):
      get_next()

  @combinations.generate(test_base.eager_only_combinations())
  def testLoadCheckpointIteratorMultipleBreaks(self):
    dataset = dataset_ops.Dataset.range(3)
    io.save(dataset, self._save_dir)
    loaded_dataset = io.load(self._save_dir)
    iterator = iter(loaded_dataset)
    get_next = iterator.get_next

    checkpoint = trackable_utils.Checkpoint(iterator=iterator)
    for i in range(len(dataset)):
      save_path = checkpoint.save(self._checkpoint_prefix)
      self.assertAllEqual(i, get_next())
      checkpoint.restore(save_path).run_restore_ops()
      self.assertAllEqual(i, get_next())
    with self.assertRaises(errors.OutOfRangeError):
      get_next()


if __name__ == "__main__":
  test.main()
