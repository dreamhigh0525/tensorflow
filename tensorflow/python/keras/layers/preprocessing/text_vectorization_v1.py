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
"""Tensorflow V1 version of the text vectorization preprocessing layer."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np

from tensorflow.python.keras import backend as K
from tensorflow.python.keras.layers.preprocessing import text_vectorization
from tensorflow.python.ops.ragged import ragged_tensor_value


class TextVectorization(text_vectorization.TextVectorization):
  """Text vectorization layer.

  This layer has basic options for managing text in a Keras model. It
  transforms a batch of strings (one sample = one string) into either a list of
  token indices (one sample = 1D tensor of integer token indices) or a dense
  representation (one sample = 1D tensor of float values representing data about
  the sample's tokens).

  The processing of each sample contains the following steps:
    1) standardize each sample (usually lowercasing + punctuation stripping)
    2) split each sample into substrings (usually words)
    3) recombine substrings into tokens (usually ngrams)
    4) index tokens (associate a unique int value with each token)
    5) transform each sample using this index, either into a vector of ints or
       a dense float vector.

  Attributes:
    max_tokens: The maximum size of the vocabulary for this layer. If None,
      there is no cap on the size of the vocabulary.
    standardize: Optional specification for standardization to apply to the
      input text. Values can be None (no standardization),
      LOWER_AND_STRIP_PUNCTUATION (lowercase and remove punctuation) or a
      Callable.
    split: Optional specification for splitting the input text. Values can be
      None (no splitting), SPLIT_ON_WHITESPACE (split on ASCII whitespace), or a
      Callable.
    ngrams: Optional specification for ngrams to create from the possibly-split
      input text. Values can be None, an integer or tuple of integers; passing
      an integer will create ngrams up to that integer, and passing a tuple of
      integers will create ngrams for the specified values in the tuple. Passing
      None means that no ngrams will be created.
    output_mode: Optional specification for the output of the layer. Values can
      be INT, BINARY, COUNT or TFIDF, which control the outputs as follows:
        INT: Outputs integer indices, one integer index per split string token.
        BINARY: Outputs a single int array per batch, of either vocab_size or
          max_tokens size, containing 1s in all elements where the token mapped
          to that index exists at least once in the batch item.
        COUNT: As BINARY, but the int array contains a count of the number of
          times the token at that index appeared in the batch item.
        TFIDF: As BINARY, but the TF-IDF algorithm is applied to find the value
          in each token slot.
    output_sequence_length: Optional length for the output tensor. If set,
      the output will be padded or truncated to this value in INT mode.
    pad_to_max_tokens: If True, BINARY, COUNT, and TFIDF modes will have their
      outputs padded to max_tokens, even if the number of unique tokens in the
      vocabulary is less than max_tokens.
  """

  def _get_table_data(self):
    keys, values = self._table.export()
    np_keys = K.get_session().run(keys)
    np_values = K.get_session().run(values)
    return (np_keys, np_values)

  def _get_table_size(self):
    return K.get_session().run(self._table.size())

  def _clear_table(self):
    keys, _ = self._table.export()
    K.get_session().run(self._table.remove(keys))
    self._has_vocab = False

  def _insert_table_data(self, keys, values):
    K.get_session().run(self._table.insert(keys, values))
    self._has_vocab = True

  def _to_numpy(self, data):
    """Converts preprocessed inputs into numpy arrays."""
    if isinstance(data, np.ndarray):
      return data
    session = K.get_session()
    data = session.run(data)
    if isinstance(data, ragged_tensor_value.RaggedTensorValue):
      data = np.array(data.to_list())
    return data
