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
"""Ops for building neural network layers, regularizers, summaries, etc.

## Higher level ops for building neural network layers.

This package provides several ops that take care of creating variables that are
used internally in a consistent way and provide the building blocks for many
common machine learning algorithms.

@@avg_pool2d
@@batch_norm
@@convolution2d
@@conv2d_in_plane
@@convolution2d_in_plane
@@conv2d_transpose
@@convolution2d_transpose
@@dropout
@@flatten
@@fully_connected
@@layer_norm
@@linear
@@max_pool2d
@@one_hot_encoding
@@relu
@@relu6
@@repeat
@@safe_embedding_lookup_sparse
@@separable_conv2d
@@separable_convolution2d
@@softmax
@@stack
@@unit_norm
@@embed_sequence

Aliases for fully_connected which set a default activation function are
available: `relu`, `relu6` and `linear`.

`stack` operation is also available. It builds a stack of layers by applying
a layer repeatedly.

## Regularizers

Regularization can help prevent overfitting. These have the signature
`fn(weights)`. The loss is typically added to
`tf.GraphKeys.REGULARIZATION_LOSSES`.

@@apply_regularization
@@l1_regularizer
@@l2_regularizer
@@sum_regularizer

## Initializers

Initializers are used to initialize variables with sensible values given their
size, data type, and purpose.

@@xavier_initializer
@@xavier_initializer_conv2d
@@variance_scaling_initializer

## Optimization

Optimize weights given a loss.

@@optimize_loss

## Summaries

Helper functions to summarize specific variables or ops.

@@summarize_activation
@@summarize_tensor
@@summarize_tensors
@@summarize_collection

The layers module defines convenience functions `summarize_variables`,
`summarize_weights` and `summarize_biases`, which set the `collection` argument
of `summarize_collection` to `VARIABLES`, `WEIGHTS` and `BIASES`, respectively.

@@summarize_activations

## Feature columns

Feature columns provide a mechanism to map data to a model.

@@bucketized_column
@@check_feature_columns
@@create_feature_spec_for_parsing
@@crossed_column
@@embedding_column
@@scattered_embedding_column
@@input_from_feature_columns
@@joint_weighted_sum_from_feature_columns
@@make_place_holder_tensors_for_base_features
@@multi_class_target
@@one_hot_column
@@parse_feature_columns_from_examples
@@parse_feature_columns_from_sequence_examples
@@real_valued_column
@@shared_embedding_columns
@@sparse_column_with_hash_bucket
@@sparse_column_with_integerized_feature
@@sparse_column_with_keys
@@weighted_sparse_column
@@weighted_sum_from_feature_columns
@@infer_real_valued_columns
@@sequence_input_from_feature_columns

"""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

# pylint: disable=unused-import,wildcard-import
from tensorflow.contrib.layers.python.layers import *
# pylint: enable=unused-import,wildcard-import

from tensorflow.python.util.all_util import remove_undocumented

_allowed_symbols = ['bias_add',
                    'conv2d',
                    'feature_column',
                    'legacy_fully_connected',
                    'legacy_linear',
                    'legacy_relu',
                    'OPTIMIZER_CLS_NAMES',
                    'regression_target',
                    'SPARSE_FEATURE_CROSS_DEFAULT_HASH_KEY',
                    'summaries']

remove_undocumented(__name__, _allowed_symbols)
