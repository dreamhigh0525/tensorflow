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
"""Upgrader for Python scripts from 1.* TensorFlow to 2.0 TensorFlow."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import argparse

from tensorflow.tools.compatibility import ast_edits
from tensorflow.tools.compatibility import renames_v2


class TFAPIChangeSpec(ast_edits.APIChangeSpec):
  """List of maps that describe what changed in the API."""

  def __init__(self):
    # Maps from a function name to a dictionary that describes how to
    # map from an old argument keyword to the new argument keyword.
    self.function_keyword_renames = {
        "tf.expand_dims": {
            "dim": "axis",
        },
        "tf.convert_to_tensor": {
            "preferred_dtype": "dtype_hint"
        },
        "tf.math.count_nonzero": {
            "input_tensor": "input",
            "keep_dims": "keepdims",
            "reduction_indices": "axis",
        },
        "tf.nn.pool": {
            "dilation_rate": "dilations"
        },
        "tf.nn.separable_conv2d": {
            "rate": "dilations"
        },
        "tf.nn.sufficient_statistics": {
            "keep_dims": "keepdims"
        },
        "tf.multinomial": {
            "output_dtype": "dtype",
        },
        "tf.random.multinomial": {
            "output_dtype": "dtype",
        },
        "tf.nn.conv3d": {
            "filter": "filters"
        },
        "tf.nn.conv3d_transpose": {
            "value": "input",
            "filter": "filters",
        },
        "tf.nn.convolution": {
            "filter": "filters",
            "dilation_rate": "dilations",
        }
    }

    # Mapping from function to the new name of the function
    self.symbol_renames = renames_v2.renames
    # pylint: disable=line-too-long
    # Add additional renames not in renames_v2.py here.
    # IMPORTANT: For the renames in here, if you also need to add to
    # function_reorders or function_keyword_renames, use the OLD function name.
    # These renames happen after the arguments have been processed.
    self.symbol_renames.update({
        "tf.contrib.data.AUTOTUNE": "tf.data.experimental.AUTOTUNE",
        "tf.contrib.data.Counter": "tf.data.experimental.Counter",
        "tf.contrib.data.CheckpointInputPipelineHook": "tf.data.experimental.CheckpointInputPipelineHook",
        "tf.contrib.data.CsvDataset": "tf.data.experimental.CsvDataset",
        "tf.contrib.data.Optional": "tf.data.experimental.Optional",
        "tf.contrib.data.RandomDataset": "tf.data.experimental.RandomDataset",
        "tf.contrib.data.Reducer": "tf.data.experimental.Reducer",
        "tf.contrib.data.SqlDataset": "tf.data.experimental.SqlDataset",
        "tf.contrib.data.StatsAggregator": "tf.data.experimental.StatsAggregator",
        "tf.contrib.data.TFRecordWriter": "tf.data.experimental.TFRecordWriter",
        "tf.contrib.data.assert_element_shape": "tf.data.experimental.assert_element_shape",
        "tf.contrib.data.batch_and_drop_remainder": "tf.compat.v1.contrib.data.batch_and_drop_remainder",
        "tf.contrib.data.bucket_by_sequence_length": "tf.data.experimental.bucket_by_sequence_length",
        "tf.contrib.data.choose_from_datasets": "tf.data.experimental.choose_from_datasets",
        "tf.contrib.data.copy_to_device": "tf.data.experimental.copy_to_device",
        "tf.contrib.data.dense_to_sparse_batch": "tf.data.experimental.dense_to_sparse_batch",
        "tf.contrib.data.enumerate_dataset": "tf.data.experimental.enumerate_dataset",
        "tf.contrib.data.get_next_as_optional": "tf.data.experimental.get_next_as_optional",
        "tf.contrib.data.get_single_element": "tf.data.experimental.get_single_element",
        "tf.contrib.data.group_by_reducer": "tf.data.experimental.group_by_reducer",
        "tf.contrib.data.group_by_window": "tf.data.experimental.group_by_window",
        "tf.contrib.data.ignore_errors": "tf.data.experimental.ignore_errors",
        "tf.contrib.data.latency_stats": "tf.data.experimental.latency_stats",
        "tf.contrib.data.make_batched_features_dataset": "tf.data.experimental.make_batched_features_dataset",
        "tf.contrib.data.make_csv_dataset": "tf.data.experimental.make_csv_dataset",
        "tf.contrib.data.make_saveable_from_iterator": "tf.data.experimental.make_saveable_from_iterator",
        "tf.contrib.data.map_and_batch": "tf.data.experimental.map_and_batch",
        "tf.contrib.data.padded_batch_and_drop_remainder": "tf.compat.v1.contrib.data.padded_batch_and_drop_remainder",
        "tf.contrib.data.parallel_interleave": "tf.data.experimental.parallel_interleave",
        "tf.contrib.data.parse_example_dataset": "tf.data.experimental.parse_example_dataset",
        "tf.contrib.data.prefetch_to_device": "tf.data.experimental.prefetch_to_device",
        "tf.contrib.data.read_batch_features": "tf.compat.v1.contrib.data.read_batch_features",
        "tf.contrib.data.reduce_dataset": "tf.compat.v1.contrib.data.reduce_dataset",
        "tf.contrib.data.rejection_resample": "tf.data.experimental.rejection_resample",
        "tf.contrib.data.sample_from_datasets": "tf.data.experimental.sample_from_datasets",
        "tf.contrib.data.scan": "tf.data.experimental.scan",
        "tf.contrib.data.set_stats_aggregator": "tf.data.experimental.set_stats_aggregator",
        "tf.contrib.data.shuffle_and_repeat": "tf.data.experimental.shuffle_and_repeat",
        "tf.contrib.data.sliding_window_batch": "tf.compat.v1.contrib.data.sliding_window_batch",
        "tf.contrib.data.sloppy_interleave": "tf.compat.v1.contrib.data.sloppy_interleave",
        "tf.contrib.data.unbatch": "tf.data.experimental.unbatch",
        "tf.contrib.data.unique": "tf.data.experimental.unique",
        "tf.quantize_v2": "tf.quantization.quantize",
        "tf.sparse_concat": "tf.sparse.concat",
        "tf.multinomial": "tf.random.categorical",
        "tf.random.multinomial": "tf.random.categorical",
        "tf.load_file_system_library": "tf.load_library",
    })
    # pylint: enable=line-too-long

    # For custom behavior and if auto-generate rename in renames_v2.py
    # is incorrect, add the op name here to exclude it from renames_v2.py.
    excluded_renames = [
    ]

    # Variables that should be changed to functions.
    self.change_to_function = {}

    # Functions that were reordered should be changed to the new keyword args
    # for safety, if positional arguments are used. If you have reversed the
    # positional arguments yourself, this could do the wrong thing.
    # IMPORTANT: order here should correspond to OLD argument order.
    # We just prepend "arg_name=" to all arguments in function calls.
    self.function_reorders = {
        "tf.argmax": ["input", "axis", "name", "dimension", "output_type"],
        "tf.argmin": ["input", "axis", "name", "dimension", "output_type"],
        "tf.boolean_mask": ["tensor", "mask", "name", "axis"],
        "tf.convert_to_tensor": ["value", "dtype", "name", "preferred_dtype"],
        "tf.nn.convolution": [
            "input", "filter", "padding", "strides", "dilation_rate", "name",
            "data_format"],
        "tf.nn.crelu": ["features", "name", "axis"],
        "tf.nn.pool": [
            "input", "window_shape", "pooling_type", "padding", "dilation_rate",
            "strides", "name", "data_format"
        ],
        "tf.multinomial": [
            "logits", "num_samples", "seed", "name", "output_dtype"
        ],
        "tf.random.multinomial": [
            "logits", "num_samples", "seed", "name", "output_dtype"
        ],
        "tf.pad": ["tensor", "paddings", "mode", "name", "constant_values"],
        "tf.quantize_v2": [
            "input", "min_range", "max_range", "T", "mode", "name",
            "round_mode"
        ],
        "tf.shape": ["input", "name", "out_type"],
        "tf.size": ["input", "name", "out_type"],
        "tf.sparse.concat": [
            "axis", "sp_inputs", "name", "expand_nonconcat_dim", "concat_dim"
        ],
    }

    # Specially handled functions.
    self.function_handle = {}

    decay_function_comment = (
        "ERROR: <function name> has been changed to return a callable instead "
        "of a tensor when graph building, but its functionality remains "
        "unchanged during eager execution (returns a callable like "
        "before). The converter cannot detect and fix this reliably, so "
        "you need to inspect this usage manually.\n"
    )

    # TODO(b/118888586): add default value change to update script.
    default_loss_reduction_changed = (
        "WARNING: default value of loss_reduction has been changed to "
        "SUM_OVER_BATCH_SIZE.\n"
    )

    assert_return_type_comment = (
        "WARNING: assert_* functions have been changed to return None, the "
        "data argument has been removed, and arguments have been reordered."
    )

    assert_rank_comment = (
        "WARNING: assert_rank_* functions have been changed to return None, and"
        " the data and summarize arguments have been removed."
    )

    # Function warnings. <function name> placeholder inside warnings will be
    # replaced by function name.
    self.function_warnings = {
        "tf.assert_greater": assert_return_type_comment,
        "tf.assert_equal": assert_return_type_comment,
        "tf.assert_less": assert_return_type_comment,
        "tf.assert_rank": assert_rank_comment,
        "tf.debugging.assert_equal": assert_return_type_comment,
        "tf.debugging.assert_greater": assert_return_type_comment,
        "tf.debugging.assert_greater_equal": assert_return_type_comment,
        "tf.debugging.assert_integer": assert_return_type_comment,
        "tf.debugging.assert_less": assert_return_type_comment,
        "tf.debugging.assert_less_equal": assert_return_type_comment,
        "tf.debugging.assert_near": assert_return_type_comment,
        "tf.debugging.assert_negative": assert_return_type_comment,
        "tf.debugging.assert_non_negative": assert_return_type_comment,
        "tf.debugging.assert_non_positive": assert_return_type_comment,
        "tf.debugging.assert_none_equal": assert_return_type_comment,
        "tf.debugging.assert_positive": assert_return_type_comment,
        "tf.debugging.assert_rank": assert_rank_comment,
        "tf.debugging.assert_rank_at_least": assert_rank_comment,
        "tf.debugging.assert_rank_in": assert_rank_comment,
        "tf.train.exponential_decay":
            decay_function_comment,
        "tf.train.piecewise_constant":
            decay_function_comment,
        "tf.train.polynomial_decay":
            decay_function_comment,
        "tf.train.natural_exp_decay":
            decay_function_comment,
        "tf.train.inverse_time_decay":
            decay_function_comment,
        "tf.train.cosine_decay":
            decay_function_comment,
        "tf.train.cosine_decay_restarts":
            decay_function_comment,
        "tf.train.linear_cosine_decay":
            decay_function_comment,
        "tf.train.noisy_linear_cosine_decay":
            decay_function_comment,
        "tf.estimator.LinearClassifier":
            default_loss_reduction_changed,
        "tf.estimator.LinearRegressor":
            default_loss_reduction_changed,
        "tf.estimator.DNNLinearCombinedClassifier":
            default_loss_reduction_changed,
        "tf.estimator.DNNLinearCombinedRegressor":
            default_loss_reduction_changed,
        "tf.estimator.DNNRegressor":
            default_loss_reduction_changed,
        "tf.estimator.DNNClassifier":
            default_loss_reduction_changed,
        "tf.estimator.BaselineClassifier":
            default_loss_reduction_changed,
        "tf.estimator.BaselineRegressor":
            default_loss_reduction_changed,
        "tf.nn.conv1d":
        "WARNING: use_cudnn_on_gpu argument has been removed and \"value\" was "
        "renamed to \"input\"",
        "tf.nn.conv2d":
        "WARNING: use_cudnn_on_gpu argument has been removed and \"filter\" "
        "was renamed to \"filters\"",
        "tf.nn.conv2d_backprop_filter":
        "WARNING: use_cudnn_on_gpu argument has been removed",
        "tf.nn.conv2d_backprop_input":
        "WARNING: use_cudnn_on_gpu argument has been removed and \"filter\" "
        "was renamed to \"filters\"",
    }
    # Right now we can't have both a rename and a warning.
    self.symbol_renames = {
        name: new_name
        for name, new_name in self.symbol_renames.items()
        if name not in self.function_warnings and name not in excluded_renames
    }


if __name__ == "__main__":
  parser = argparse.ArgumentParser(
      formatter_class=argparse.RawDescriptionHelpFormatter,
      description="""Convert a TensorFlow Python file to 2.0

