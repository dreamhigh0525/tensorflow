# Copyright 2020 The TensorFlow Authors. All Rights Reserved.
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
"""Benchmarks on Bidirectional LSTM on IMDB."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import tensorflow as tf

from tensorflow.python.keras.benchmarks import benchmark_util


class BidirectionalLSTMBenchmark(tf.test.Benchmark):
  """Benchmarks for Bidirectional LSTM using `tf.test.Benchmark`."""

  # Required Arguments for measure_performance.
  #   x: Input data, it could be Numpy or load from tfds.
  #   y: Target data. If `x` is a dataset, generator instance,
  #      `y` should not be specified.
  #   loss: Loss function for model.
  #   optimizer: Optimizer for model.
  #   Other details can see in `measure_performance()` method of
  #   benchmark_util.

  def __init__(self):
    super(BidirectionalLSTMBenchmark, self).__init__()
    self.max_feature = 20000
    self.max_len = 200
    (self.imdb_x, self.imdb_y), _ = tf.keras.datasets.imdb.load_data(
        num_words=self.max_feature)
    self.imdb_x = tf.keras.preprocessing.sequence.pad_sequences(
        self.imdb_x, maxlen=self.max_len)

  def _build_model(self):
    """Model from https://keras.io/examples/nlp/bidirectional_lstm_imdb/."""
    inputs = tf.keras.Input(shape=(None,), dtype='int32')
    x = tf.keras.layers.Embedding(self.max_feature, 128)(inputs)
    x = tf.keras.layers.Bidirectional(
        tf.keras.layers.LSTM(64, return_sequences=True))(
            x)
    x = tf.keras.layers.Bidirectional(tf.keras.layers.LSTM(64))(x)
    outputs = tf.keras.layers.Dense(1, activation='sigmoid')(x)
    model = tf.keras.Model(inputs, outputs)
    return model

  def benchmark_bidirect_lstm_imdb_bs_128(self):
    """Measure performance with batch_size=128 and run_iters=3."""
    batch_size = 128
    run_iters = 3
    metrics, wall_time, extras = benchmark_util.measure_performance(
        self._build_model,
        x=self.imdb_x,
        y=self.imdb_y,
        batch_size=batch_size,
        run_iters=run_iters,
        optimizer='adam',
        loss='binary_crossentropy',
        metrics=['accuracy'])

    self.report_benchmark(
        iters=run_iters, wall_time=wall_time, metrics=metrics, extras=extras)

  def benchmark_bidirect_lstm_imdb_bs_256(self):
    """Measure performance with batch_size=256 and run_iters=2."""
    batch_size = 256
    run_iters = 2
    metrics, wall_time, extras = benchmark_util.measure_performance(
        self._build_model,
        x=self.imdb_x,
        y=self.imdb_y,
        batch_size=batch_size,
        run_iters=run_iters,
        optimizer='adam',
        loss='binary_crossentropy',
        metrics=['accuracy'])

    self.report_benchmark(
        iters=run_iters, wall_time=wall_time, metrics=metrics, extras=extras)

  def benchmark_bidirect_lstm_imdb_bs_512(self):
    """Measure performance with batch_size=512 and run_iters=4."""
    batch_size = 512
    run_iters = 4
    metrics, wall_time, extras = benchmark_util.measure_performance(
        self._build_model,
        x=self.imdb_x,
        y=self.imdb_y,
        batch_size=batch_size,
        run_iters=run_iters,
        optimizer='adam',
        loss='binary_crossentropy',
        metrics=['accuracy'])

    self.report_benchmark(
        iters=run_iters, wall_time=wall_time, metrics=metrics, extras=extras)


if __name__ == '__main__':
  tf.test.main()
