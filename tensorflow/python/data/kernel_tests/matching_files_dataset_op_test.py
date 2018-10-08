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
"""Tests for the experimental input pipeline ops."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import os
import shutil
import tempfile
import time

import numpy as np

from tensorflow.python.client import session
from tensorflow.python.data.kernel_tests import test_base
from tensorflow.python.data.ops.dataset_ops import MatchingFilesDataset
from tensorflow.python.framework import errors
from tensorflow.python.framework import ops
from tensorflow.python.platform import test
from tensorflow.python.util import compat


class MatchingFilesDatasetTest(test_base.DatasetTestBase):

  def setUp(self):
    self.tmp_dir = tempfile.mkdtemp()

  def tearDown(self):
    shutil.rmtree(self.tmp_dir, ignore_errors=True)

  def _touchTempFiles(self, filenames):
    for filename in filenames:
      open(os.path.join(self.tmp_dir, filename), 'a').close()

  def testEmptyDirectory(self):
    """Test the MatchingFiles dataset with an empty directory"""

    dataset = MatchingFilesDataset(os.path.join(self.tmp_dir, '*'))
    with self.cached_session() as sess:
      next_element = dataset.make_one_shot_iterator().get_next()
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element)

  def testSimpleDirectory(self):
    """Test the MatchingFiles dataset with a simple directory"""

    filenames = ['a', 'b', 'c']
    self._touchTempFiles(filenames)

    dataset = MatchingFilesDataset(os.path.join(self.tmp_dir, '*'))
    with self.cached_session() as sess:
      next_element = dataset.make_one_shot_iterator().get_next()

      expected_filenames = []
      actual_filenames = []
      for filename in filenames:
        expected_filenames.append(
            compat.as_bytes(os.path.join(self.tmp_dir, filename)))
        actual_filenames.append(compat.as_bytes(sess.run(next_element)))

      self.assertItemsEqual(expected_filenames, actual_filenames)
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element)

  def testFileSuffixes(self):
    """Test the MatchingFiles dataset using the suffixes of filename"""

    filenames = ['a.txt', 'b.py', 'c.py', 'd.pyc']
    self._touchTempFiles(filenames)

    dataset = MatchingFilesDataset(os.path.join(self.tmp_dir, '*.py'))
    with self.cached_session() as sess:
      next_element = dataset.make_one_shot_iterator().get_next()
      expected_filenames = []
      actual_filenames = []
      for filename in filenames[1:-1]:
        expected_filenames.append(
            compat.as_bytes(os.path.join(self.tmp_dir, filename)))
        actual_filenames.append(compat.as_bytes(sess.run(next_element)))

      self.assertItemsEqual(expected_filenames, actual_filenames)
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element)

  def testFileMiddles(self):
    """Test the MatchingFiles dataset using the middles of filename"""

    filenames = ['a.txt', 'b.py', 'c.pyc']
    self._touchTempFiles(filenames)

    dataset = MatchingFilesDataset(os.path.join(self.tmp_dir, '*.py*'))
    with self.cached_session() as sess:
      next_element = dataset.make_one_shot_iterator().get_next()
      expected_filenames = []
      actual_filenames = []
      for filename in filenames[1:]:
        expected_filenames.append(
            compat.as_bytes(os.path.join(self.tmp_dir, filename)))
        actual_filenames.append(compat.as_bytes(sess.run(next_element)))

      self.assertItemsEqual(expected_filenames, actual_filenames)
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element)

  def testNestedDirectories(self):
    """Test the MatchingFiles dataset with nested directories"""

    filenames = []
    width = 8
    depth = 4
    for i in range(width):
      for j in range(depth):
        new_base = os.path.join(self.tmp_dir, str(i),
                                *[str(dir_name) for dir_name in range(j)])
        os.makedirs(new_base, exist_ok=True)
        for f in ['a.txt', 'b.py', 'c.pyc']:
          filename = os.path.join(new_base, f)
          filenames.append(filename)
          open(filename, 'w').close()

    patterns = []
    for i in range(depth):
      pattern = '{}/{}/*.txt'.format(
          self.tmp_dir, os.path.join(*['**' for _ in range(i + 1)]))
      patterns.append(pattern)

    dataset = MatchingFilesDataset(patterns)
    with self.cached_session() as sess:
      next_element = dataset.make_one_shot_iterator().get_next()
      expected_filenames = [compat.as_bytes(file)
                            for file in filenames if file.endswith('.txt')]
      actual_filenames = []
      while True:
        try:
          actual_filenames.append(compat.as_bytes(sess.run(next_element)))
        except errors.OutOfRangeError:
          break

      self.assertItemsEqual(expected_filenames, actual_filenames)


class MatchingFilesDatasetBenchmark(test.Benchmark):

  def benchmarkNestedDirectories(self):
    tmp_dir = tempfile.mkdtemp()
    width = 1000
    depth = 10
    for i in range(width):
      for j in range(depth):
        new_base = os.path.join(tmp_dir, str(i),
                                *[str(dir_name) for dir_name in range(j)])
        if not os.path.exists(new_base):
          os.makedirs(new_base)
        for f in ['a.txt', 'b.py', 'c.pyc']:
          filename = os.path.join(new_base, f)
          open(filename, 'w').close()

    patterns = []
    for i in range(depth):
      pattern = '{}/{}/*.txt'.format(tmp_dir,
                                     os.path.join(
                                         *['**' for _ in range(i + 1)]))
      patterns.append(pattern)

    deltas = []
    iters = 3
    for _ in range(iters):
      with ops.Graph().as_default():
        dataset = MatchingFilesDataset(patterns)
        next_element = dataset.make_one_shot_iterator().get_next()

        with session.Session() as sess:
          sub_deltas = []
          while True:
            try:
              start = time.time()
              sess.run(next_element)
              end = time.time()
              sub_deltas.append(end - start)
            except errors.OutOfRangeError:
              break
          deltas.append(sub_deltas)

    median_deltas = np.median(deltas, axis=0)
    print("Nested directory size (width*depth): %d*%d Median wall time: "
          "%fs (read first filename), %fs (read second filename), avg %fs"
          " (read %d more filenames)" % (width, depth,
                                         median_deltas[0],
                                         median_deltas[1],
                                         np.average(median_deltas[2:]),
                                         len(median_deltas) - 2))
    self.report_benchmark(
        iters=iters,
        wall_time=np.sum(median_deltas),
        extras={"read first file:": median_deltas[0],
                "read second file:": median_deltas[1],
                "avg time for reading %d more filenames:" %
                (len(median_deltas) - 2):
                np.average(median_deltas[2:])},
        name="benchmark_matching_files_dataset_nesteddirectory(%d*%d)" %
        (width, depth))

    shutil.rmtree(tmp_dir, ignore_errors=True)


if __name__ == "__main__":
  test.main()
