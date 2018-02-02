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
"""Operators for manipulating tensors.

@@roll
"""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.python.ops import gen_manip_ops as _gen_manip_ops
from tensorflow.python.util.all_util import remove_undocumented

# pylint: disable=protected-access
def roll(input, shift, axis):
  return _gen_manip_ops.roll(input, shift, axis)

roll.__doc__ = _gen_manip_ops.roll.__doc__
# pylint: enable=protected-access

_allowed_symbols = ['roll']

remove_undocumented(__name__, allowed_exception_list=_allowed_symbols)
