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
# pylint: disable=protected-access
"""Tests for saving/loading function for keras Model."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import os
import shutil

import numpy as np

from tensorflow.python import keras
from tensorflow.python.eager import context
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import test_util
from tensorflow.python.keras import keras_parameterized
from tensorflow.python.keras import regularizers
from tensorflow.python.keras import testing_utils
from tensorflow.python.keras.saving.saved_model import load as saved_model_load
from tensorflow.python.keras.utils import tf_utils
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import init_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import variables
from tensorflow.python.platform import test
from tensorflow.python.saved_model import load as tf_load
from tensorflow.python.saved_model import save as tf_save


class LayerWithLearningPhase(keras.engine.base_layer.Layer):

  def build(self, input_shape):
    self.input_spec = keras.layers.InputSpec(shape=[None] * len(input_shape))
    self.built = True

  def call(self, x, training=None):
    if training is None:
      training = keras.backend.learning_phase()
    output = tf_utils.smart_cond(
        training, lambda: x * 0, lambda: array_ops.identity(x))
    if not context.executing_eagerly():
      output._uses_learning_phase = True  # pylint: disable=protected-access
    return output

  def compute_output_shape(self, input_shape):
    return input_shape


@test_util.run_all_in_graph_and_eager_modes
class TestModelSavingAndLoadingV2(keras_parameterized.TestCase):

  def _save_model_dir(self, dirname='saved_model'):
    temp_dir = self.get_temp_dir()
    self.addCleanup(shutil.rmtree, temp_dir, ignore_errors=True)
    return os.path.join(temp_dir, dirname)

  @keras_parameterized.run_with_all_model_types
  def test_model_save_and_load(self):
    input_arr = np.random.random((1, 3)).astype(np.float32)
    target_arr = np.random.random((1, 4)).astype(np.float32)

    model = testing_utils.get_small_mlp(1, 4, input_dim=3)
    model.layers[-1].activity_regularizer = regularizers.get('l2')
    model.activity_regularizer = regularizers.get('l2')
    model.compile(
        loss='mse',
        optimizer='rmsprop')
    model.train_on_batch(input_arr, target_arr)

    def callable_loss():
      return math_ops.reduce_sum(model.weights[0])
    model.add_loss(callable_loss)
    saved_model_dir = self._save_model_dir()
    tf_save.save(model, saved_model_dir)

    loaded = saved_model_load.load(saved_model_dir)
    self.evaluate(variables.variables_initializer(loaded.variables))
    self.assertAllClose(self.evaluate(model.weights),
                        self.evaluate(loaded.weights))

    input_arr = constant_op.constant(
        np.random.random((1, 3)).astype(np.float32))
    self.assertAllClose(self.evaluate(model(input_arr)),
                        self.evaluate(loaded(input_arr)))
    # Validate losses. The order of conditional losses may change between the
    # model and loaded model, so sort the losses first.
    if context.executing_eagerly():
      self.assertAllClose(sorted(self.evaluate(model.losses)),
                          sorted(self.evaluate(loaded.losses)))
    else:
      self.assertAllClose(self.evaluate(model.get_losses_for(None)),
                          self.evaluate(loaded.get_losses_for(None)))
      self.assertAllClose(
          sorted(self.evaluate(model.get_losses_for(input_arr))),
          sorted(self.evaluate(loaded.get_losses_for(input_arr))))

  def test_trainable_weights(self):
    layer = keras.layers.Dense(4, name='custom_layer')
    layer.build([3,])
    layer.add_weight(
        'extra_weight', shape=[],
        initializer=init_ops.constant_initializer(11),
        trainable=True)
    layer.add_weight(
        'extra_weight_2', shape=[],
        initializer=init_ops.constant_initializer(12),
        trainable=False)

    saved_model_dir = self._save_model_dir()
    self.evaluate(variables.variables_initializer(layer.variables))
    tf_save.save(layer, saved_model_dir)
    loaded = saved_model_load.load(saved_model_dir)
    self.evaluate(variables.variables_initializer(loaded.variables))

    equal_attrs = ['name', '_expects_training_arg', 'trainable']
    for attr in equal_attrs:
      self.assertEqual(getattr(layer, attr), getattr(loaded, attr))

    all_close = ['weights', 'trainable_weights', 'non_trainable_weights']
    for attr in all_close:
      self.assertAllClose(self.evaluate(getattr(layer, attr)),
                          self.evaluate(getattr(loaded, attr)))

  def test_maintains_losses(self):
    """Tests that the layer losses do not change before and after export."""

    class LayerWithLoss(keras.layers.Layer):

      def call(self, inputs):
        self.add_loss(math_ops.reduce_sum(inputs), inputs)
        return inputs

    model = keras.models.Sequential([LayerWithLoss()])
    model.compile(
        loss='mse',
        optimizer='rmsprop')
    input_arr = np.random.random((1, 3)).astype(np.float32)
    target_arr = np.random.random((1, 3)).astype(np.float32)

    # Test that symbolic losses are maintained (train_on_batch saves symbolic
    # losses.)
    model.train_on_batch(input_arr, target_arr)
    previous_losses = model.losses[:]

    saved_model_dir = self._save_model_dir()
    tf_save.save(model, saved_model_dir)
    self.assertAllEqual(previous_losses, model.losses)

    if context.executing_eagerly():
      # Test that eager losses are maintained.
      model(input_arr)  # Calls model eagerly, creating eager losses.
      previous_losses = model.losses[:]
      tf_save.save(model, saved_model_dir)
      self.assertAllEqual(previous_losses, model.losses)

  def test_layer_with_learning_phase(self):
    layer = LayerWithLearningPhase()
    layer.build([None, None])
    saved_model_dir = self._save_model_dir()
    tf_save.save(layer, saved_model_dir)
    loaded = saved_model_load.load(saved_model_dir)
    input_arr = array_ops.ones((4, 3))

    # Run the layer, and use the keras backend learing phase
    keras.backend.set_learning_phase(0)
    self.assertAllEqual(input_arr, loaded(input_arr))
    keras.backend.set_learning_phase(1)
    self.assertAllEqual(array_ops.zeros((4, 3)), loaded(input_arr))

    # Run the layer while explicitly setting the training argument
    self.assertAllEqual(
        input_arr, loaded(input_arr, training=constant_op.constant(False)))
    self.assertAllEqual(
        array_ops.zeros((4, 3)),
        loaded(input_arr, training=constant_op.constant(True)))

  @keras_parameterized.run_with_all_model_types
  def test_standard_loader(self):
    model = testing_utils.get_small_mlp(1, 4, input_dim=3)
    model.activity_regularizer = regularizers.get('l2')
    def eager_loss():
      return math_ops.reduce_sum(model.weights[0])
    model.add_loss(eager_loss)

    # Call predict to ensure that all layers are built and inputs are set.
    model.predict(np.random.random((1, 3)))
    saved_model_dir = self._save_model_dir()

    tf_save.save(model, saved_model_dir)

    loaded = tf_load.load(saved_model_dir)
    self.evaluate(variables.variables_initializer(loaded.variables))
    all_close = ['variables', 'trainable_variables',
                 'non_trainable_variables']
    for attr in all_close:
      self.assertAllClose(self.evaluate(getattr(model, attr)),
                          self.evaluate(getattr(loaded.keras_api, attr)))
    self.assertLen(loaded.regularization_losses, 1)
    expected_layers = len(model.layers)
    self.assertEqual(expected_layers, len(loaded.keras_api.layers))
    input_arr = array_ops.ones((4, 3))
    self.assertAllClose(self.evaluate(model(input_arr)),
                        self.evaluate(loaded(input_arr)))

  @keras_parameterized.run_with_all_model_types
  def test_compiled_model(self):
    input_arr = np.random.random((1, 3))
    target_arr = np.random.random((1, 4))

    model = testing_utils.get_small_mlp(1, 4, input_dim=3)
    expected_predict = model.predict(input_arr)

    # Compile and save model.
    model.compile('rmsprop', 'mse')
    saved_model_dir = self._save_model_dir()
    tf_save.save(model, saved_model_dir)

    # TODO(b/134519980): Issue with model.fit if the model call function uses
    # a tf.function (Graph mode only).
    with context.eager_mode():
      loaded = saved_model_load.load(saved_model_dir)
      actual_predict = loaded.predict(input_arr)
      self.assertAllClose(expected_predict, actual_predict)

      loss_before = loaded.evaluate(input_arr, target_arr)
      loaded.fit(input_arr, target_arr)
      loss_after = loaded.evaluate(input_arr, target_arr)
      self.assertLess(loss_after, loss_before)
      predict = loaded.predict(input_arr)

      ckpt_path = os.path.join(self.get_temp_dir(), 'weights')
      loaded.save_weights(ckpt_path)

    # Ensure that the checkpoint is compatible with the original model.
    model.load_weights(ckpt_path)
    self.assertAllClose(predict, model.predict(input_arr))

  def test_metadata_input_spec(self):
    class LayerWithNestedSpec(keras.layers.Layer):

      def __init__(self):
        super(LayerWithNestedSpec, self).__init__()
        self.input_spec = {
            'a': keras.layers.InputSpec(max_ndim=3, axes={-1: 2}),
            'b': keras.layers.InputSpec(shape=(None, 2, 3), dtype='float16')}

    layer = LayerWithNestedSpec()
    saved_model_dir = self._save_model_dir()
    tf_save.save(layer, saved_model_dir)
    loaded = saved_model_load.load(saved_model_dir)
    self.assertEqual(3, loaded.input_spec['a'].max_ndim)
    self.assertEqual({-1: 2}, loaded.input_spec['a'].axes)
    self.assertAllEqual([None, 2, 3], loaded.input_spec['b'].shape)
    self.assertEqual('float16', loaded.input_spec['b'].dtype)

  def test_multi_input_model(self):
    input_1 = keras.layers.Input(shape=(3,))
    input_2 = keras.layers.Input(shape=(5,))
    model = keras.Model([input_1, input_2], [input_1, input_2])
    saved_model_dir = self._save_model_dir()

    model.save(saved_model_dir, save_format='tf')
    loaded = saved_model_load.load(saved_model_dir)
    input_arr_1 = np.random.random((1, 3)).astype('float32')
    input_arr_2 = np.random.random((1, 5)).astype('float32')

    outputs = loaded([input_arr_1, input_arr_2])
    self.assertAllEqual(input_arr_1, outputs[0])
    self.assertAllEqual(input_arr_2, outputs[1])

  def test_revived_sequential(self):
    model = keras.models.Sequential()
    model.add(keras.layers.Dense(5, input_shape=(3,),
                                 kernel_regularizer=regularizers.get('l2')))
    model.add(keras.layers.Dense(2, kernel_regularizer=regularizers.get('l2')))

    self.evaluate(variables.variables_initializer(model.variables))

    saved_model_dir = self._save_model_dir()
    model.save(saved_model_dir, save_format='tf')
    loaded = saved_model_load.load(saved_model_dir)

    self.assertLen(loaded.layers, 2)
    self.assertLen(loaded.losses, 2)

    loaded.pop()

    self.assertLen(loaded.layers, 1)
    self.assertLen(loaded.losses, 1)

    loaded.add(keras.layers.Dense(2, kernel_regularizer=regularizers.get('l2')))

    self.assertLen(loaded.layers, 2)
    self.assertLen(loaded.losses, 2)

if __name__ == '__main__':
  test.main()
