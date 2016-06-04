# Copyright 2016 Google Inc. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================
"""Tests for slim.data.tfexample_decoder."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function


import numpy as np
import tensorflow as tf

import tensorflow.contrib.slim as slim


class TFExampleDecoderTest(tf.test.TestCase):

  def _EncodedFloatFeature(self, ndarray):
    return tf.train.Feature(float_list=tf.train.FloatList(
        value=ndarray.flatten().tolist()))

  def _EncodedBytesFeature(self, tf_encoded):
    with self.test_session():
      encoded = tf_encoded.eval()

    def BytesList(value):
      return tf.train.BytesList(value=[value])

    return tf.train.Feature(bytes_list=BytesList(encoded))

  def _BytesFeature(self, ndarray):
    values = ndarray.flatten().tolist()
    for i in range(len(values)):
      values[i] = values[i].encode('utf-8')
    return tf.train.Feature(bytes_list=tf.train.BytesList(value=values))

  def _StringFeature(self, value):
    value = value.encode('utf-8')
    return tf.train.Feature(bytes_list=tf.train.BytesList(value=[value]))

  def _Encoder(self, image, image_format):
    assert image_format  in ['jpeg', 'png']
    if image_format == 'jpeg':
      tf_image = tf.constant(image, dtype=tf.uint8)
      return tf.image.encode_jpeg(tf_image)
    if image_format == 'png':
      tf_image = tf.constant(image, dtype=tf.uint8)
      return tf.image.encode_png(tf_image)

  def GenerateImage(self, image_format, image_shape):
    """Generates an image and an example containing the encoded image.

    Args:
      image_format: the encoding format of the image.
      image_shape: the shape of the image to generate.

    Returns:
      image: the generated image.
      example: a TF-example with a feature key 'image/encoded' set to the
        serialized image and a feature key 'image/format' set to the image
        encoding format ['jpeg', 'png'].
    """
    image = np.linspace(0, 17, num=18).reshape(image_shape).astype(np.uint8)
    tf_encoded = self._Encoder(image, image_format)
    example = tf.train.Example(features=tf.train.Features(feature={
        'image/encoded': self._EncodedBytesFeature(tf_encoded),
        'image/format': self._StringFeature(image_format)
    }))

    return image, example.SerializeToString()

  def DecodeExample(self, serialized_example, item_handler, image_shape,
                    image_format):
    """Decodes the given serialized example with the specified item handler.

    Args:
      serialized_example: a serialized TF example string.
      item_handler: the item handler used to decode the image.
      image_shape: the shape of the image being decoded.
      image_format: the image format being decoded.

    Returns:
      the decoded image found in the serialized Example.
    """
    with self.test_session():
      serialized_example = tf.reshape(serialized_example, shape=[])
      decoder = slim.tfexample_decoder.TFExampleDecoder(
          keys_to_features={
              'image/encoded': tf.FixedLenFeature(
                  (), tf.string, default_value=''),
              'image/format': tf.FixedLenFeature(
                  (), tf.string, default_value=image_format),
          },
          items_to_handlers={'image': item_handler}
      )
      [tf_image] = decoder.decode(serialized_example, ['image'])
      decoded_image = tf_image.eval()

    # We need to recast them here to avoid some issues with uint8.
    return decoded_image.astype(np.float32)

  def testDecodeExampleWithJpegEncoding(self):
    image_shape = (2, 3, 3)
    image, serialized_example = self.GenerateImage(
        image_format='jpeg',
        image_shape=image_shape)

    decoded_image = self.DecodeExample(
        serialized_example,
        slim.tfexample_decoder.Image(),
        image_shape=image_shape,
        image_format='jpeg')

    # Need to use a tolerance of 1 because of noise in the jpeg encode/decode
    self.assertAllClose(image, decoded_image, atol=1.001)

  def testDecodeExampleWithPngEncoding(self):
    image_shape = (2, 3, 3)
    image, serialized_example = self.GenerateImage(
        image_format='png',
        image_shape=image_shape)

    decoded_image = self.DecodeExample(
        serialized_example,
        slim.tfexample_decoder.Image(),
        image_shape=image_shape,
        image_format='png')

    self.assertAllClose(image, decoded_image, atol=0)

  def testDecodeExampleWithStringTensor(self):
    tensor_shape = (2, 3, 1)
    np_array = np.array([[['ab'], ['cd'], ['ef']],
                         [['ghi'], ['jkl'], ['mnop']]])

    example = tf.train.Example(features=tf.train.Features(feature={
        'labels': self._BytesFeature(np_array),
    }))

    serialized_example = example.SerializeToString()

    with self.test_session():
      serialized_example = tf.reshape(serialized_example, shape=[])
      keys_to_features = {
          'labels': tf.FixedLenFeature(
              tensor_shape, tf.string, default_value=tf.constant(
                  '', shape=tensor_shape, dtype=tf.string))
      }
      items_to_handlers = {
          'labels': slim.tfexample_decoder.Tensor('labels'),
      }
      decoder = slim.tfexample_decoder.TFExampleDecoder(
          keys_to_features, items_to_handlers)
      [tf_labels] = decoder.decode(serialized_example, ['labels'])
      labels = tf_labels.eval()

      labels = labels.astype(np_array.dtype)
      self.assertTrue(np.array_equal(np_array, labels))

  def testDecodeExampleWithTensor(self):
    tensor_shape = (2, 3, 1)
    np_array = np.random.rand(2, 3, 1)

    example = tf.train.Example(features=tf.train.Features(feature={
        'image/depth_map': self._EncodedFloatFeature(np_array),
    }))

    serialized_example = example.SerializeToString()

    with self.test_session():
      serialized_example = tf.reshape(serialized_example, shape=[])

      keys_to_features = {
          'image/depth_map': tf.FixedLenFeature(
              tensor_shape, tf.float32, default_value=tf.zeros(tensor_shape))
      }

      items_to_handlers = {
          'depth': slim.tfexample_decoder.Tensor(
              'image/depth_map')
      }

      decoder = slim.tfexample_decoder.TFExampleDecoder(
          keys_to_features, items_to_handlers)
      [tf_depth] = decoder.decode(serialized_example, ['depth'])
      depth = tf_depth.eval()

    self.assertTrue(np.isclose(np_array, depth).all())

  def testDecodeExampleWithItemHandlerCallback(self):
    tensor_shape = (2, 3, 1)
    np_array = np.random.rand(2, 3, 1)

    example = tf.train.Example(features=tf.train.Features(feature={
        'image/depth_map': self._EncodedFloatFeature(np_array),
    }))

    serialized_example = example.SerializeToString()

    with self.test_session():
      serialized_example = tf.reshape(serialized_example, shape=[])

      keys_to_features = {
          'image/depth_map': tf.FixedLenFeature(
              tensor_shape, tf.float32, default_value=tf.zeros(tensor_shape))
      }

      def HandleDepth(keys_to_tensors):
        depth = list(keys_to_tensors.values())[0]
        depth += 1
        return depth

      items_to_handlers = {
          'depth': slim.tfexample_decoder.ItemHandlerCallback(
              'image/depth_map', HandleDepth)
      }

      decoder = slim.tfexample_decoder.TFExampleDecoder(
          keys_to_features, items_to_handlers)
      [tf_depth] = decoder.decode(serialized_example, ['depth'])
      depth = tf_depth.eval()

    self.assertTrue(np.isclose(np_array, depth-1).all())

  def testDecodeImageWithItemHandlerCallback(self):
    image_shape = (2, 3, 3)
    for image_encoding in ['jpeg', 'png']:
      image, serialized_example = self.GenerateImage(
          image_format=image_encoding,
          image_shape=image_shape)

      with self.test_session():

        def ConditionalDecoding(keys_to_tensors):
          """See base class."""
          image_buffer = keys_to_tensors['image/encoded']
          image_format = keys_to_tensors['image/format']

          def DecodePng():
            return tf.image.decode_png(image_buffer, 3)
          def DecodeJpg():
            return tf.image.decode_jpeg(image_buffer, 3)

          image = tf.case({
              tf.equal(image_format, 'png'): DecodePng,
          }, default=DecodeJpg, exclusive=True)
          image = tf.reshape(image, image_shape)
          return image

        keys_to_features = {
            'image/encoded': tf.FixedLenFeature(
                (), tf.string, default_value=''),
            'image/format': tf.FixedLenFeature(
                (), tf.string, default_value='jpeg')
        }

        items_to_handlers = {
            'image': slim.tfexample_decoder.ItemHandlerCallback(
                ['image/encoded', 'image/format'], ConditionalDecoding)
        }

        decoder = slim.tfexample_decoder.TFExampleDecoder(
            keys_to_features, items_to_handlers)
        [tf_image] = decoder.decode(serialized_example, ['image'])
        decoded_image = tf_image.eval()
        if image_encoding == 'jpeg':
          # For jenkins:
          image = image.astype(np.float32)
          decoded_image = decoded_image.astype(np.float32)
          self.assertAllClose(image, decoded_image, rtol=.5, atol=1.001)
        else:
          self.assertAllClose(image, decoded_image, atol=0)

if __name__ == '__main__':
  tf.test.main()
