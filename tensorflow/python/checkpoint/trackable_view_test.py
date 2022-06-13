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
# =============================================================================
"""Tests for the trackable view."""

from tensorflow.python.checkpoint import trackable_view
from tensorflow.python.eager import test
from tensorflow.python.trackable import base


class TrackableViewTest(test.TestCase):

  def test_children(self):
    root = base.Trackable()
    leaf = base.Trackable()
    root._track_trackable(leaf, name="leaf")
    (current_name,
     current_dependency), = trackable_view.TrackableView(object).children(
         root, object).items()
    self.assertIs(leaf, current_dependency)
    self.assertEqual("leaf", current_name)

  def test_all_nodes(self):
    root = base.Trackable()
    leaf = base.Trackable()
    root._track_trackable(leaf, name="leaf")
    all_nodes = trackable_view.TrackableView(root).all_nodes()
    self.assertIs(2, len(all_nodes))
    self.assertIs(root, all_nodes[0])
    self.assertIs(leaf, all_nodes[1])


if __name__ == "__main__":
  test.main()
