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
"""Tests for the GroupByReducer serialization."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np

from tensorflow.contrib.data.python.kernel_tests.serialization import dataset_serialization_test_base
from tensorflow.contrib.data.python.ops import grouping
from tensorflow.python.data.ops import dataset_ops
from tensorflow.python.platform import test


class GroupByReducerSerializationTest(
    dataset_serialization_test_base.DatasetSerializationTestBase):

  def _build_dataset(self, components):
    reducer = grouping.Reducer(
        init_func=lambda _: np.int64(0),
        reduce_func=lambda x, y: x + y,
        finalize_func=lambda x: x)

    return dataset_ops.Dataset.from_tensor_slices(components).apply(
        grouping.group_by_reducer(lambda x: x % 5, reducer))

  def testCoreGroupByReducer(self):
    components = np.array([0, 1, 2, 3, 4, 5, 6, 7, 8, 9], dtype=np.int64)
    self.verify_unused_iterator(
        lambda: self._build_dataset(components), 5, verify_exhausted=True)
    self.verify_init_before_restore(
        lambda: self._build_dataset(components), 5, verify_exhausted=True)
    self.verify_multiple_breaks(
        lambda: self._build_dataset(components), 5, verify_exhausted=True)
    self.verify_reset_restored_iterator(
        lambda: self._build_dataset(components), 5, verify_exhausted=True)
    self.verify_restore_in_empty_graph(
        lambda: self._build_dataset(components), 5, verify_exhausted=True)
    diff_components = np.array([5, 4, 3, 2, 1, 0], dtype=np.int64)
    self.verify_restore_in_modified_graph(
        lambda: self._build_dataset(components),
        lambda: self._build_dataset(diff_components),
        5,
        verify_exhausted=True)


if __name__ == '__main__':
  test.main()
