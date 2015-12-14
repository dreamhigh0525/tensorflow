"""TensorFlow ops for deep neural networks."""

#  Copyright 2015 Google Inc. All Rights Reserved.
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.


import tensorflow as tf

# TODO(ilblackdragon@): Fix this in TensorFlow v0.6.
try:
    from tensorflow.models.rnn.linear import linear
except AttributeError:
    from tensorflow.python.ops.rnn_cell import linear


def dnn(tensor_in, hidden_units, activation=tf.nn.relu, keep_prob=None):
    """Creates fully connected deep neural network subgraph.

    Args:
        tenson_in: tensor or placeholder for input features.
        hidden_units: list of counts of hidden units in each layer.
        activation: activation function between layers.
        keep_proba: if not None, will add a dropout layer with given
                    probability.

    Returns:
        A tensor which would be a deep neural network.
    """
    with tf.variable_scope('dnn'):
        for i, n_units in enumerate(hidden_units):
            with tf.variable_scope('layer%d' % i):
                tensor_in = linear(tensor_in, n_units, True)
            tensor_in = activation(tensor_in)
            if keep_prob:
                tensor_in = tf.nn.dropout(tensor_in, keep_prob)
        return tensor_in
