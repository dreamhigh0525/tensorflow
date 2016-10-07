# Copyright 2016 The TensorFlow Authors. All Rights Reserved.
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
"""Tests for CRF."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import itertools

import numpy as np
import tensorflow as tf


class CrfTest(tf.test.TestCase):

  def testCrfSequenceScore(self):
    inputs = np.array(
        [[4, 5, -3], [3, -1, 3], [-1, 2, 1], [0, 0, 0]], dtype=np.float32)
    tag_indices = np.array([1, 2, 1, 0], dtype=np.int32)
    transition_params = np.array(
        [[-3, 5, -2], [3, 4, 1], [1, 2, 1]], dtype=np.float32)
    sequence_lengths = np.array(3, dtype=np.int32)
    with self.test_session() as sess:
      sequence_score = tf.contrib.crf.crf_sequence_score(
          inputs=tf.expand_dims(inputs, 0),
          tag_indices=tf.expand_dims(tag_indices, 0),
          sequence_lengths=tf.expand_dims(sequence_lengths, 0),
          transition_params=tf.constant(transition_params))
      sequence_score = tf.squeeze(sequence_score, [0])
      tf_sequence_score = sess.run(sequence_score)
      expected_unary_score = sum(inputs[i][tag_indices[i]]
                                 for i in range(sequence_lengths))
      expected_binary_score = sum(
          transition_params[tag_indices[i], tag_indices[i + 1]]
          for i in range(sequence_lengths - 1))
      expected_sequence_score = expected_unary_score + expected_binary_score
      self.assertAllClose(tf_sequence_score, expected_sequence_score)

  def testCrfUnaryScore(self):
    inputs = np.array(
        [[4, 5, -3], [3, -1, 3], [-1, 2, 1], [0, 0, 0]], dtype=np.float32)
    tag_indices = np.array([1, 2, 1, 0], dtype=np.int32)
    sequence_lengths = np.array(3, dtype=np.int32)
    with self.test_session() as sess:
      unary_score = tf.contrib.crf.crf_unary_score(
          tag_indices=tf.expand_dims(tag_indices, 0),
          sequence_lengths=tf.expand_dims(sequence_lengths, 0),
          inputs=tf.expand_dims(inputs, 0))
      unary_score = tf.squeeze(unary_score, [0])
      tf_unary_score = sess.run(unary_score)
      expected_unary_score = sum(inputs[i][tag_indices[i]]
                                 for i in range(sequence_lengths))
      self.assertAllClose(tf_unary_score, expected_unary_score)

  def testCrfBinaryScore(self):
    tag_indices = np.array([1, 2, 1, 0], dtype=np.int32)
    transition_params = np.array(
        [[-3, 5, -2], [3, 4, 1], [1, 2, 1]], dtype=np.float32)
    sequence_lengths = np.array(3, dtype=np.int32)
    with self.test_session() as sess:
      binary_score = tf.contrib.crf.crf_binary_score(
          tag_indices=tf.expand_dims(tag_indices, 0),
          sequence_lengths=tf.expand_dims(sequence_lengths, 0),
          transition_params=tf.constant(transition_params))
      binary_score = tf.squeeze(binary_score, [0])
      tf_binary_score = sess.run(binary_score)
      expected_binary_score = sum(
          transition_params[tag_indices[i], tag_indices[i + 1]]
          for i in range(sequence_lengths - 1))
      self.assertAllClose(tf_binary_score, expected_binary_score)

  def testCrfLogNorm(self):
    inputs = np.array(
        [[4, 5, -3], [3, -1, 3], [-1, 2, 1], [0, 0, 0]], dtype=np.float32)
    transition_params = np.array(
        [[-3, 5, -2], [3, 4, 1], [1, 2, 1]], dtype=np.float32)
    num_words = inputs.shape[0]
    num_tags = inputs.shape[1]
    sequence_lengths = np.array(3, dtype=np.int32)
    with self.test_session() as sess:
      all_sequence_scores = []

      # Compare the dynamic program with brute force computation.
      for tag_indices in itertools.product(
          range(num_tags), repeat=sequence_lengths):
        tag_indices = list(tag_indices)
        tag_indices.extend([0] * (num_words - sequence_lengths))
        all_sequence_scores.append(
            tf.contrib.crf.crf_sequence_score(
                inputs=tf.expand_dims(inputs, 0),
                tag_indices=tf.expand_dims(tag_indices, 0),
                sequence_lengths=tf.expand_dims(sequence_lengths, 0),
                transition_params=tf.constant(transition_params)))

      brute_force_log_norm = tf.reduce_logsumexp(all_sequence_scores)
      log_norm = tf.contrib.crf.crf_log_norm(
          inputs=tf.expand_dims(inputs, 0),
          sequence_lengths=tf.expand_dims(sequence_lengths, 0),
          transition_params=tf.constant(transition_params))
      log_norm = tf.squeeze(log_norm, [0])
      tf_brute_force_log_norm, tf_log_norm = sess.run(
          [brute_force_log_norm, log_norm])

      self.assertAllClose(tf_log_norm, tf_brute_force_log_norm)

  def testCrfLogLikelihood(self):
    inputs = np.array(
        [[4, 5, -3], [3, -1, 3], [-1, 2, 1], [0, 0, 0]], dtype=np.float32)
    transition_params = np.array(
        [[-3, 5, -2], [3, 4, 1], [1, 2, 1]], dtype=np.float32)
    sequence_lengths = np.array(3, dtype=np.int32)
    num_words = inputs.shape[0]
    num_tags = inputs.shape[1]
    with self.test_session() as sess:
      all_sequence_log_likelihoods = []

      # Make sure all probabilities sum to 1.
      for tag_indices in itertools.product(
          range(num_tags), repeat=sequence_lengths):
        tag_indices = list(tag_indices)
        tag_indices.extend([0] * (num_words - sequence_lengths))
        sequence_log_likelihood, _ = tf.contrib.crf.crf_log_likelihood(
            inputs=tf.expand_dims(inputs, 0),
            tag_indices=tf.expand_dims(tag_indices, 0),
            sequence_lengths=tf.expand_dims(sequence_lengths, 0),
            transition_params=tf.constant(transition_params))
        all_sequence_log_likelihoods.append(sequence_log_likelihood)
      total_log_likelihood = tf.reduce_logsumexp(all_sequence_log_likelihoods)
      tf_total_log_likelihood = sess.run(total_log_likelihood)
      self.assertAllClose(tf_total_log_likelihood, 0.0)

  def testLengthsToMasks(self):
    with self.test_session() as sess:
      sequence_lengths = [4, 1, 8, 2]
      max_sequence_length = max(sequence_lengths)

      mask = tf.contrib.crf._lengths_to_masks(sequence_lengths,
                                              max_sequence_length)
      tf_mask = sess.run(mask)
      self.assertEqual(len(tf_mask), len(sequence_lengths))
      for m, l in zip(tf_mask, sequence_lengths):
        self.assertAllEqual(m[:l], [1] * l)
        self.assertAllEqual(m[l:], [0] * (len(m) - l))

  def testViterbiDecode(self):
    inputs = np.array(
        [[4, 5, -3], [3, -1, 3], [-1, 2, 1], [0, 0, 0]], dtype=np.float32)
    transition_params = np.array(
        [[-3, 5, -2], [3, 4, 1], [1, 2, 1]], dtype=np.float32)
    sequence_lengths = np.array(3, dtype=np.int32)
    num_words = inputs.shape[0]
    num_tags = inputs.shape[1]

    with self.test_session() as sess:
      all_sequence_scores = []
      all_sequences = []

      # Compare the dynamic program with brute force computation.
      for tag_indices in itertools.product(
          range(num_tags), repeat=sequence_lengths):
        tag_indices = list(tag_indices)
        tag_indices.extend([0] * (num_words - sequence_lengths))
        all_sequences.append(tag_indices)
        sequence_score = tf.contrib.crf.crf_sequence_score(
            inputs=tf.expand_dims(inputs, 0),
            tag_indices=tf.expand_dims(tag_indices, 0),
            sequence_lengths=tf.expand_dims(sequence_lengths, 0),
            transition_params=tf.constant(transition_params))
        sequence_score = tf.squeeze(sequence_score, [0])
        all_sequence_scores.append(sequence_score)

      tf_all_sequence_scores = sess.run(all_sequence_scores)

      expected_max_sequence_index = np.argmax(tf_all_sequence_scores)
      expected_max_sequence = all_sequences[expected_max_sequence_index]
      expected_max_score = tf_all_sequence_scores[expected_max_sequence_index]

      actual_max_sequence, actual_max_score = tf.contrib.crf.viterbi_decode(
          inputs[:sequence_lengths], transition_params)

      self.assertAllClose(actual_max_score, expected_max_score)
      self.assertEqual(actual_max_sequence,
                       expected_max_sequence[:sequence_lengths])


if __name__ == "__main__":
  tf.test.main()
