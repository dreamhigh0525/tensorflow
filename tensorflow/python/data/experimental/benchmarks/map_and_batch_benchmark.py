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
"""Benchmarks for `tf.data.experimental.map_and_batch()`."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import hashlib
import itertools

import numpy as np

from tensorflow.core.protobuf import config_pb2
from tensorflow.python.data.experimental.ops import batching
from tensorflow.python.data.benchmarks import benchmark_base
from tensorflow.python.data.ops import dataset_ops
from tensorflow.python.framework import constant_op
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import random_ops

_NUMPY_RANDOM_SEED = 42


class MapAndBatchBenchmark(benchmark_base.DatasetBenchmarkBase):
  """Benchmarks for `tf.data.experimental.map_and_batch()`."""

  def benchmark_map_and_batch(self):
    """Measures the performance of parallelized batching."""
    shapes = [(), (10,), (10, 10), (10, 10, 10), (224, 224, 3)]
    batch_size_values = [1, 32, 64, 128, 1024]

    for shape in shapes:
      for batch_size in batch_size_values:

        dataset = dataset_ops.Dataset.range(1000000000)
        dense_value = random_ops.random_normal(shape=shape)

        dataset = dataset.apply(batching.map_and_batch(
            lambda _: dense_value, batch_size))
        options = dataset_ops.Options()
        options.experimental_optimization.apply_default_optimizations = False
        dataset = dataset.with_options(options)

        self.run_and_report_benchmark(
            dataset=dataset,
            num_elements=100,
            iters=100,
            warmup=True,
            name="num_elements_%d_batch_size_%d" % (np.prod(shape), batch_size)
        )

  def benchmark_map_and_batch_chaining_versus_fusing(self):
    """Compares the performance of chaining and fusing map and batch.

    NOTE: It is recommended to build the benchmark with
    `-c opt --copt=-mavx --copt=-mavx2 --copt=-mfma --copt=-gmlt`
    and execute it on a machine with at least 32 CPU cores.
    """

    # Sequential pipeline configurations.
    seq_elem_size_series = itertools.product([1], [1], [1, 2, 4, 8], [16])
    seq_batch_size_series = itertools.product([1], [1], [1], [8, 16, 32, 64])

    # Parallel pipeline configuration.
    par_elem_size_series = itertools.product([32], [32], [1, 2, 4, 8], [256])
    par_batch_size_series = itertools.product([32], [32], [1],
                                              [128, 256, 512, 1024])
    par_num_calls_series = itertools.product([8, 16, 32, 64], [32], [1], [512])
    par_inter_op_series = itertools.product([32], [8, 16, 32, 64], [1], [512])

    def name(method, label, num_calls, inter_op, element_size, batch_size):
      return ("%s_id_%s_num_calls_%d_inter_op_%d_elem_size_%d_batch_size_%d" % (
          method,
          hashlib.sha1((label).encode("utf-8")).hexdigest()[:8],
          num_calls,
          inter_op,
          element_size,
          batch_size,
      ))

    def benchmark(label, series):
      """Runs benchmark the given series."""

      def make_dataset(element_size, num_calls, batch_size):  # pylint: disable=missing-docstring
        k = 1024 * 1024
        x = constant_op.constant(np.random.rand(element_size, 4 * k))
        y = constant_op.constant(np.random.rand(4 * k, 1))
        dataset = dataset_ops.Dataset.range(1000000000000).map(lambda _: (x, y))
        dataset = dataset.map(
            math_ops.matmul,
            num_parallel_calls=num_calls).batch(batch_size=batch_size)
        options = dataset_ops.Options()
        options.experimental_optimization.apply_default_optimizations = False
        return dataset.with_options(options)

      for num_calls, inter_op, element_size, batch_size in series:
        num_iters = 1024 // (
            (element_size * batch_size) // min(num_calls, inter_op))
        # By default the chained map().batch() calls will not be fused.
        chained_dataset = make_dataset(element_size, num_calls, batch_size)
        session_config = config_pb2.ConfigProto(
            inter_op_parallelism_threads=inter_op,
            use_per_session_threads=True)

        self.run_and_report_benchmark(
            dataset=chained_dataset,
            iters=num_iters,
            num_elements=batch_size,
            warmup=True,
            session_config=session_config,
            name=name("chained", label, num_calls, inter_op, element_size,
                      batch_size)
        )

        # Apply an option to the default dataset that will fuse map().batch().
        options = dataset_ops.Options()
        options.experimental_optimization.map_and_batch_fusion = True
        fused_dataset = chained_dataset.with_options(options)

        self.run_and_report_benchmark(
            dataset=fused_dataset,
            iters=num_iters,
            num_elements=batch_size,
            warmup=True,
            session_config=session_config,
            name=name("fused", label, num_calls, inter_op, element_size,
                      batch_size)
        )

    np.random.seed(_NUMPY_RANDOM_SEED)
    benchmark("Sequential element size evaluation", seq_elem_size_series)
    benchmark("Sequential batch size evaluation", seq_batch_size_series)
    benchmark("Parallel element size evaluation", par_elem_size_series)
    benchmark("Parallel batch size evaluation", par_batch_size_series)
    benchmark("Transformation parallelism evaluation", par_num_calls_series)
    benchmark("Threadpool size evaluation", par_inter_op_series)


if __name__ == "__main__":
  benchmark_base.test.main()
