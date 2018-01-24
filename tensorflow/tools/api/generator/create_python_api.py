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
# =============================================================================
"""Generates and prints out imports and constants for new TensorFlow python api.
"""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import argparse
import collections
import os
import sys

# This import is needed so that we can traverse over TensorFlow modules.
import tensorflow as tf  # pylint: disable=unused-import
from tensorflow.python.util import tf_decorator


_API_CONSTANTS_ATTR = '_tf_api_constants'
_API_NAMES_ATTR = '_tf_api_names'
_API_DIR = '/api/'
_GENERATED_FILE_HEADER = """\"\"\"Imports for Python API.

This file is MACHINE GENERATED! Do not edit.
Generated by: tensorflow/tools/api/generator/create_python_api.py script.
\"\"\"
"""


def format_import(source_module_name, source_name, dest_name):
  """Formats import statement.

  Args:
    source_module_name: (string) Source module to import from.
    source_name: (string) Source symbol name to import.
    dest_name: (string) Destination alias name.

  Returns:
    An import statement string.
  """
  if source_name == dest_name:
    return 'from %s import %s' % (source_module_name, source_name)
  else:
    return 'from %s import %s as %s' % (
        source_module_name, source_name, dest_name)


def get_api_imports():
  """Get a map from destination module to formatted imports.

  Returns:
    A dictionary where
      key: (string) destination module (for e.g. tf or tf.consts).
      value: List of strings representing module imports
          (for e.g. 'from foo import bar') and constant
          assignments (for e.g. 'FOO = 123').
  """
  module_imports = collections.defaultdict(list)
  # Traverse over everything imported above. Specifically,
  # we want to traverse over TensorFlow Python modules.
  for module in sys.modules.values():
    # Only look at tensorflow modules.
    if not module or 'tensorflow.' not in module.__name__:
      continue

    for module_contents_name in dir(module):
      attr = getattr(module, module_contents_name)

      # If attr is _tf_api_constants attribute, then add the constants.
      if module_contents_name == _API_CONSTANTS_ATTR:
        for exports, value in attr:
          for export in exports:
            names = ['tf'] + export.split('.')
            dest_module = '.'.join(names[:-1])
            import_str = format_import(module.__name__, value, names[-1])
            module_imports[dest_module].append(import_str)
        continue

      _, attr = tf_decorator.unwrap(attr)
      # If attr is a symbol with _tf_api_names attribute, then
      # add import for it.
      if hasattr(attr, '__dict__') and _API_NAMES_ATTR in attr.__dict__:
        # The same op might be accessible from multiple modules.
        # We only want to consider location where function was defined.
        if attr.__module__ != module.__name__:
          continue

        for export in attr._tf_api_names:  # pylint: disable=protected-access
          names = ['tf'] + export.split('.')
          dest_module = '.'.join(names[:-1])
          import_str = format_import(
              module.__name__, module_contents_name, names[-1])
          module_imports[dest_module].append(import_str)

  # Import all required modules in their parent modules.
  # For e.g. if we import 'tf.foo.bar.Value'. Then, we also
  # import 'bar' in 'tf.foo'.
  dest_modules = set(module_imports.keys())
  for dest_module in dest_modules:
    dest_module_split = dest_module.split('.')
    for dest_submodule_index in range(1, len(dest_module_split)):
      dest_submodule = '.'.join(dest_module_split[:dest_submodule_index])
      submodule_import = format_import(
          '', dest_module_split[dest_submodule_index],
          dest_module_split[dest_submodule_index])
      if submodule_import not in module_imports[dest_submodule]:
        module_imports[dest_submodule].append(submodule_import)

  return module_imports


def create_api_files(output_files):
  """Creates __init__.py files for the Python API.

  Args:
    output_files: List of __init__.py file paths to create.
      Each file must be under api/ directory.

  Raises:
    ValueError: if an output file is not under api/ directory,
      or output_files list is missing a required file.
  """
  module_name_to_file_path = {}
  for output_file in output_files:
    if _API_DIR not in output_file:
      raise ValueError(
          'Output files must be in api/ directory, found %s.' % output_file)
    # Get the module name that corresponds to output_file.
    # First get module directory under _API_DIR.
    module_dir = os.path.dirname(
        output_file[output_file.rfind(_API_DIR)+len(_API_DIR):])
    # Convert / to . and prefix with tf.
    module_name = '.'.join(['tf', module_dir.replace('/', '.')]).strip('.')
    module_name_to_file_path[module_name] = output_file

  # Create file for each expected output in genrule.
  for module, file_path in module_name_to_file_path.items():
    if not os.path.isdir(os.path.dirname(file_path)):
      os.makedirs(os.path.dirname(file_path))
    open(file_path, 'a').close()

  # Add imports to output files.
  module_imports = get_api_imports()
  missing_output_files = []
  for module, exports in module_imports.items():
    # Make sure genrule output file list is in sync with API exports.
    if module not in module_name_to_file_path:
      module_without_tf = module[len('tf.'):]
      module_file_path = '"api/%s/__init__.py"' %  (
          module_without_tf.replace('.', '/'))
      missing_output_files.append(module_file_path)
      continue
    with open(module_name_to_file_path[module], 'w') as fp:
      fp.write(_GENERATED_FILE_HEADER + '\n'.join(exports))

  if missing_output_files:
    raise ValueError(
        'Missing outputs for python_api_gen genrule:\n%s.'
        'Make sure all required outputs are in the '
        'tensorflow/tools/api/generator/BUILD file.' %
        ',\n'.join(sorted(missing_output_files)))


def main(output_files):
  create_api_files(output_files)

if __name__ == '__main__':
  parser = argparse.ArgumentParser()
  parser.add_argument(
      'outputs', metavar='O', type=str, nargs='+',
      help='Python files that we expect this script to output.')
  args = parser.parse_args()
  main(args.outputs)
