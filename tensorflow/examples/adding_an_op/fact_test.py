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

"""Test that user ops can be used as expected."""
import tensorflow as tf
from tensorflow.python.framework import test_util


class FactTest(tf.test.TestCase):

  @test_util.run_deprecated_v1
  def test(self):
    with self.cached_session():
      print(tf.compat.v1.user_ops.my_fact().eval())


if __name__ == '__main__':
  tf.test.main()
