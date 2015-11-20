"""General TensorFlow ops."""

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
from tensorflow.models.rnn import linear


def embedding_lookup(params, ids, name="embedding_lookup"):
    """Provides a N dimensional version of tf.embedding_lookup.

    Ids are flattened to a 1d tensor before being passed to embedding_lookup
    then, they are unflattend to match the original ids shape plus an extra
    leading dimension of the size of the embeddings.

    Args:
        params: List of tensors of size D0 x D1 x ... x Dn-2 x Dn-1.
        ids: N-dimensional tensor of B0 x B1 x .. x Bn-2 x Bn-1.
             Must contain indexes into params.
        name: Optional name for the op.

    Returns:
        A tensor of size B0 x B1 x .. x Bn-2 x Bn-1 x D1 x ... x Dn-2 x Dn-1 containing the values from
        the params tensor(s) for indecies in ids.

    Raises:
        ValueError: if some parameters are invalid.
    """
    with tf.op_scope([params, ids], name, "embedding_lookup"):
        params = tf.convert_to_tensor(params)
        ids = tf.convert_to_tensor(ids)
        shape = tf.shape(ids)
        ids_flat = tf.reshape(ids, tf.reduce_prod(shape, keep_dims=True))
        embeds_flat = tf.nn.embedding_lookup(params, ids_flat, name)
        embed_shape = tf.concat(0, [shape, [-1]])
        embeds = tf.reshape(embeds_flat, embed_shape)
        embeds.set_shape(ids.get_shape().concatenate(params.get_shape()[1:]))
        return embeds


def categorical_variable(tensor_in, n_classes, embedding_size, name):
    """Creates an embedding for categorical variable with given number of
    classes.

    Args:
        tensor_in: Input tensor with class identifier (can be batch or
            N-dimensional).
        n_classes: Number of classes.
        embedding_size: Size of embedding vector to represent each class.
        name: Name of this categorical variable.
    Returns:
        Tensor of input shape, with additional dimension for embedding.

    Example:
        Calling categorical_variable([1, 2], 5, 10, "my_cat"), will return 2 x 10
        tensor, where each row is representation of the class.
    """
    with tf.variable_scope(name):
        embeddings = tf.get_variable(name + "_embeddings", [n_classes, embedding_size])
        return embedding_lookup(embeddings, tensor_in)


def mean_squared_error_regressor(tensor_in, labels, weights, biases, name=None):
    """Returns prediction and loss for mean squared error regression."""
    with tf.op_scope([tensor_in, labels], name, "mean_squared_error_regressor"):
        predictions = tf.nn.xw_plus_b(tensor_in, weights, biases)
        diff = predictions - labels
        loss = tf.reduce_mean(tf.mul(diff, diff))
        return predictions, loss


def softmax_classifier(tensor_in, labels, weights, biases, name=None):
    """Returns prediction and loss for softmax classifier."""
    with tf.op_scope([tensor_in, labels], name, "softmax_classifier"):
        logits = tf.nn.xw_plus_b(tensor_in, weights, biases)
        xent = tf.nn.softmax_cross_entropy_with_logits(logits,
                                                       labels,
                                                       name="xent_raw")
        loss = tf.reduce_mean(xent, name="xent")
        predictions = tf.nn.softmax(logits, name=name)
        return predictions, loss


def dnn(X, hidden_units, activation=tf.nn.relu, keep_prob=None):
    """Creates fully connected deep neural network subgraph.

    Args:
        X: tensor or placeholder for input features.
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
                X = linear.linear(X, n_units, True)
            X = activation(X)
            if keep_prob:
                X = tf.nn.dropout(X, keep_prob)
        return X


