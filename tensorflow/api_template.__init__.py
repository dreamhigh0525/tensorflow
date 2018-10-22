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
"""
Top-level module of TensorFlow. By convention, we refer to this module as 
`tf` instead of `tensorflow`, following the common practice of importing 
TensorFlow via the command `import tensorflow as tf`.

The primary function of this module is to import all of the public TensorFlow 
interfaces into a single place. The interfaces themselves are located in 
sub-modules, as described below.

Note that the file `__init__.py` in the TensorFlow source code tree is actually 
only a placeholder to enable test cases to run. The TensorFlow build replaces 
this file with a file generated from [`api_template.__init__.py`](https://www.github.com/tensorflow/tensorflow/blob/master/tensorflow/api_template.__init__.py)
"""

from __future__ import absolute_import as _absolute_import
from __future__ import division as _division
from __future__ import print_function as _print_function

import os as _os

# pylint: disable=g-bad-import-order
from tensorflow.python import pywrap_tensorflow  # pylint: disable=unused-import

from tensorflow.python.tools import component_api_helper
component_api_helper.package_hook(
    parent_package_str=__name__,
    child_package_str=('tensorflow_estimator.python.estimator.api.estimator'))
del component_api_helper

# API IMPORTS PLACEHOLDER

from tensorflow.python.util.lazy_loader import LazyLoader  # pylint: disable=g-import-not-at-top
contrib = LazyLoader('contrib', globals(), 'tensorflow.contrib')
del LazyLoader
# The templated code that replaces the placeholder above sometimes
# sets the __all__ variable. If it does, we have to be sure to add
# "contrib".
if '__all__' in vars():
  vars()['__all__'].append('contrib')

from tensorflow.python.platform import flags  # pylint: disable=g-import-not-at-top
app.flags = flags  # pylint: disable=undefined-variable

# Make sure directory containing top level submodules is in
# the __path__ so that "from tensorflow.foo import bar" works.
_tf_api_dir = _os.path.dirname(_os.path.dirname(app.__file__))  # pylint: disable=undefined-variable
if _tf_api_dir not in __path__:
  __path__.append(_tf_api_dir)

# These symbols appear because we import the python package which
# in turn imports from tensorflow.core and tensorflow.python. They
# must come from this module. So python adds these symbols for the
# resolution to succeed.
# pylint: disable=undefined-variable
try:
  del python
  del core
except NameError:
  # Don't fail if these modules are not available.
  # For e.g. we are using this file for compat.v1 module as well and
  # 'python', 'core' directories are not under compat/v1.
  pass
# pylint: enable=undefined-variable
