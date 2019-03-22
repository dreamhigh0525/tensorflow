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
"""Tests for the `LatencyAllEdges` optimization."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.python.data.experimental.kernel_tests import stats_dataset_test_base
from tensorflow.python.data.experimental.ops import optimization
from tensorflow.python.data.experimental.ops import stats_aggregator
from tensorflow.python.data.ops import dataset_ops
from tensorflow.python.framework import test_util
from tensorflow.python.platform import test


# TODO(b/116314787): add test coverage for StatsAggregatorV2.
@test_util.run_v1_only("b/116314787, add test coverage")
class LatencyAllEdgesTest(stats_dataset_test_base.StatsDatasetTestBase):

  def testLatencyStatsOptimization(self):
    aggregator = stats_aggregator.StatsAggregator()
    dataset = dataset_ops.Dataset.from_tensors(1).apply(
        optimization.assert_next(
            ["LatencyStats", "Map", "LatencyStats", "Prefetch",
             "LatencyStats"])).map(lambda x: x * x).prefetch(1)
    options = dataset_ops.Options()
    options.experimental_optimization.apply_default_optimizations = False
    options.experimental_stats.latency_all_edges = True
    options.experimental_stats.aggregator = aggregator
    dataset = dataset.with_options(options)
    self.assertDatasetProduces(
        dataset,
        expected_output=[1],
        requires_initialization=True,
        num_test_iterations=1)
    summary_t = aggregator.get_summary()
    summary_str = self.evaluate(summary_t)
    self.assertSummaryHasCount(
        summary_str, self.regexForNodeName("record_latency::TensorDataset"), 1)
    self.assertSummaryHasCount(
        summary_str, self.regexForNodeName("record_latency::MapDataset"), 1)
    self.assertSummaryHasCount(
        summary_str, self.regexForNodeName("record_latency::PrefetchDataset"),
        1)


if __name__ == "__main__":
  test.main()
