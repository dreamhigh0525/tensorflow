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
"""Tests for multiple_dispatch."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from tensorflow.contrib.py2tf.utils import multiple_dispatch
from tensorflow.python.client.session import Session
from tensorflow.python.framework import dtypes
from tensorflow.python.framework.constant_op import constant
from tensorflow.python.ops.control_flow_ops import cond
from tensorflow.python.platform import test


class MultipleDispatchTest(test.TestCase):

  def test_run_cond_python(self):
    true_fn = lambda: 2.0
    false_fn = lambda: 3.0
    self.assertEqual(multiple_dispatch.run_cond(True, true_fn, false_fn), 2.0)
    self.assertEqual(multiple_dispatch.run_cond(False, true_fn, false_fn), 3.0)

  def test_run_cond_tf(self):

    true_fn = lambda: constant([2.0])
    false_fn = lambda: constant([3.0])
    with Session() as sess:

      out = cond(constant(True), true_fn, false_fn)
      self.assertEqual(sess.run(out), 2.0)
      out = cond(constant(False, dtype=dtypes.bool), true_fn, false_fn)
      self.assertEqual(sess.run(out), 3.0)


if __name__ == '__main__':
  test.main()
