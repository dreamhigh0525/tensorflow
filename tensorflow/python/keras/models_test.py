# Copyright 2016 The TensorFlow Authors. All Rights Reserved.
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
"""Tests for `models.py` (model cloning, mainly)."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import os

import numpy as np

from tensorflow.python import keras
from tensorflow.python.framework import test_util
from tensorflow.python.keras import metrics
from tensorflow.python.keras import models
from tensorflow.python.platform import test
from tensorflow.python.training import adam


class TestModelCloning(test.TestCase):

  def test_clone_sequential_model(self):
    with self.test_session():
      val_a = np.random.random((10, 4))
      val_out = np.random.random((10, 4))

      model = keras.models.Sequential()
      model.add(keras.layers.Dense(4, input_shape=(4,)))
      model.add(keras.layers.BatchNormalization())
      model.add(keras.layers.Dropout(0.5))
      model.add(keras.layers.Dense(4))

    # Everything should work in a new session.
    keras.backend.clear_session()

    with self.test_session():
      # With placeholder creation
      new_model = keras.models.clone_model(model)
      # update ops from batch norm needs to be included
      self.assertEquals(len(new_model.get_updates_for(new_model.inputs)), 2)
      new_model.compile('rmsprop', 'mse')
      new_model.train_on_batch(val_a, val_out)

      # On top of new tensor
      input_a = keras.Input(shape=(4,))
      new_model = keras.models.clone_model(
          model, input_tensors=input_a)
      self.assertEquals(len(new_model.get_updates_for(new_model.inputs)), 2)
      new_model.compile('rmsprop', 'mse')
      new_model.train_on_batch(val_a, val_out)

      # On top of new, non-Keras tensor
      input_a = keras.backend.variable(val_a)
      new_model = keras.models.clone_model(
          model, input_tensors=input_a)
      self.assertEquals(len(new_model.get_updates_for(new_model.inputs)), 2)
      new_model.compile('rmsprop', 'mse')
      new_model.train_on_batch(None, val_out)

  def test_clone_functional_model(self):
    with self.test_session():
      val_a = np.random.random((10, 4))
      val_b = np.random.random((10, 4))
      val_out = np.random.random((10, 4))

      input_a = keras.Input(shape=(4,))
      input_b = keras.Input(shape=(4,))
      dense_1 = keras.layers.Dense(4,)
      dense_2 = keras.layers.Dense(4,)

      x_a = dense_1(input_a)
      x_a = keras.layers.Dropout(0.5)(x_a)
      x_a = keras.layers.BatchNormalization()(x_a)
      x_b = dense_1(input_b)
      x_a = dense_2(x_a)
      outputs = keras.layers.add([x_a, x_b])
      model = keras.models.Model([input_a, input_b], outputs)

    # Everything should work in a new session.
    keras.backend.clear_session()

    with self.test_session():
      # With placeholder creation
      new_model = keras.models.clone_model(model)
      self.assertEquals(len(new_model.get_updates_for(new_model.inputs)), 2)
      new_model.compile('rmsprop', 'mse')
      new_model.train_on_batch([val_a, val_b], val_out)

      # On top of new tensors
      input_a = keras.Input(shape=(4,), name='a')
      input_b = keras.Input(shape=(4,), name='b')
      new_model = keras.models.clone_model(
          model, input_tensors=[input_a, input_b])
      self.assertEquals(len(new_model.get_updates_for(new_model.inputs)), 2)
      new_model.compile('rmsprop', 'mse')
      new_model.train_on_batch([val_a, val_b], val_out)

      # On top of new, non-Keras tensors
      input_a = keras.backend.variable(val_a)
      input_b = keras.backend.variable(val_b)
      new_model = keras.models.clone_model(
          model, input_tensors=[input_a, input_b])
      self.assertEquals(len(new_model.get_updates_for(new_model.inputs)), 2)
      new_model.compile('rmsprop', 'mse')
      new_model.train_on_batch(None, val_out)

  @test_util.run_in_graph_and_eager_modes
  def test_clone_functional_model_with_masking(self):
    with self.test_session():
      x = np.array([[[1], [1]], [[0], [0]]])
      inputs = keras.Input((2, 1))
      outputs = keras.layers.Masking(mask_value=0)(inputs)
      outputs = keras.layers.TimeDistributed(
          keras.layers.Dense(1, kernel_initializer='one'))(outputs)
      model = keras.Model(inputs, outputs)

      model = keras.models.clone_model(model)
      model.compile(loss='mse', optimizer=adam.AdamOptimizer(0.01))
      y = np.array([[[1], [1]], [[1], [1]]])
      loss = model.train_on_batch(x, y)
      self.assertEqual(float(loss), 0.)

  def test_model_cloning_invalid_use_cases(self):
    seq_model = keras.models.Sequential()
    seq_model.add(keras.layers.Dense(4, input_shape=(4,)))

    x = keras.Input((4,))
    y = keras.layers.Dense(4)(x)
    fn_model = keras.models.Model(x, y)

    with self.assertRaises(ValueError):
      keras.models._clone_functional_model(seq_model)
    with self.assertRaises(ValueError):
      keras.models._clone_functional_model(None)
    with self.assertRaises(ValueError):
      keras.models._clone_sequential_model(fn_model)

    with self.assertRaises(ValueError):
      keras.models._clone_sequential_model(seq_model, input_tensors=[x, x])
    with self.assertRaises(ValueError):
      keras.models._clone_sequential_model(seq_model, input_tensors=y)


class CheckpointingTests(test.TestCase):

  @test_util.run_in_graph_and_eager_modes
  def test_optimizer_dependency(self):
    model = keras.models.Sequential()
    model.add(keras.layers.Dense(1, input_shape=(4,)))
    opt = adam.AdamOptimizer(0.01)
    model.compile(optimizer=opt, loss='mse')
    model.fit(x=np.array([[1., 2., 3., 4.]]), y=[1.], epochs=2)
    save_prefix = os.path.join(self.get_temp_dir(), 'ckpt')
    beta1_power, _ = opt._get_beta_accumulators()
    self.evaluate(beta1_power.assign(12.))
    model.save_weights(save_prefix)
    self.evaluate(beta1_power.assign(13.))
    model.load_weights(save_prefix)
    self.assertEqual(12., self.evaluate(beta1_power))


class TestModelBackend(test.TestCase):

  def test_model_backend_float64_use_cases(self):
    # Test case for GitHub issue 19318
    floatx = keras.backend.floatx()
    keras.backend.set_floatx('float64')

    x = keras.Input((5,))
    y = keras.layers.Dense(1)(x)
    model = keras.models.Model(x, y)
    model.compile('rmsprop', 'mse')

    keras.backend.set_floatx(floatx)


class TestCloneAndBuildModel(test.TestCase):

  def test_clone_and_build_non_compiled_model(self):
    with self.test_session():
      inp = np.random.random((10, 4))
      out = np.random.random((10, 4))

      model = keras.models.Sequential()
      model.add(keras.layers.Dense(4, input_shape=(4,)))
      model.add(keras.layers.BatchNormalization())
      model.add(keras.layers.Dropout(0.5))
      model.add(keras.layers.Dense(4))

    # Everything should work in a new session.
    keras.backend.clear_session()

    with self.test_session():
      # With placeholder creation
      new_model = models.clone_and_build_model(model, compile_clone=True)
      with self.assertRaisesRegexp(RuntimeError, 'must compile'):
        new_model.evaluate(inp, out)
      with self.assertRaisesRegexp(RuntimeError, 'must compile'):
        new_model.train_on_batch(inp, out)
      new_model.compile('rmsprop', 'mse')
      new_model.train_on_batch(inp, out)

      # Create new tensors for inputs and targets
      input_a = keras.Input(shape=(4,))
      target_a = keras.Input(shape=(4,))
      new_model = models.clone_and_build_model(model, input_tensors=input_a,
                                               target_tensors=[target_a],
                                               compile_clone=True)
      with self.assertRaisesRegexp(RuntimeError, 'must compile'):
        new_model.evaluate(inp, out)
      with self.assertRaisesRegexp(RuntimeError, 'must compile'):
        new_model.train_on_batch(inp, out)
      new_model.compile('rmsprop', 'mse')
      new_model.train_on_batch(inp, out)

  def _assert_same_compile_params(self, model):
    """Assert that two models have the same compile parameters."""

    self.assertEqual('mse', model.loss)
    self.assertTrue(
        isinstance(model.optimizer, keras.optimizers.RMSprop))
    self.assertEqual(['acc', metrics.categorical_accuracy], model.metrics)

  def _clone_and_build_test_helper(self, model, is_subclassed=False):
    inp = np.random.random((10, 4))
    out = np.random.random((10, 4))

    # Everything should work in a new session.
    keras.backend.clear_session()

    with self.test_session():
      # With placeholder creation
      new_model = models.clone_and_build_model(
          model, compile_clone=True, in_place_reset=is_subclassed)

      self._assert_same_compile_params(new_model)
      new_model.train_on_batch(inp, out)
      new_model.evaluate(inp, out)

      # Create new tensors for inputs and targets
      input_a = keras.Input(shape=(4,), name='a')
      new_model = models.clone_and_build_model(
          model, input_tensors=input_a, compile_clone=True,
          in_place_reset=is_subclassed)
      self._assert_same_compile_params(new_model)
      new_model.train_on_batch(inp, out)
      new_model.evaluate(inp, out)

      target_a = keras.Input(shape=(4,), name='b')
      new_model = models.clone_and_build_model(
          model, input_tensors=input_a, target_tensors=[target_a],
          compile_clone=True, in_place_reset=is_subclassed)
      self._assert_same_compile_params(new_model)
      new_model.train_on_batch(inp, out)
      new_model.evaluate(inp, out)

  def test_clone_and_build_compiled_sequential_model(self):
    with self.test_session():
      model = keras.models.Sequential()
      model.add(keras.layers.Dense(4, input_shape=(4,)))
      model.add(keras.layers.BatchNormalization())
      model.add(keras.layers.Dropout(0.5))
      model.add(keras.layers.Dense(4))
      model.compile('rmsprop', 'mse',
                    metrics=['acc', metrics.categorical_accuracy])

    self._clone_and_build_test_helper(model)

  def test_clone_and_build_functional_model(self):
    with self.test_session():
      input_a = keras.Input(shape=(4,))
      dense_1 = keras.layers.Dense(4,)
      dense_2 = keras.layers.Dense(4,)

      x_a = dense_1(input_a)
      x_a = keras.layers.Dropout(0.5)(x_a)
      x_a = keras.layers.BatchNormalization()(x_a)
      x_a = dense_2(x_a)
      model = keras.models.Model(input_a, x_a)
      model.compile('rmsprop', 'mse',
                    metrics=['acc', metrics.categorical_accuracy])

    self._clone_and_build_test_helper(model)

  def test_clone_and_build_subclassed_model(self):
    class SubclassedModel(keras.Model):

      def __init__(self):
        super(SubclassedModel, self).__init__()
        self.layer1 = keras.layers.Dense(4)
        self.layer2 = keras.layers.Dense(4)

      def call(self, inp):
        out = self.layer1(inp)
        out = keras.layers.BatchNormalization()(out)
        out = keras.layers.Dropout(0.5)(out)
        out = self.layer2(out)
        return out

    with self.test_session():
      model = SubclassedModel()
      model.compile('rmsprop', 'mse',
                    metrics=['acc', metrics.categorical_accuracy])
    self._clone_and_build_test_helper(model, True)


if __name__ == '__main__':
  test.main()
