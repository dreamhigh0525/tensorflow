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
"""Tests for Keras Layer utils."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.python import keras
from tensorflow.python.keras.utils import vis_utils
from tensorflow.python.lib.io import file_io
from tensorflow.python.platform import test


class ModelToDotFormatTest(test.TestCase):

  def test_plot_model_cnn(self):
    model = keras.Sequential()
    model.add(
        keras.layers.Conv2D(
            filters=2, kernel_size=(2, 3), input_shape=(3, 5, 5), name='conv'))
    model.add(keras.layers.Flatten(name='flat'))
    model.add(keras.layers.Dense(5, name='dense'))
    dot_img_file = 'model_1.png'
    try:
      vis_utils.plot_model(model, to_file=dot_img_file, show_shapes=True)
      self.assertTrue(file_io.file_exists(dot_img_file))
      file_io.delete_file(dot_img_file)
    except ImportError:
      pass

  def test_plot_model_rnn(self):
    model = keras.Sequential()
    model.add(
        keras.layers.LSTM(
            16, return_sequences=True, input_shape=(2, 3), name='lstm'))
    model.add(keras.layers.TimeDistributed(keras.layers.Dense(5, name='dense')))
    dot_img_file = 'model_2.png'
    try:
      vis_utils.plot_model(model, to_file=dot_img_file, show_shapes=True)
      self.assertTrue(file_io.file_exists(dot_img_file))
      file_io.delete_file(dot_img_file)
    except ImportError:
      pass


if __name__ == '__main__':
  test.main()
