# Copyright 2015 The TensorFlow Authors. All Rights Reserved.
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
"""Tests for tf.layers.core."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import tensorflow as tf

from tensorflow.python.layers import core as core_layers


class FullyConnectedTest(tf.test.TestCase):

  def testFCProperties(self):
    fc = core_layers.FullyConnected(2, activation=tf.nn.relu, name='fc')
    self.assertEqual(fc.output_dim, 2)
    self.assertEqual(fc.activation, tf.nn.relu)
    self.assertEqual(fc.w_regularizer, None)
    self.assertEqual(fc.bias_regularizer, None)
    self.assertEqual(fc.activity_regularizer, None)
    self.assertEqual(fc.use_bias, True)
    self.assertEqual(fc.name, 'fc')

    # Test auto-naming
    fc = core_layers.FullyConnected(2, activation=tf.nn.relu)
    self.assertEqual(fc.name, 'fully_connected')
    fc = core_layers.FullyConnected(2, activation=tf.nn.relu)
    self.assertEqual(fc.name, 'fully_connected_1')

  def testCall(self):
    fc = core_layers.FullyConnected(2, activation=tf.nn.relu, name='fc')
    inputs = tf.random_uniform((5, 2), seed=1)
    _ = fc(inputs)
    self.assertListEqual(fc.weights, [fc.w, fc.bias])
    self.assertListEqual(fc.trainable_weights, [fc.w, fc.bias])
    self.assertListEqual(fc.non_trainable_weights, [])
    self.assertListEqual(fc._trainable_weights, [fc.w, fc.bias])
    self.assertListEqual(fc._non_trainable_weights, [])
    self.assertEqual(
        len(tf.get_collection(tf.GraphKeys.TRAINABLE_VARIABLES)), 2)
    self.assertEqual(fc.w.name, 'fc/weights:0')
    self.assertEqual(fc.bias.name, 'fc/biases:0')

  def testNoBias(self):
    fc = core_layers.FullyConnected(2, use_bias=False, name='fc')
    inputs = tf.random_uniform((5, 2), seed=1)
    _ = fc(inputs)
    self.assertListEqual(fc.weights, [fc.w])
    self.assertListEqual(fc.trainable_weights, [fc.w])
    self.assertListEqual(fc.non_trainable_weights, [])
    self.assertEqual(
        len(tf.get_collection(tf.GraphKeys.TRAINABLE_VARIABLES)), 1)
    self.assertEqual(fc.w.name, 'fc/weights:0')
    self.assertEqual(fc.bias, None)

  def testNonTrainable(self):
    fc = core_layers.FullyConnected(2, trainable=False, name='fc')
    inputs = tf.random_uniform((5, 2), seed=1)
    _ = fc(inputs)
    self.assertListEqual(fc.weights, [fc.w, fc.bias])
    self.assertListEqual(fc.non_trainable_weights, [fc.w, fc.bias])
    self.assertListEqual(fc.trainable_weights, [])
    self.assertListEqual(fc._trainable_weights, [fc.w, fc.bias])
    self.assertListEqual(fc._non_trainable_weights, [])
    self.assertEqual(
        len(tf.get_collection(tf.GraphKeys.TRAINABLE_VARIABLES)), 0)

  def testOutputShape(self):
    fc = core_layers.FullyConnected(7, activation=tf.nn.relu, name='fc')
    inputs = tf.random_uniform((5, 3), seed=1)
    outputs = fc.apply(inputs)
    self.assertEqual(outputs.get_shape().as_list(), [5, 7])

    inputs = tf.random_uniform((5, 2, 3), seed=1)
    outputs = fc(inputs)
    self.assertEqual(outputs.get_shape().as_list(), [5, 2, 7])

    inputs = tf.random_uniform((1, 2, 4, 3), seed=1)
    outputs = fc.apply(inputs)
    self.assertEqual(outputs.get_shape().as_list(), [1, 2, 4, 7])

  def testCallOnPlaceHolder(self):
    inputs = tf.placeholder(dtype=tf.float32)
    fc = core_layers.FullyConnected(4, name='fc')
    with self.assertRaises(ValueError):
      fc(inputs)

    inputs = tf.placeholder(dtype=tf.float32, shape=[None, None])
    fc = core_layers.FullyConnected(4, name='fc')
    with self.assertRaises(ValueError):
      fc(inputs)

    inputs = tf.placeholder(dtype=tf.float32, shape=[None, None, None])
    fc = core_layers.FullyConnected(4, name='fc')
    with self.assertRaises(ValueError):
      fc(inputs)

    inputs = tf.placeholder(dtype=tf.float32, shape=[None, 3])
    fc = core_layers.FullyConnected(4, name='fc')
    fc(inputs)

    inputs = tf.placeholder(dtype=tf.float32, shape=[None, None, 3])
    fc = core_layers.FullyConnected(4, name='fc')
    fc(inputs)

  def testActivation(self):
    fc = core_layers.FullyConnected(2, activation=tf.nn.relu, name='fc1')
    inputs = tf.random_uniform((5, 3), seed=1)
    outputs = fc(inputs)
    self.assertEqual(outputs.op.name, 'fc1/Relu')

    fc = core_layers.FullyConnected(2, name='fc2')
    inputs = tf.random_uniform((5, 3), seed=1)
    outputs = fc(inputs)
    self.assertEqual(outputs.op.name, 'fc2/BiasAdd')

  def testActivityRegularizer(self):
    regularizer = lambda x: tf.reduce_sum(x) * 1e-3
    fc = core_layers.FullyConnected(2, name='fc',
                                    activity_regularizer=regularizer)
    inputs = tf.random_uniform((5, 3), seed=1)
    _ = fc(inputs)
    loss_keys = tf.get_collection(tf.GraphKeys.REGULARIZATION_LOSSES)
    self.assertEqual(len(loss_keys), 1)
    self.assertListEqual(fc.losses, loss_keys)

  def testWeightsRegularizer(self):
    regularizer = lambda x: tf.reduce_sum(x) * 1e-3
    fc = core_layers.FullyConnected(2, name='fc',
                                    w_regularizer=regularizer)
    inputs = tf.random_uniform((5, 3), seed=1)
    _ = fc(inputs)
    loss_keys = tf.get_collection(tf.GraphKeys.REGULARIZATION_LOSSES)
    self.assertEqual(len(loss_keys), 1)
    self.assertListEqual(fc.losses, loss_keys)

  def testBiasRegularizer(self):
    regularizer = lambda x: tf.reduce_sum(x) * 1e-3
    fc = core_layers.FullyConnected(2, name='fc',
                                    bias_regularizer=regularizer)
    inputs = tf.random_uniform((5, 3), seed=1)
    _ = fc(inputs)
    loss_keys = tf.get_collection(tf.GraphKeys.REGULARIZATION_LOSSES)
    self.assertEqual(len(loss_keys), 1)
    self.assertListEqual(fc.losses, loss_keys)

  def testFunctionalFC(self):
    inputs = tf.random_uniform((5, 3), seed=1)
    outputs = core_layers.fully_connected(
        inputs, 2, activation=tf.nn.relu, name='fc')
    self.assertEqual(
        len(tf.get_collection(tf.GraphKeys.TRAINABLE_VARIABLES)), 2)
    self.assertEqual(outputs.op.name, 'fc/Relu')
    self.assertEqual(outputs.get_shape().as_list(), [5, 2])


if __name__ == '__main__':
  tf.test.main()
