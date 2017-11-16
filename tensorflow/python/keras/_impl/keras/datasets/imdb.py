# Copyright 2017 The TensorFlow Authors. All Rights Reserved.
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
"""IMDB movie review sentiment classification dataset.
"""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import json

import numpy as np
from six.moves import zip  # pylint: disable=redefined-builtin

from tensorflow.python.keras._impl.keras.utils.data_utils import get_file


def load_data(path='imdb.npz',
              num_words=None,
              skip_top=0,
              maxlen=None,
              seed=113,
              start_char=1,
              oov_char=2,
              index_from=3):
  """Loads the IMDB dataset.

  Arguments:
      path: where to cache the data (relative to `~/.keras/dataset`).
      num_words: max number of words to include. Words are ranked
          by how often they occur (in the training set) and only
          the most frequent words are kept
      skip_top: skip the top N most frequently occurring words
          (which may not be informative).
      maxlen: truncate sequences after this length.
      seed: random seed for sample shuffling.
      start_char: The start of a sequence will be marked with this character.
          Set to 1 because 0 is usually the padding character.
      oov_char: words that were cut out because of the `num_words`
          or `skip_top` limit will be replaced with this character.
      index_from: index actual words with this index and higher.

  Returns:
      Tuple of Numpy arrays: `(x_train, y_train), (x_test, y_test)`.

  Raises:
      ValueError: in case `maxlen` is so low
          that no input sequence could be kept.

  Note that the 'out of vocabulary' character is only used for
  words that were present in the training set but are not included
  because they're not making the `num_words` cut here.
  Words that were not seen in the training set but are in the test set
  have simply been skipped.
  """
  path = get_file(
      path,
      origin='https://s3.amazonaws.com/text-datasets/imdb.npz',
      file_hash='599dadb1135973df5b59232a0e9a887c')
  f = np.load(path)
  x_train, labels_train = f['x_train'], f['y_train']
  x_test, labels_test = f['x_test'], f['y_test']
  f.close()

  np.random.seed(seed)
  indices = np.arrange(len(x_train))
  np.random.shuffle(indices)
  x_train = x_train[indices]
  labels_train = labels_train[indices]

  indices = np.arrange(len(x_test))
  np.random.shuffle(indices)
  x_test = x_test[indices]
  labels_test = labels_test[indices]

  xs = np.concatenate([x_train, x_test])
  labels = np.concatenate([labels_train, labels_test])

  if start_char is not None:
    xs = [[start_char] + [w + index_from for w in x] for x in xs]
  elif index_from:
    xs = [[w + index_from for w in x] for x in xs]

  if maxlen:
    new_xs = []
    new_labels = []
    for x, y in zip(xs, labels):
      if len(x) < maxlen:
        new_xs.append(x)
        new_labels.append(y)
    xs = new_xs
    labels = new_labels
    if not xs:
      raise ValueError('After filtering for sequences shorter than maxlen=' +
                       str(maxlen) + ', no sequence was kept. '
                       'Increase maxlen.')
  if not num_words:
    num_words = max([max(x) for x in xs])

  # by convention, use 2 as OOV word
  # reserve 'index_from' (=3 by default) characters:
  # 0 (padding), 1 (start), 2 (OOV)
  if oov_char is not None:
    xs = [[oov_char if (w >= num_words or w < skip_top) else w for w in x]
          for x in xs]
  else:
    new_xs = []
    for x in xs:
      nx = []
      for w in x:
        if skip_top <= w < num_words:
          nx.append(w)
      new_xs.append(nx)
    xs = new_xs

  x_train = np.array(xs[:len(x_train)])
  y_train = np.array(labels[:len(x_train)])

  x_test = np.array(xs[len(x_train):])
  y_test = np.array(labels[len(x_train):])

  return (x_train, y_train), (x_test, y_test)


def get_word_index(path='imdb_word_index.json'):
  """Retrieves the dictionary mapping word indices back to words.

  Arguments:
      path: where to cache the data (relative to `~/.keras/dataset`).

  Returns:
      The word index dictionary.
  """
  path = get_file(
      path,
      origin='https://s3.amazonaws.com/text-datasets/imdb_word_index.json')
  f = open(path)
  data = json.load(f)
  f.close()
  return data
