# Copyright 2022 The TensorFlow Authors. All Rights Reserved.
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
"""Tests for restore.py."""

from tensorflow.python.checkpoint import restore
from tensorflow.python.eager import test


class ExtractSaveablenameTest(test.TestCase):

  def test_standard_saveable_name(self):
    self.assertEqual(
        "object_path/.ATTRIBUTES/",
        restore._extract_saveable_name("object_path/.ATTRIBUTES/123"))
    self.assertEqual(
        "object/path/ATTRIBUTES/.ATTRIBUTES/",
        restore._extract_saveable_name("object/path/ATTRIBUTES/.ATTRIBUTES/"))


if __name__ == "__main__":
  test.main()
