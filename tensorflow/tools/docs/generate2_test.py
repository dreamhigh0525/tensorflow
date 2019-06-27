# Copyright 2018 The TensorFlow Authors. All Rights Reserved.
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
"""Tests for tensorflow.tools.docs.generate2."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import os
import shutil

import tensorflow as tf

from tensorflow.python.platform import googletest
from tensorflow.tools.docs import generate2

# Including the compat modules just makes the test take a lot longer.
# Ignoring these is okay, as the main risks of failure are around `tf.contrib`
# and any other modules that are not generated by tf_export.
del tf.compat.v2
del tf.compat.v1

class Generate2Test(googletest.TestCase):

  def test_end_to_end(self):
    output_dir = os.path.join(googletest.GetTempDir(), 'output')
    if os.path.exists(output_dir):
      shutil.rmtree(output_dir)
    os.makedirs(output_dir)
    generate2.build_docs(output_dir=output_dir, code_url_prefix='')


if __name__ == '__main__':
  googletest.main()
