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
"""Experimental API for controlling optimizations in `tf.data` pipelines."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function


from tensorflow.python.data.util import options
from tensorflow.python.util.tf_export import tf_export


@tf_export("data.experimental.OptimizationOptions")
class OptimizationOptions(options.OptionsBase):
  """Represents options for dataset optimizations.

  You can apply `OptimizationOptions` to a `dataset` object, as follows:

  ```python
  options = tf.data.Options()
  options.optimization = tf.data.experimental.OptimizationOptions()
  options.optimization.map_and_batch_fusion = True
  dataset = dataset.with_options(options)
  ```
  """
  apply_default_optimizations = options.create_option(
      name="apply_default_optimizations",
      ty=bool,
      docstring=
      "Whether to apply default static optimizations. If False, only static "
      "optimizations that have been explicitly enabled will be applied.")

  filter_fusion = options.create_option(
      name="filter_fusion",
      ty=bool,
      docstring="Whether to fuse filter transformations.")

  hoist_random_uniform = options.create_option(
      name="hoist_random_uniform",
      ty=bool,
      docstring=
      "Whether to hoist `tf.random_uniform()` ops out of map transformations.")

  map_and_batch_fusion = options.create_option(
      name="map_and_batch_fusion",
      ty=bool,
      docstring="Whether to fuse map and batch transformations.")

  map_and_filter_fusion = options.create_option(
      name="map_and_filter_fusion",
      ty=bool,
      docstring="Whether to fuse map and filter transformations.")

  map_fusion = options.create_option(
      name="map_and_filter_fusion",
      ty=bool,
      docstring="Whether to fuse map transformations.")

  map_parallelization = options.create_option(
      name="map_parallelization",
      ty=bool,
      docstring="Whether to parallelize stateless map transformations.")

  map_vectorization = options.create_option(
      name="map_vectorization",
      ty=bool,
      docstring="Whether to vectorize map transformations.")

  noop_elimination = options.create_option(
      name="noop_elimination",
      ty=bool,
      docstring="Whether to eliminate no-op transformations.")

  shuffle_and_repeat_fusion = options.create_option(
      name="shuffle_and_repeat_fusion",
      ty=bool,
      docstring="Whether to fuse shuffle and repeat transformations.")

  def _static_optimizations(self):
    """Produces the list of enabled static optimizations."""
    result = []
    optimizations_to_enable = [
        "filter_fusion",
        "hoist_random_uniform",
        "map_and_batch_fusion",
        "map_and_filter_fusion",
        "map_fusion",
        "map_parallelization",
        "map_vectorization",
        "shuffle_and_repeat_fusion",
    ]
    for optimization in optimizations_to_enable:
      if getattr(self, optimization):
        result.append(optimization)

    if self.apply_default_optimizations is not False:
      # The following optimizations are turned on by default, unless the
      # user explicitly disables them.
      optimizations_to_disable = [
          "noop_elimination",
      ]
      for optimization in optimizations_to_disable:
        if getattr(self, optimization) is not False:
          result.append(optimization)
    return result
