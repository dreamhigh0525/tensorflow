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

"""Convert @{symbol} to MarkDown links in the Python API guides."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import argparse
import os

from tensorflow.tools.docs import generate
from tensorflow.tools.docs import parser
from tensorflow.tools.docs import py_guide_parser


class UpdateTags(py_guide_parser.PyGuideParser):
  """Rewrites a Python guide so that each section has an explicit tag."""

  def process_section(self, line_number, section_title, tag):
    self.replace_line(line_number, '## %s {#%s}' % (section_title, tag))


def _main(input_dir, output_dir):
  """Convert all the files in `input_dir` and write results to `output_dir`."""
  visitor = generate.extract()

  # Make output_dir.
  try:
    if not os.path.exists(output_dir):
      os.makedirs(output_dir)
  except OSError as e:
    print('Creating output dir "%s" failed: %s' % (output_dir, e))
    raise

  # How to get from api_guides/python/ to api_docs/python/
  relative_path_to_root = '../../api_docs/python/'

  # Iterate through all the source files and process them.
  tag_updater = UpdateTags()
  for full_path, base_name in py_guide_parser.md_files_in_dir(input_dir):
    print('Processing %s...' % base_name)
    md_string = tag_updater.process(full_path)
    output = parser.replace_references(
        md_string, relative_path_to_root, visitor.duplicate_of)
    open(os.path.join(output_dir, base_name), 'w').write(output)
  print('Done.')


if __name__ == '__main__':
  argument_parser = argparse.ArgumentParser()
  argument_parser.add_argument(
      '--input_dir',
      type=str,
      default=None,
      required=True,
      help='Directory to copy docs from.'
  )
  argument_parser.add_argument(
      '--output_dir',
      type=str,
      default=None,
      required=True,
      help='Directory to write docs to. Will be created, must not exist.'
  )
  flags, _ = argument_parser.parse_known_args()
  if os.path.exists(flags.output_dir):
    raise RuntimeError('output_dir %s exists.\n'
                       'Cowardly refusing to wipe it, please do that yourself.'
                       % flags.output_dir)

  _main(flags.input_dir, flags.output_dir)
