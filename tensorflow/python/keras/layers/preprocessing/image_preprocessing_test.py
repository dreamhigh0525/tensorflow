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
"""Tests for image preprocessing layers."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from absl.testing import parameterized
import numpy as np

from tensorflow.python.compat import compat
from tensorflow.python.framework import errors
from tensorflow.python.framework import test_util as tf_test_util
from tensorflow.python.keras import keras_parameterized
from tensorflow.python.keras import testing_utils
from tensorflow.python.keras.layers.preprocessing import image_preprocessing
from tensorflow.python.keras.utils.generic_utils import CustomObjectScope
from tensorflow.python.ops import gen_stateful_random_ops
from tensorflow.python.ops import image_ops_impl as image_ops
from tensorflow.python.ops import random_ops
from tensorflow.python.ops import stateless_random_ops
from tensorflow.python.platform import test


@keras_parameterized.run_all_keras_modes(always_skip_v1=True)
class ResizingTest(keras_parameterized.TestCase):

  def _run_test(self, kwargs, expected_height, expected_width):
    np.random.seed(1337)
    num_samples = 2
    orig_height = 5
    orig_width = 8
    channels = 3
    kwargs.update({'height': expected_height, 'width': expected_width})
    with tf_test_util.use_gpu():
      testing_utils.layer_test(
          image_preprocessing.Resizing,
          kwargs=kwargs,
          input_shape=(num_samples, orig_height, orig_width, channels),
          expected_output_shape=(None, expected_height, expected_width,
                                 channels))

  @parameterized.named_parameters(
      ('down_sample_bilinear_2_by_2', {'interpolation': 'bilinear'}, 2, 2),
      ('down_sample_bilinear_3_by_2', {'interpolation': 'bilinear'}, 3, 2),
      ('down_sample_nearest_2_by_2', {'interpolation': 'nearest'}, 2, 2),
      ('down_sample_nearest_3_by_2', {'interpolation': 'nearest'}, 3, 2),
      ('down_sample_area_2_by_2', {'interpolation': 'area'}, 2, 2),
      ('down_sample_area_3_by_2', {'interpolation': 'area'}, 3, 2))
  def test_down_sampling(self, kwargs, expected_height, expected_width):
    with CustomObjectScope({'Resizing': image_preprocessing.Resizing}):
      self._run_test(kwargs, expected_height, expected_width)

  @parameterized.named_parameters(
      ('up_sample_bilinear_10_by_12', {'interpolation': 'bilinear'}, 10, 12),
      ('up_sample_bilinear_12_by_12', {'interpolation': 'bilinear'}, 12, 12),
      ('up_sample_nearest_10_by_12', {'interpolation': 'nearest'}, 10, 12),
      ('up_sample_nearest_12_by_12', {'interpolation': 'nearest'}, 12, 12),
      ('up_sample_area_10_by_12', {'interpolation': 'area'}, 10, 12),
      ('up_sample_area_12_by_12', {'interpolation': 'area'}, 12, 12))
  def test_up_sampling(self, kwargs, expected_height, expected_width):
    with CustomObjectScope({'Resizing': image_preprocessing.Resizing}):
      self._run_test(kwargs, expected_height, expected_width)

  @parameterized.named_parameters(
      ('reshape_bilinear_10_by_4', {'interpolation': 'bilinear'}, 10, 4))
  def test_reshaping(self, kwargs, expected_height, expected_width):
    with CustomObjectScope({'Resizing': image_preprocessing.Resizing}):
      self._run_test(kwargs, expected_height, expected_width)

  def test_invalid_interpolation(self):
    with self.assertRaises(NotImplementedError):
      image_preprocessing.Resizing(5, 5, 'invalid_interpolation')

  def test_config_with_custom_name(self):
    layer = image_preprocessing.Resizing(5, 5, name='image_preproc')
    config = layer.get_config()
    layer_1 = image_preprocessing.Resizing.from_config(config)
    self.assertEqual(layer_1.name, layer.name)


def get_numpy_center_crop(images, expected_height, expected_width):
  orig_height = images.shape[1]
  orig_width = images.shape[2]
  height_start = int((orig_height - expected_height) / 2)
  width_start = int((orig_width - expected_width) / 2)
  height_end = height_start + expected_height
  width_end = width_start + expected_width
  return images[:, height_start:height_end, width_start:width_end, :]


@keras_parameterized.run_all_keras_modes(always_skip_v1=True)
class CenterCropTest(keras_parameterized.TestCase):

  def _run_test(self, expected_height, expected_width):
    np.random.seed(1337)
    num_samples = 2
    orig_height = 5
    orig_width = 8
    channels = 3
    kwargs = {'height': expected_height, 'width': expected_width}
    input_images = np.random.random(
        (num_samples, orig_height, orig_width, channels)).astype(np.float32)
    expected_output = get_numpy_center_crop(
        input_images, expected_height, expected_width)
    with tf_test_util.use_gpu():
      testing_utils.layer_test(
          image_preprocessing.CenterCrop,
          kwargs=kwargs,
          input_shape=(num_samples, orig_height, orig_width, channels),
          input_data=input_images,
          expected_output=expected_output,
          expected_output_shape=(None, expected_height, expected_width,
                                 channels))

  @parameterized.named_parameters(
      ('center_crop_3_by_4', 3, 4),
      ('center_crop_3_by_2', 3, 2))
  def test_center_crop_aligned(self, expected_height, expected_width):
    with CustomObjectScope({'CenterCrop': image_preprocessing.CenterCrop}):
      self._run_test(expected_height, expected_width)

  @parameterized.named_parameters(
      ('center_crop_4_by_5', 4, 5),
      ('center_crop_4_by_3', 4, 3))
  def test_center_crop_mis_aligned(self, expected_height, expected_width):
    with CustomObjectScope({'CenterCrop': image_preprocessing.CenterCrop}):
      self._run_test(expected_height, expected_width)

  @parameterized.named_parameters(
      ('center_crop_4_by_6', 4, 6),
      ('center_crop_3_by_2', 3, 2))
  def test_center_crop_half_mis_aligned(self, expected_height, expected_width):
    with CustomObjectScope({'CenterCrop': image_preprocessing.CenterCrop}):
      self._run_test(expected_height, expected_width)

  @parameterized.named_parameters(
      ('center_crop_5_by_12', 5, 12),
      ('center_crop_10_by_8', 10, 8),
      ('center_crop_10_by_12', 10, 12))
  def test_invalid_center_crop(self, expected_height, expected_width):
    with self.assertRaisesRegexp(errors.InvalidArgumentError,
                                 r'assertion failed'):
      self._run_test(expected_height, expected_width)

  def test_config_with_custom_name(self):
    layer = image_preprocessing.CenterCrop(5, 5, name='image_preproc')
    config = layer.get_config()
    layer_1 = image_preprocessing.CenterCrop.from_config(config)
    self.assertEqual(layer_1.name, layer.name)


@keras_parameterized.run_all_keras_modes(always_skip_v1=True)
class RandomCropTest(keras_parameterized.TestCase):

  def _run_test(self, expected_height, expected_width):
    np.random.seed(1337)
    num_samples = 2
    orig_height = 5
    orig_width = 8
    channels = 3
    kwargs = {'height': expected_height, 'width': expected_width}
    with tf_test_util.use_gpu():
      testing_utils.layer_test(
          image_preprocessing.RandomCrop,
          kwargs=kwargs,
          input_shape=(num_samples, orig_height, orig_width, channels),
          expected_output_shape=(None, expected_height, expected_width,
                                 channels))

  @parameterized.named_parameters(
      ('random_crop_5_by_12', 5, 12),
      ('random_crop_10_by_8', 10, 8),
      ('random_crop_10_by_12', 10, 12))
  def test_invalid_random_crop(self, expected_height, expected_width):
    with self.assertRaises(errors.InvalidArgumentError):
      with CustomObjectScope({'RandomCrop': image_preprocessing.RandomCrop}):
        self._run_test(expected_height, expected_width)

  def test_training_with_mock(self):
    if test.is_built_with_rocm():
      # TODO(rocm):
      # re-enable this test once ROCm adds support for
      # the StatefulUniformFullInt Op (on the GPU)
      self.skipTest('Feature not supported on ROCm')
    np.random.seed(1337)
    height, width = 3, 4
    height_offset = np.random.randint(low=0, high=3)
    width_offset = np.random.randint(low=0, high=5)
    mock_offset = [0, height_offset, width_offset, 0]
    with test.mock.patch.object(
        stateless_random_ops, 'stateless_random_uniform',
        return_value=mock_offset):
      with tf_test_util.use_gpu():
        layer = image_preprocessing.RandomCrop(height, width)
        inp = np.random.random((12, 5, 8, 3))
        actual_output = layer(inp, training=1)
        expected_output = inp[:, height_offset:(height_offset + height),
                              width_offset:(width_offset + width), :]
        self.assertAllClose(expected_output, actual_output)

  @parameterized.named_parameters(
      ('random_crop_4_by_6', 4, 6),
      ('random_crop_3_by_2', 3, 2))
  def test_random_crop_output_shape(self, expected_height, expected_width):
    if test.is_built_with_rocm():
      # TODO(rocm):
      # re-enable this test once ROCm adds support for
      # the StatefulUniformFullInt Op (on the GPU)
      self.skipTest('Feature not supported on ROCm')
    with CustomObjectScope({'RandomCrop': image_preprocessing.RandomCrop}):
      self._run_test(expected_height, expected_width)

  def test_predicting_with_mock_longer_height(self):
    np.random.seed(1337)
    height, width = 3, 3
    inp = np.random.random((12, 10, 6, 3))
    with tf_test_util.use_gpu():
      layer = image_preprocessing.RandomCrop(height, width)
      actual_output = layer(inp, training=0)
      resized_inp = image_ops.resize_images_v2(
          inp, size=[5, 3])
      expected_output = resized_inp[:, 1:4, :, :]
      self.assertAllClose(expected_output, actual_output)

  def test_predicting_with_mock_longer_width(self):
    np.random.seed(1337)
    height, width = 4, 6
    inp = np.random.random((12, 8, 16, 3))
    with tf_test_util.use_gpu():
      layer = image_preprocessing.RandomCrop(height, width)
      actual_output = layer(inp, training=0)
      resized_inp = image_ops.resize_images_v2(
          inp, size=[4, 8])
      expected_output = resized_inp[:, :, 1:7, :]
      self.assertAllClose(expected_output, actual_output)

  def test_config_with_custom_name(self):
    layer = image_preprocessing.RandomCrop(5, 5, name='image_preproc')
    config = layer.get_config()
    layer_1 = image_preprocessing.RandomCrop.from_config(config)
    self.assertEqual(layer_1.name, layer.name)


class RescalingTest(keras_parameterized.TestCase):

  @keras_parameterized.run_all_keras_modes(always_skip_v1=True)
  def test_rescaling_base(self):
    kwargs = {'scale': 0.004}
    testing_utils.layer_test(
        image_preprocessing.Rescaling,
        kwargs=kwargs,
        input_shape=(2, 5, 6, 3),
        expected_output_shape=(None, 5, 6, 3))

  @tf_test_util.run_v2_only
  def test_rescaling_correctness_float(self):
    layer = image_preprocessing.Rescaling(0.004)
    inputs = random_ops.random_uniform((2, 4, 5, 3))
    outputs = layer(inputs)
    self.assertAllClose(outputs.numpy(), inputs.numpy() * 0.004)

  @tf_test_util.run_v2_only
  def test_rescaling_correctness_int(self):
    layer = image_preprocessing.Rescaling(0.004)
    inputs = random_ops.random_uniform((2, 4, 5, 3), 0, 100, dtype='int32')
    outputs = layer(inputs)
    self.assertEqual(outputs.dtype.name, 'float32')
    self.assertAllClose(outputs.numpy(), inputs.numpy() * 0.004)

  def test_config_with_custom_name(self):
    layer = image_preprocessing.Rescaling(0.5, name='rescaling')
    config = layer.get_config()
    layer_1 = image_preprocessing.Rescaling.from_config(config)
    self.assertEqual(layer_1.name, layer.name)


@keras_parameterized.run_all_keras_modes(always_skip_v1=True)
class RandomFlipTest(keras_parameterized.TestCase):

  def _run_test(self, mode, expected_output=None, mock_random=None):
    np.random.seed(1337)
    num_samples = 2
    orig_height = 5
    orig_width = 8
    channels = 3
    if mock_random is None:
      mock_random = [1 for _ in range(num_samples)]
      mock_random = np.reshape(mock_random, [2, 1, 1, 1])
    inp = np.random.random((num_samples, orig_height, orig_width, channels))
    if expected_output is None:
      expected_output = inp
      if mode == 'horizontal' or mode == 'horizontal_and_vertical':
        expected_output = np.flip(expected_output, axis=2)
      if mode == 'vertical' or mode == 'horizontal_and_vertical':
        expected_output = np.flip(expected_output, axis=1)
    with test.mock.patch.object(
        random_ops, 'random_uniform', return_value=mock_random):
      with tf_test_util.use_gpu():
        layer = image_preprocessing.RandomFlip(mode)
        actual_output = layer(inp, training=1)
        self.assertAllClose(expected_output, actual_output)

  @parameterized.named_parameters(
      ('random_flip_horizontal', 'horizontal'),
      ('random_flip_vertical', 'vertical'),
      ('random_flip_both', 'horizontal_and_vertical'))
  def test_random_flip(self, mode):
    with CustomObjectScope({'RandomFlip': image_preprocessing.RandomFlip}):
      self._run_test(mode)

  def test_random_flip_horizontal_half(self):
    with CustomObjectScope({'RandomFlip': image_preprocessing.RandomFlip}):
      np.random.seed(1337)
      mock_random = [1, 0]
      mock_random = np.reshape(mock_random, [2, 1, 1, 1])
      input_images = np.random.random((2, 5, 8, 3)).astype(np.float32)
      expected_output = input_images.copy()
      expected_output[0, :, :, :] = np.flip(input_images[0, :, :, :], axis=1)
      self._run_test('horizontal', expected_output, mock_random)

  def test_random_flip_vertical_half(self):
    with CustomObjectScope({'RandomFlip': image_preprocessing.RandomFlip}):
      np.random.seed(1337)
      mock_random = [1, 0]
      mock_random = np.reshape(mock_random, [2, 1, 1, 1])
      input_images = np.random.random((2, 5, 8, 3)).astype(np.float32)
      expected_output = input_images.copy()
      expected_output[0, :, :, :] = np.flip(input_images[0, :, :, :], axis=0)
      self._run_test('vertical', expected_output, mock_random)

  def test_random_flip_inference(self):
    with CustomObjectScope({'RandomFlip': image_preprocessing.RandomFlip}):
      input_images = np.random.random((2, 5, 8, 3)).astype(np.float32)
      expected_output = input_images
      with tf_test_util.use_gpu():
        layer = image_preprocessing.RandomFlip()
        actual_output = layer(input_images, training=0)
        self.assertAllClose(expected_output, actual_output)

  def test_random_flip_default(self):
    with CustomObjectScope({'RandomFlip': image_preprocessing.RandomFlip}):
      input_images = np.random.random((2, 5, 8, 3)).astype(np.float32)
      expected_output = np.flip(np.flip(input_images, axis=1), axis=2)
      mock_random = [1, 1]
      mock_random = np.reshape(mock_random, [2, 1, 1, 1])
      with test.mock.patch.object(
          random_ops, 'random_uniform', return_value=mock_random):
        with self.cached_session(use_gpu=True):
          layer = image_preprocessing.RandomFlip()
          actual_output = layer(input_images, training=1)
          self.assertAllClose(expected_output, actual_output)

  @tf_test_util.run_v2_only
  def test_config_with_custom_name(self):
    layer = image_preprocessing.RandomFlip(name='image_preproc')
    config = layer.get_config()
    layer_1 = image_preprocessing.RandomFlip.from_config(config)
    self.assertEqual(layer_1.name, layer.name)


@keras_parameterized.run_all_keras_modes(always_skip_v1=True)
class RandomContrastTest(keras_parameterized.TestCase):

  def _run_test(self,
                lower,
                upper,
                expected_output=None,
                mock_random=None):
    np.random.seed(1337)
    num_samples = 2
    orig_height = 5
    orig_width = 8
    channels = 3
    if mock_random is None:
      mock_random = 0.2
    inp = np.random.random((num_samples, orig_height, orig_width, channels))
    if expected_output is None:
      # reduce mean on height.
      inp_mean = np.mean(inp, axis=1, keepdims=True)
      # reduce mean on width.
      inp_mean = np.mean(inp_mean, axis=2, keepdims=True)
      expected_output = (inp - inp_mean) * mock_random + inp_mean
    with test.mock.patch.object(
        random_ops, 'random_uniform', return_value=mock_random):
      with tf_test_util.use_gpu():
        layer = image_preprocessing.RandomContrast((lower, upper))
        actual_output = layer(inp, training=True)
        self.assertAllClose(expected_output, actual_output)

  @parameterized.named_parameters(
      ('random_contrast_2_by_5', 0.2, 0.5),
      ('random_contrast_2_by_13', 0.2, 1.3),
      ('random_contrast_5_by_2', 0.5, 0.2))
  def test_random_contrast(self, lower, upper):
    with CustomObjectScope(
        {'RandomContrast': image_preprocessing.RandomContrast}):
      self._run_test(lower, upper)

  @parameterized.named_parameters(
      ('random_contrast_amplitude_2', 0.2),
      ('random_contrast_amplitude_5', 0.5))
  def test_random_contrast_amplitude(self, amplitude):
    with CustomObjectScope(
        {'RandomContrast': image_preprocessing.RandomContrast}):
      input_images = np.random.random((2, 5, 8, 3))
      with tf_test_util.use_gpu():
        layer = image_preprocessing.RandomContrast(amplitude)
        layer(input_images)

  def test_random_contrast_inference(self):
    with CustomObjectScope(
        {'RandomContrast': image_preprocessing.RandomContrast}):
      input_images = np.random.random((2, 5, 8, 3)).astype(np.float32)
      expected_output = input_images
      with tf_test_util.use_gpu():
        layer = image_preprocessing.RandomContrast((0.1, 0.2))
        actual_output = layer(input_images, training=False)
        self.assertAllClose(expected_output, actual_output)

  def test_random_contrast_int_dtype(self):
    with CustomObjectScope(
        {'RandomContrast': image_preprocessing.RandomContrast}):
      input_images = np.random.randint(low=0, high=255, size=(2, 5, 8, 3))
      with tf_test_util.use_gpu():
        layer = image_preprocessing.RandomContrast((0.1, 0.2))
        layer(input_images)

  def test_random_contrast_invalid_bounds(self):
    with self.assertRaises(ValueError):
      image_preprocessing.RandomContrast((-0.1, .5))

    with self.assertRaises(ValueError):
      image_preprocessing.RandomContrast((1.1, .5))

    with self.assertRaises(ValueError):
      image_preprocessing.RandomContrast((0.1, -0.2))

  @tf_test_util.run_v2_only
  def test_config_with_custom_name(self):
    layer = image_preprocessing.RandomContrast((.5, .6), name='image_preproc')
    config = layer.get_config()
    layer_1 = image_preprocessing.RandomContrast.from_config(config)
    self.assertEqual(layer_1.name, layer.name)


@keras_parameterized.run_all_keras_modes(always_skip_v1=True)
class RandomTranslationTest(keras_parameterized.TestCase):

  def _run_test(self, height_factor, width_factor):
    np.random.seed(1337)
    num_samples = 2
    orig_height = 5
    orig_width = 8
    channels = 3
    kwargs = {'height_factor': height_factor, 'width_factor': width_factor}
    with tf_test_util.use_gpu():
      testing_utils.layer_test(
          image_preprocessing.RandomTranslation,
          kwargs=kwargs,
          input_shape=(num_samples, orig_height, orig_width, channels),
          expected_output_shape=(None, orig_height, orig_width, channels))

  @parameterized.named_parameters(
      ('random_translate_4_by_6', .4, .6), ('random_translate_3_by_2', .3, .2),
      ('random_translate_tuple_factor', (.5, .4), (.2, .3)))
  def test_random_translation(self, height_factor, width_factor):
    self._run_test(height_factor, width_factor)

  def test_random_translation_negative_lower(self):
    mock_offset = np.random.random((12, 1))
    with test.mock.patch.object(
        gen_stateful_random_ops, 'stateful_uniform', return_value=mock_offset):
      with self.cached_session(use_gpu=True):
        layer = image_preprocessing.RandomTranslation((-0.2, .3), .4)
        layer_2 = image_preprocessing.RandomTranslation((0.2, .3), .4)
        inp = np.random.random((12, 5, 8, 3)).astype(np.float32)
        actual_output = layer(inp, training=1)
        actual_output_2 = layer_2(inp, training=1)
        self.assertAllClose(actual_output, actual_output_2)

  def test_random_translation_inference(self):
    with CustomObjectScope(
        {'RandomTranslation': image_preprocessing.RandomTranslation}):
      input_images = np.random.random((2, 5, 8, 3)).astype(np.float32)
      expected_output = input_images
      with tf_test_util.use_gpu():
        layer = image_preprocessing.RandomTranslation(.5, .5)
        actual_output = layer(input_images, training=0)
        self.assertAllClose(expected_output, actual_output)

  @tf_test_util.run_v2_only
  def test_config_with_custom_name(self):
    layer = image_preprocessing.RandomTranslation(.5, .6, name='image_preproc')
    config = layer.get_config()
    layer_1 = image_preprocessing.RandomTranslation.from_config(config)
    self.assertEqual(layer_1.name, layer.name)


@keras_parameterized.run_all_keras_modes(always_skip_v1=True)
class RandomTransformTest(keras_parameterized.TestCase):

  def _run_random_transform_with_mock(self,
                                      transform_matrix,
                                      expected_output,
                                      mode,
                                      interpolation='bilinear'):
    inp = np.arange(15).reshape((1, 5, 3, 1)).astype(np.float32)
    with self.cached_session(use_gpu=True):
      output = image_preprocessing.transform(
          inp, transform_matrix, fill_mode=mode, interpolation=interpolation)
    self.assertAllClose(expected_output, output)

  def test_random_translation_reflect(self):
    # reflected output is (dcba|abcd|dcba)

    # Test down shift by 1.
    # pyformat: disable
    expected_output = np.asarray(
        [[0., 1., 2.],
         [0., 1., 2.],
         [3., 4., 5.],
         [6., 7., 8],
         [9., 10., 11]]).reshape((1, 5, 3, 1)).astype(np.float32)
    # pyformat: enable
    transform_matrix = np.asarray([[1., 0., 0., 0., 1., -1., 0., 0.]])
    self._run_random_transform_with_mock(transform_matrix, expected_output,
                                         'reflect')

    # Test up shift by 1.
    # pyformat: disable
    expected_output = np.asarray(
        [[3., 4., 5.],
         [6., 7., 8],
         [9., 10., 11.],
         [12., 13., 14.],
         [12., 13., 14.]]).reshape((1, 5, 3, 1)).astype(np.float32)
    # pyformat: enable
    transform_matrix = np.asarray([[1., 0., 0., 0., 1., 1., 0., 0.]])
    self._run_random_transform_with_mock(transform_matrix, expected_output,
                                         'reflect')

    # Test left shift by 1.
    # reflected output is (dcba|abcd|dcba)
    # pyformat: disable
    expected_output = np.asarray(
        [[1., 2., 2.],
         [4., 5., 5.],
         [7., 8., 8.],
         [10., 11., 11.],
         [13., 14., 14.]]).reshape((1, 5, 3, 1)).astype(np.float32)
    # pyformat: enable
    transform_matrix = np.asarray([[1., 0., 1., 0., 1., 0., 0., 0.]])
    self._run_random_transform_with_mock(transform_matrix, expected_output,
                                         'reflect')

    # Test right shift by 1.
    # pyformat: disable
    expected_output = np.asarray(
        [[0., 0., 1.],
         [3., 3., 4],
         [6., 6., 7.],
         [9., 9., 10.],
         [12., 12., 13.]]).reshape((1, 5, 3, 1)).astype(np.float32)
    # pyformat: enable
    transform_matrix = np.asarray([[1., 0., -1., 0., 1., 0., 0., 0.]])
    self._run_random_transform_with_mock(transform_matrix, expected_output,
                                         'reflect')

  def test_random_translation_wrap(self):
    # warpped output is (abcd|abcd|abcd)

    # Test down shift by 1.
    # pyformat: disable
    expected_output = np.asarray(
        [[12., 13., 14.],
         [0., 1., 2.],
         [3., 4., 5.],
         [6., 7., 8],
         [9., 10., 11]]).reshape((1, 5, 3, 1)).astype(np.float32)
    # pyformat: enable
    transform_matrix = np.asarray([[1., 0., 0., 0., 1., -1., 0., 0.]])
    self._run_random_transform_with_mock(transform_matrix, expected_output,
                                         'wrap')

    # Test up shift by 1.
    # pyformat: disable
    expected_output = np.asarray(
        [[3., 4., 5.],
         [6., 7., 8],
         [9., 10., 11.],
         [12., 13., 14.],
         [0., 1., 2.]]).reshape((1, 5, 3, 1)).astype(np.float32)
    # pyformat: enable
    transform_matrix = np.asarray([[1., 0., 0., 0., 1., 1., 0., 0.]])
    self._run_random_transform_with_mock(transform_matrix, expected_output,
                                         'wrap')

    # Test left shift by 1.
    # pyformat: disable
    expected_output = np.asarray(
        [[1., 2., 0.],
         [4., 5., 3.],
         [7., 8., 6.],
         [10., 11., 9.],
         [13., 14., 12.]]).reshape((1, 5, 3, 1)).astype(np.float32)
    # pyformat: enable
    transform_matrix = np.asarray([[1., 0., 1., 0., 1., 0., 0., 0.]])
    self._run_random_transform_with_mock(transform_matrix, expected_output,
                                         'wrap')

    # Test right shift by 1.
    # pyformat: disable
    expected_output = np.asarray(
        [[2., 0., 1.],
         [5., 3., 4],
         [8., 6., 7.],
         [11., 9., 10.],
         [14., 12., 13.]]).reshape((1, 5, 3, 1)).astype(np.float32)
    # pyformat: enable
    transform_matrix = np.asarray([[1., 0., -1., 0., 1., 0., 0., 0.]])
    self._run_random_transform_with_mock(transform_matrix, expected_output,
                                         'wrap')

  def test_random_translation_constant(self):
    # constant output is (0000|abcd|0000)

    # Test down shift by 1.
    # pyformat: disable
    expected_output = np.asarray(
        [[0., 0., 0.],
         [0., 1., 2.],
         [3., 4., 5.],
         [6., 7., 8],
         [9., 10., 11]]).reshape((1, 5, 3, 1)).astype(np.float32)
    # pyformat: enable
    transform_matrix = np.asarray([[1., 0., 0., 0., 1., -1., 0., 0.]])
    self._run_random_transform_with_mock(transform_matrix, expected_output,
                                         'constant')

    # Test up shift by 1.
    # pyformat: disable
    expected_output = np.asarray(
        [[3., 4., 5.],
         [6., 7., 8],
         [9., 10., 11.],
         [12., 13., 14.],
         [0., 0., 0.]]).reshape((1, 5, 3, 1)).astype(np.float32)
    # pyformat: enable
    transform_matrix = np.asarray([[1., 0., 0., 0., 1., 1., 0., 0.]])
    self._run_random_transform_with_mock(transform_matrix, expected_output,
                                         'constant')

    # Test left shift by 1.
    # pyformat: disable
    expected_output = np.asarray(
        [[1., 2., 0.],
         [4., 5., 0.],
         [7., 8., 0.],
         [10., 11., 0.],
         [13., 14., 0.]]).reshape((1, 5, 3, 1)).astype(np.float32)
    # pyformat: enable
    transform_matrix = np.asarray([[1., 0., 1., 0., 1., 0., 0., 0.]])
    self._run_random_transform_with_mock(transform_matrix, expected_output,
                                         'constant')

    # Test right shift by 1.
    # pyformat: disable
    expected_output = np.asarray(
        [[0., 0., 1.],
         [0., 3., 4],
         [0., 6., 7.],
         [0., 9., 10.],
         [0., 12., 13.]]).reshape((1, 5, 3, 1)).astype(np.float32)
    # pyformat: enable
    transform_matrix = np.asarray([[1., 0., -1., 0., 1., 0., 0., 0.]])
    self._run_random_transform_with_mock(transform_matrix, expected_output,
                                         'constant')

  def test_random_translation_nearest_interpolation(self):
    # nearest output is (aaaa|abcd|dddd)

    # Test down shift by 1.
    # pyformat: disable
    expected_output = np.asarray(
        [[0., 0., 0.],
         [0., 1., 2.],
         [3., 4., 5.],
         [6., 7., 8],
         [9., 10., 11]]).reshape((1, 5, 3, 1)).astype(np.float32)
    # pyformat: enable
    transform_matrix = np.asarray([[1., 0., 0., 0., 1., -1., 0., 0.]])
    self._run_random_transform_with_mock(
        transform_matrix, expected_output,
        mode='constant', interpolation='nearest')

    # Test up shift by 1.
    # pyformat: disable
    expected_output = np.asarray(
        [[3., 4., 5.],
         [6., 7., 8],
         [9., 10., 11.],
         [12., 13., 14.],
         [0., 0., 0.]]).reshape((1, 5, 3, 1)).astype(np.float32)
    # pyformat: enable
    transform_matrix = np.asarray([[1., 0., 0., 0., 1., 1., 0., 0.]])
    self._run_random_transform_with_mock(
        transform_matrix, expected_output,
        mode='constant', interpolation='nearest')

    # Test left shift by 1.
    # pyformat: disable
    expected_output = np.asarray(
        [[1., 2., 0.],
         [4., 5., 0.],
         [7., 8., 0.],
         [10., 11., 0.],
         [13., 14., 0.]]).reshape((1, 5, 3, 1)).astype(np.float32)
    # pyformat: enable
    transform_matrix = np.asarray([[1., 0., 1., 0., 1., 0., 0., 0.]])
    self._run_random_transform_with_mock(
        transform_matrix, expected_output,
        mode='constant', interpolation='nearest')

    # Test right shift by 1.
    # pyformat: disable
    expected_output = np.asarray(
        [[0., 0., 1.],
         [0., 3., 4],
         [0., 6., 7.],
         [0., 9., 10.],
         [0., 12., 13.]]).reshape((1, 5, 3, 1)).astype(np.float32)
    # pyformat: enable
    transform_matrix = np.asarray([[1., 0., -1., 0., 1., 0., 0., 0.]])
    self._run_random_transform_with_mock(
        transform_matrix, expected_output,
        mode='constant', interpolation='nearest')


@keras_parameterized.run_all_keras_modes(always_skip_v1=True)
class RandomRotationTest(keras_parameterized.TestCase):

  def _run_test(self, factor):
    np.random.seed(1337)
    num_samples = 2
    orig_height = 5
    orig_width = 8
    channels = 3
    kwargs = {'factor': factor}
    with tf_test_util.use_gpu():
      testing_utils.layer_test(
          image_preprocessing.RandomRotation,
          kwargs=kwargs,
          input_shape=(num_samples, orig_height, orig_width, channels),
          expected_output_shape=(None, orig_height, orig_width, channels))

  @parameterized.named_parameters(('random_rotate_4', .4),
                                  ('random_rotate_3', .3),
                                  ('random_rotate_tuple_factor', (.5, .4)))
  def test_random_rotation(self, factor):
    self._run_test(factor)

  def test_random_rotation_inference(self):
    with CustomObjectScope(
        {'RandomTranslation': image_preprocessing.RandomRotation}):
      input_images = np.random.random((2, 5, 8, 3)).astype(np.float32)
      expected_output = input_images
      with tf_test_util.use_gpu():
        layer = image_preprocessing.RandomRotation(.5)
        actual_output = layer(input_images, training=0)
        self.assertAllClose(expected_output, actual_output)

  @tf_test_util.run_v2_only
  def test_config_with_custom_name(self):
    layer = image_preprocessing.RandomRotation(.5, name='image_preproc')
    config = layer.get_config()
    layer_1 = image_preprocessing.RandomRotation.from_config(config)
    self.assertEqual(layer_1.name, layer.name)


@keras_parameterized.run_all_keras_modes(always_skip_v1=True)
class RandomZoomTest(keras_parameterized.TestCase):

  def _run_test(self, height_factor, width_factor):
    np.random.seed(1337)
    num_samples = 2
    orig_height = 5
    orig_width = 8
    channels = 3
    kwargs = {'height_factor': height_factor, 'width_factor': width_factor}
    with tf_test_util.use_gpu():
      testing_utils.layer_test(
          image_preprocessing.RandomZoom,
          kwargs=kwargs,
          input_shape=(num_samples, orig_height, orig_width, channels),
          expected_output_shape=(None, orig_height, orig_width, channels))

  @parameterized.named_parameters(
      ('random_zoom_4_by_6', .4, .6), ('random_zoom_2_by_3', .2, .3),
      ('random_zoom_tuple_factor', (.4, .5), (.2, .3)))
  def test_random_zoom_in(self, height_factor, width_factor):
    self._run_test(height_factor, width_factor)

  @parameterized.named_parameters(
      ('random_zoom_4_by_6', 1.4, 1.6), ('random_zoom_2_by_3', 1.2, 1.3),
      ('random_zoom_tuple_factor', (1.4, 1.5), (1.2, 1.3)))
  def test_random_zoom_out(self, height_factor, width_factor):
    self._run_test(height_factor, width_factor)

  def test_random_zoom_invalid_factor(self):
    with self.assertRaises(ValueError):
      image_preprocessing.RandomZoom((.5, .4), .2)
    with self.assertRaises(ValueError):
      image_preprocessing.RandomZoom(.2, (.5, .4))

  def test_random_zoom_inference(self):
    with CustomObjectScope(
        {'RandomZoom': image_preprocessing.RandomZoom}):
      input_images = np.random.random((2, 5, 8, 3)).astype(np.float32)
      expected_output = input_images
      with tf_test_util.use_gpu():
        layer = image_preprocessing.RandomZoom(.5, .5)
        actual_output = layer(input_images, training=0)
        self.assertAllClose(expected_output, actual_output)

  @tf_test_util.run_v2_only
  def test_config_with_custom_name(self):
    layer = image_preprocessing.RandomZoom(.5, .6, name='image_preproc')
    config = layer.get_config()
    layer_1 = image_preprocessing.RandomZoom.from_config(config)
    self.assertEqual(layer_1.name, layer.name)


@keras_parameterized.run_all_keras_modes(always_skip_v1=True)
class RandomHeightTest(keras_parameterized.TestCase):

  def _run_test(self, factor):
    np.random.seed(1337)
    num_samples = 2
    orig_height = 5
    orig_width = 8
    channels = 3
    with tf_test_util.use_gpu():
      img = np.random.random((num_samples, orig_height, orig_width, channels))
      layer = image_preprocessing.RandomHeight(factor)
      img_out = layer(img, training=True)
      self.assertEqual(img_out.shape[0], 2)
      self.assertEqual(img_out.shape[2], 8)
      self.assertEqual(img_out.shape[3], 3)

  @parameterized.named_parameters(('random_height_4_by_6', (.4, .6)),
                                  ('random_height_3_by_2', (.3, 1.2)),
                                  ('random_height_3', .3))
  def test_random_height_basic(self, factor):
    self._run_test(factor)

  def test_valid_random_height(self):
    # need (maxval - minval) * rnd + minval = 0.6
    mock_factor = 0
    with test.mock.patch.object(
        gen_stateful_random_ops, 'stateful_uniform', return_value=mock_factor):
      with tf_test_util.use_gpu():
        img = np.random.random((12, 5, 8, 3))
        layer = image_preprocessing.RandomHeight(.4)
        img_out = layer(img, training=True)
        self.assertEqual(img_out.shape[1], 3)

  def test_random_height_invalid_factor(self):
    with self.assertRaises(ValueError):
      image_preprocessing.RandomHeight((-1.5, .4))

  def test_random_height_inference(self):
    with CustomObjectScope({'RandomHeight': image_preprocessing.RandomHeight}):
      input_images = np.random.random((2, 5, 8, 3)).astype(np.float32)
      expected_output = input_images
      with tf_test_util.use_gpu():
        layer = image_preprocessing.RandomHeight(.5)
        actual_output = layer(input_images, training=0)
        self.assertAllClose(expected_output, actual_output)

  @tf_test_util.run_v2_only
  def test_config_with_custom_name(self):
    layer = image_preprocessing.RandomHeight(.5, name='image_preproc')
    config = layer.get_config()
    layer_1 = image_preprocessing.RandomHeight.from_config(config)
    self.assertEqual(layer_1.name, layer.name)


@keras_parameterized.run_all_keras_modes(always_skip_v1=True)
class RandomWidthTest(keras_parameterized.TestCase):

  def _run_test(self, factor):
    np.random.seed(1337)
    num_samples = 2
    orig_height = 5
    orig_width = 8
    channels = 3
    with tf_test_util.use_gpu():
      img = np.random.random((num_samples, orig_height, orig_width, channels))
      layer = image_preprocessing.RandomWidth(factor)
      img_out = layer(img, training=True)
      self.assertEqual(img_out.shape[0], 2)
      self.assertEqual(img_out.shape[1], 5)
      self.assertEqual(img_out.shape[3], 3)

  @parameterized.named_parameters(('random_width_4_by_6', (.4, .6)),
                                  ('random_width_3_by_2', (.3, 1.2)),
                                  ('random_width_3', .3))
  def test_random_width_basic(self, factor):
    self._run_test(factor)

  def test_valid_random_width(self):
    # need (maxval - minval) * rnd + minval = 0.6
    mock_factor = 0
    with test.mock.patch.object(
        gen_stateful_random_ops, 'stateful_uniform', return_value=mock_factor):
      with tf_test_util.use_gpu():
        img = np.random.random((12, 8, 5, 3))
        layer = image_preprocessing.RandomWidth(.4)
        img_out = layer(img, training=True)
        self.assertEqual(img_out.shape[2], 3)

  def test_random_width_invalid_factor(self):
    with self.assertRaises(ValueError):
      image_preprocessing.RandomWidth((-1.5, .4))

  def test_random_width_inference(self):
    with CustomObjectScope({'RandomWidth': image_preprocessing.RandomWidth}):
      input_images = np.random.random((2, 5, 8, 3)).astype(np.float32)
      expected_output = input_images
      with tf_test_util.use_gpu():
        layer = image_preprocessing.RandomWidth(.5)
        actual_output = layer(input_images, training=0)
        self.assertAllClose(expected_output, actual_output)

  @tf_test_util.run_v2_only
  def test_config_with_custom_name(self):
    layer = image_preprocessing.RandomWidth(.5, name='image_preproc')
    config = layer.get_config()
    layer_1 = image_preprocessing.RandomWidth.from_config(config)
    self.assertEqual(layer_1.name, layer.name)


if __name__ == '__main__':
  test.main()
