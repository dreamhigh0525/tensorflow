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
"""Correctness tests for tf.keras using DistributionStrategy."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from absl.testing import parameterized
import numpy as np

from tensorflow.contrib.distribute.python import combinations
from tensorflow.contrib.distribute.python import mirrored_strategy
from tensorflow.contrib.distribute.python import tpu_strategy
from tensorflow.python import keras
from tensorflow.python.data.ops import dataset_ops
from tensorflow.python.distribute import distribute_lib
from tensorflow.python.eager import context
from tensorflow.python.eager import test
from tensorflow.python.framework import random_seed
from tensorflow.python.keras.engine import distributed_training_utils
from tensorflow.python.keras.optimizer_v2 import gradient_descent as gradient_descent_keras
from tensorflow.python.training import gradient_descent

_RANDOM_SEED = 1337

# Note: Please make sure the tests in this file are also covered in
# keras_backward_compat_test for features that are supported with both APIs.


all_strategies = [
    combinations.default_strategy,
    combinations.one_device_strategy,
    combinations.mirrored_strategy_with_gpu_and_cpu,
    combinations.mirrored_strategy_with_two_gpus,
    combinations.core_mirrored_strategy_with_gpu_and_cpu,
    combinations.core_mirrored_strategy_with_two_gpus,
    combinations.tpu_strategy,  # steps_per_run=2
    combinations.tpu_strategy_one_step,
]


def all_strategy_combinations_with_eager_and_graph_modes():
  return combinations.combine(distribution=all_strategies,
                              mode=['graph', 'eager'])


def all_strategy_combinations_with_graph_mode():
  return combinations.combine(distribution=all_strategies, mode=['graph'])


def strategy_and_input_combinations():
  return (
      combinations.times(
          combinations.combine(distribution=all_strategies),
          combinations.combine(mode=['graph', 'eager'],
                               use_numpy=[True, False],
                               use_validation_data=[True, False])))


class MaybeDistributionScope(object):
  """Provides a context allowing no distribution strategy."""

  def __init__(self, distribution):
    self._distribution = distribution
    self._scope = None

  def __enter__(self):
    if self._distribution:
      self._scope = self._distribution.scope()
      self._scope.__enter__()

  def __exit__(self, exc_type, value, traceback):
    if self._distribution:
      self._scope.__exit__(exc_type, value, traceback)
      self._scope = None


def _create_dnn_model(weights=None, distribution=None, compile_model=True):
  with MaybeDistributionScope(distribution):
    # We add few non-linear layers to make it non-trivial.
    model = keras.Sequential()
    model.add(keras.layers.Dense(10, activation='relu', input_shape=(1,)))
    model.add(keras.layers.Dense(10, activation='relu'))
    model.add(keras.layers.Dense(10, activation='relu'))
    model.add(keras.layers.Dense(1))

    if weights:
      model.set_weights(weights)

    if compile_model:
      model.compile(
          loss=keras.losses.mean_squared_error,
          optimizer=gradient_descent_keras.SGD(0.5),
          metrics=['mse'])
    return model


def batch_wrapper(dataset, batch_size, distribution, repeat=None):
  if repeat:
    dataset = dataset.repeat(repeat)
  # TPUs currently require fully defined input shapes, drop_remainder ensures
  # the input will have fully defined shapes.
  if isinstance(distribution, tpu_strategy.TPUStrategy):
    return dataset.batch(batch_size, drop_remainder=True)
  else:
    return dataset.batch(batch_size)


def get_batch_size(global_batch_size, distribution):
  batch_size = global_batch_size
  # TODO(b/118776054): Use global batch size for Keras/DS support.
  use_per_core_batch_size = (
      distribution and
      not distributed_training_utils.global_batch_size_supported(distribution))
  if use_per_core_batch_size:
    batch_size //= distribution.num_replicas_in_sync
  return batch_size


def get_correctness_test_inputs(use_numpy, use_validation_data,
                                with_distribution, x_train, y_train, x_predict):
  """Generates the inputs for correctness check when enable Keras with DS."""
  training_epochs = 2
  global_batch_size = 64
  batch_size = get_batch_size(global_batch_size, with_distribution)

  if use_numpy:
    training_inputs = {
        'batch_size': batch_size,
        'x': x_train,
        'y': y_train,
        'epochs': training_epochs,
        'shuffle': False,
    }

    if use_validation_data:
      eval_inputs = None
      training_inputs['validation_data'] = (x_train, y_train)
    else:
      eval_inputs = {
          'batch_size': batch_size,
          'x': x_train,
          'y': y_train,
      }
    predict_inputs = {
        'x': np.array(x_predict, dtype=np.float32),
    }
  else:
    # For dataset inputs, we do not pass batch_size to
    # keras.fit/evaluate/predict. The batch size is part of the dataset.
    train_dataset = dataset_ops.Dataset.from_tensor_slices((x_train, y_train))
    x = batch_wrapper(train_dataset, batch_size, with_distribution,
                      repeat=training_epochs)

    training_inputs = {
        'batch_size': None,
        'x': x,
        'y': None,
        'epochs': training_epochs,
        'shuffle': False,
        'steps_per_epoch': len(x_train) // global_batch_size,
    }
    if use_validation_data:
      eval_inputs = None  # Remove the eval_inputs
      eval_dataset = dataset_ops.Dataset.from_tensor_slices((x_train, y_train))
      x = batch_wrapper(eval_dataset, batch_size, with_distribution)
      training_inputs['validation_data'] = x
      training_inputs['validation_steps'] = 5
    else:
      eval_inputs = {
          'batch_size': None,
          'x': x,
          'y': None,
          'steps': 20,
      }

    predict_batch_size = get_batch_size(len(x_predict), with_distribution)
    predict_dataset = dataset_ops.Dataset.from_tensor_slices(x_predict)
    predict_dataset = batch_wrapper(predict_dataset, predict_batch_size,
                                    with_distribution)
    predict_inputs = {
        'steps': 1,
        'x': predict_dataset,
    }

  return training_inputs, eval_inputs, predict_inputs


def fit_eval_and_predict(
    initial_weights, input_fn, distribution=None):
  model = _create_dnn_model(initial_weights, distribution)
  training_inputs, eval_inputs, predict_inputs = input_fn(distribution)

  result = {}
  result['training_history_1'] = model.fit(**training_inputs).history

  if eval_inputs is not None:
    result['eval_result_1'] = model.evaluate(**eval_inputs)

  result['weights_1'] = model.get_weights()

  if predict_inputs is not None:
    result['predict_result_1'] = model.predict(**predict_inputs)

  # Train and eval again to mimic user's flow.

  result['training_history_2'] = model.fit(**training_inputs).history

  if eval_inputs is not None:
    result['eval_result_2'] = model.evaluate(**eval_inputs)

  result['weights_2'] = model.get_weights()

  return result


def compare_results(results_with_ds, results_without_ds, distribution,
                    testcase):
  default_tolerance = 1e-5
  tol_table = {}

  if isinstance(distribution, (
      mirrored_strategy.MirroredStrategy,
      mirrored_strategy.CoreMirroredStrategy,
      distribute_lib._DefaultDistributionStrategy)):  # pylint: disable=protected-access
    # TODO(b/119257215): Weights are not exactly the same, so use larger
    # tolerance for now. Predict should be related to weights.
    tol_table = {
        'weights_1': 1e-4,
        'weights_2': 1e-4,
        'predict_result_1': 1e-4,
    }

  for key in results_with_ds:
    if (key.startswith('training_history') and
        isinstance(distribution, tpu_strategy.TPUStrategy) and
        distribution.extended.steps_per_run > 1):
      # TODO(b/119894254): Enable this test for all cases once the
      # underlying bug is fixed.
      continue

    tolerance = tol_table.get(key, default_tolerance)

    testcase.assertAllClose(
        results_with_ds[key],
        results_without_ds[key],
        atol=tolerance,
        rtol=tolerance,
        msg='Fail to assert {}.'.format(key))


class LearningRateBatchScheduler(keras.callbacks.Callback):

  def __init__(self, update_freq=None):
    self._update_freq = update_freq

  def on_batch_begin(self, batch, logs=None):
    if self._update_freq and batch % self._update_freq != 0:
      return

    # To avoid divergence, limit the value range.
    lr = 0.001 * (batch % 10)
    keras.backend.set_value(self.model.optimizer.lr, lr)


class TestDistributionStrategyCorrectness(test.TestCase,
                                          parameterized.TestCase):

  def _should_skip_tpu_with_eager(self, distribution):
    return (context.executing_eagerly() and
            isinstance(distribution, tpu_strategy.TPUStrategy))

  @combinations.generate(all_strategy_combinations_with_eager_and_graph_modes())
  def test_metric_correctness(self, distribution):
    if self._should_skip_tpu_with_eager(distribution):
      self.skipTest('TPUStrategy does not support eager mode now.')

    with self.cached_session():
      keras.backend.set_image_data_format('channels_last')
      num_samples = 10000

      x_train = np.random.randint(0, 2, num_samples)
      x_train = np.reshape(x_train, (num_samples, 1))
      y_train = x_train
      x_train = x_train.astype('float32')
      y_train = y_train.astype('float32')

      # Create identity model.
      with distribution.scope():
        model = keras.Sequential()
        model.add(
            keras.layers.Dense(1, input_shape=(1,), kernel_initializer='ones'))
        model.compile(
            loss=keras.losses.mean_squared_error,
            optimizer=gradient_descent.GradientDescentOptimizer(0.5),
            metrics=[keras.metrics.BinaryAccuracy()])

      batch_size = 64
      batch_size = get_batch_size(batch_size, distribution)
      train_dataset = dataset_ops.Dataset.from_tensor_slices((x_train, y_train))
      train_dataset = batch_wrapper(train_dataset, batch_size, distribution)

      history = model.fit(x=train_dataset, epochs=2, steps_per_epoch=10)
      self.assertEqual(history.history['binary_accuracy'], [1.0, 1.0])

  @combinations.generate(all_strategy_combinations_with_eager_and_graph_modes())
  def test_eval_metrics_correctness(self, distribution):
    if self._should_skip_tpu_with_eager(distribution):
      self.skipTest('TPUStrategy does not support eager mode now.')

    with self.cached_session():
      with distribution.scope():
        model = keras.Sequential()
        model.add(
            keras.layers.Dense(
                3, activation='relu', input_dim=4, kernel_initializer='ones'))
        model.add(
            keras.layers.Dense(
                1, activation='sigmoid', kernel_initializer='ones'))
        model.compile(
            loss='mae',
            metrics=['accuracy', keras.metrics.BinaryAccuracy()],
            optimizer=gradient_descent.GradientDescentOptimizer(0.001))

      # verify correctness of stateful and stateless metrics.
      x = np.ones((100, 4)).astype('float32')
      y = np.ones((100, 1)).astype('float32')
      dataset = dataset_ops.Dataset.from_tensor_slices((x, y)).repeat()
      dataset = batch_wrapper(dataset, 4, distribution)
      outs = model.evaluate(dataset, steps=10)
      self.assertEqual(outs[1], 1.)
      self.assertEqual(outs[2], 1.)

      y = np.zeros((100, 1)).astype('float32')
      dataset = dataset_ops.Dataset.from_tensor_slices((x, y)).repeat()
      dataset = batch_wrapper(dataset, 4, distribution)
      outs = model.evaluate(dataset, steps=10)
      self.assertEqual(outs[1], 0.)
      self.assertEqual(outs[2], 0.)

  @combinations.generate(strategy_and_input_combinations())
  def test_correctness(self, distribution, use_numpy, use_validation_data):
    if self._should_skip_tpu_with_eager(distribution):
      self.skipTest('TPUStrategy does not support eager mode now.')

    if context.executing_eagerly() and use_numpy:
      self.skipTest('Numpy as inputs is not supported with strategy in eager.')

    if context.executing_eagerly() and use_validation_data:
      self.skipTest('TODO')

    with self.cached_session():
      keras.backend.set_image_data_format('channels_last')
      np.random.seed(_RANDOM_SEED)
      random_seed.set_random_seed(_RANDOM_SEED)

      # Train, eval, and predict datasets are created with the same input numpy
      # arrays.
      # TODO(xiejw): Change this back to 10000, once we support final partial
      # batch.
      num_samples = 9984
      x_train = np.random.rand(num_samples, 1)
      y_train = 3 * x_train
      x_train = x_train.astype('float32')
      y_train = y_train.astype('float32')
      x_predict = [[1.], [2.], [3.], [4.]]

      # The model is built once and the initial weights are saved.
      # This is used to initialize the model for both the distribution and
      # non-distribution run.
      model = _create_dnn_model(compile_model=False)
      initial_weights = model.get_weights()

      def input_fn(dist):
        return get_correctness_test_inputs(
            use_numpy, use_validation_data, dist, x_train, y_train, x_predict)

      results_with_ds = fit_eval_and_predict(
          initial_weights, input_fn=input_fn, distribution=distribution)
      results_without_ds = fit_eval_and_predict(
          initial_weights, input_fn=input_fn, distribution=None)
      compare_results(results_with_ds, results_without_ds, distribution,
                      testcase=self)

  @combinations.generate(all_strategy_combinations_with_graph_mode())
  def test_dynamic_lr(self, distribution):

    with self.cached_session():

      keras.backend.set_image_data_format('channels_last')
      np.random.seed(_RANDOM_SEED)
      random_seed.set_random_seed(_RANDOM_SEED)

      # TODO(xiejw): Change this back to 10000, once we support final partial
      # batch.
      num_samples = 9984
      x_train = np.random.rand(num_samples, 1).astype('float32')
      y_train = 3 * x_train

      model = _create_dnn_model(compile_model=False)
      initial_weights = model.get_weights()

      update_freq = None
      if (isinstance(distribution, tpu_strategy.TPUStrategy) and
          distribution.extended.steps_per_run > 1):
        # For TPUStrategy with steps_per_run > 1, the callback is not invoked
        # every step. So, to compare the CPU/TPU, we let the CPU to behave the
        # same as TPU.
        update_freq = distribution.extended.steps_per_run

      def input_fn(dist):
        training_epochs = 2
        global_batch_size = 64
        batch_size = get_batch_size(global_batch_size, dist)

        training_inputs = {
            'batch_size': batch_size,
            'x': x_train,
            'y': y_train,
            'epochs': training_epochs,
            'shuffle': False,
            'callbacks': [LearningRateBatchScheduler(update_freq)],
            'validation_data': (x_train, y_train)
        }
        # In this test case, we do not care eval and predict.
        eval_inputs, predict_inputs = None, None
        return training_inputs, eval_inputs, predict_inputs

      results_with_ds = fit_eval_and_predict(
          initial_weights, input_fn=input_fn, distribution=distribution)
      results_without_ds = fit_eval_and_predict(
          initial_weights, input_fn=input_fn, distribution=None)
      compare_results(results_with_ds, results_without_ds, distribution,
                      testcase=self)


if __name__ == '__main__':
  test.main()