Simple usage:
  tf_convert_v2.py --infile foo.py --outfile bar.py
  tf_convert_v2.py --intree ~/code/old --outtree ~/code/new
""")
  parser.add_argument(
      "--infile",
      dest="input_file",
      help="If converting a single file, the name of the file "
      "to convert")
  parser.add_argument(
      "--outfile",
      dest="output_file",
      help="If converting a single file, the output filename.")
  parser.add_argument(
      "--intree",
      dest="input_tree",
      help="If converting a whole tree of files, the directory "
      "to read from (relative or absolute).")
  parser.add_argument(
      "--outtree",
      dest="output_tree",
      help="If converting a whole tree of files, the output "
      "directory (relative or absolute).")
  parser.add_argument(
      "--copyotherfiles",
      dest="copy_other_files",
      help=("If converting a whole tree of files, whether to "
            "copy the other files."),
      type=bool,
      default=False)
  parser.add_argument(
      "--reportfile",
      dest="report_filename",
      help=("The name of the file where the report log is "
            "stored."
            "(default: %(default)s)"),
      default="report.txt")
  args = parser.parse_args()

  upgrade = ast_edits.ASTCodeUpgrader(TFAPIChangeSpec())
  report_text = None
  report_filename = args.report_filename
  files_processed = 0
  if args.input_file:
    if not args.output_file:
      raise ValueError(
          "--outfile=<output file> argument is required when converting a "
          "single file.")
    files_processed, report_text, errors = upgrade.process_file(
        args.input_file, args.output_file)
    files_processed = 1
  elif args.input_tree:
    if not args.output_tree:
      raise ValueError(
          "--outtree=<output directory> argument is required when converting a "
          "file tree.")
    files_processed, report_text, errors = upgrade.process_tree(
        args.input_tree, args.output_tree, args.copy_other_files)
  else:
    parser.print_help()
  if report_text:
    open(report_filename, "w").write(report_text)
    print("TensorFlow 2.0 Upgrade Script")
    print("-----------------------------")
    print("Converted %d files\n" % files_processed)
    print("Detected %d errors that require attention" % len(errors))
    print("-" * 80)
    print("\n".join(errors))
    print("\nMake sure to read the detailed log %r\n" % report_filename)
