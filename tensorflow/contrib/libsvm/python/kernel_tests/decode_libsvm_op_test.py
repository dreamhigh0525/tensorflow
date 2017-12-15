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
"""Tests for DecodeLibsvm op."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np
import sys
from tensorflow.contrib.libsvm.python.ops import libsvm_ops
from tensorflow.python.platform import test


class DecodeLibsvmOpTest(test.TestCase):

  def testBasic(self):
    with self.test_session() as sess:
      content = ["1 1:3.4 2:0.5 4:0.231",
                 "1 2:2.5 3:0.1 5:0.503",
                 "2 3:2.5 2:0.1 1:0.105"]
      label, feature = libsvm_ops.decode_libsvm(content, num_features=6)
      label, feature = sess.run([label, feature])
      self.assertAllEqual(label, [1, 1, 2])
      self.assertAllClose(feature, [[0, 3.4, 0.5, 0, 0.231, 0],
                                    [0, 0, 2.5, 0.1, 0, 0.503],
                                    [0, 0.105, 0.1, 2.5, 0, 0]])


if __name__ == "__main__":
  test.main()
