# Copyright 2021 The TensorFlow Authors. All Rights Reserved.
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
"""Python TF-Lite QuantizationDebugger."""
import collections
from typing import Callable, Dict, Iterable, List, Mapping, Optional, Sequence

import numpy as np
import tensorflow as tf

from tensorflow.python.util import tf_export

# Returns metrics based on difference of values for quantized/float ops.
_DEFAULT_LAYER_DEBUG_METRICS = {
    'num_elements': lambda diffs: diffs.size,
    'stddev': np.std,
    'mean_error': np.average,
    'max_abs_error': lambda diffs: np.max(np.abs(diffs)),
    'mean_square_error': lambda diffs: np.average(diffs**2),
}


@tf_export.tf_export(v1=['lite.experimental.QuantizationDebugOptions'])
class QuantizationDebugOptions:
  """Debug options to set up a given QuantizationDebugger."""

  def __init__(self,
               layer_debug_metrics: Optional[Mapping[str,
                                                     Callable[[np.ndarray],
                                                              float]]] = None,
               model_debug_metrics: Optional[Mapping[
                   str, Callable[[Sequence[np.ndarray], Sequence[np.ndarray]],
                                 float]]] = None):
    """Initializes debugger options.

    Args:
      layer_debug_metrics: a dict to specify layer debug functions
        {function_name_str: function} where the function accepts result of
          NumericVerify Op, which is value difference between float and
          dequantized op results. The function returns single scalar value.
      model_debug_metrics: a dict to specify model debug functions
        {function_name_str: function} where the function accepts outputs from
          two models, and returns single scalar value for a metric. (e.g.
          accuracy, IoU)
    """
    self.layer_debug_metrics = layer_debug_metrics
    self.model_debug_metrics = model_debug_metrics


@tf_export.tf_export(v1=['lite.experimental.QuantizationDebugger'])
class QuantizationDebugger:
  """Debugger for Quantized TensorFlow Lite debug mode models.

  This can run the TensorFlow Lite converted models equipped with debug ops and
  collect debug information. This debugger calculates statistics from
  user-defined post-processing functions as well as default ones.
  """

  def __init__(
      self,
      quant_debug_model_path: Optional[str] = None,
      quant_debug_model_content: Optional[bytes] = None,
      float_model_path: Optional[str] = None,
      float_model_content: Optional[bytes] = None,
      debug_dataset: Optional[Callable[[],
                                       Iterable[Sequence[np.ndarray]]]] = None,
      debug_options: Optional[QuantizationDebugOptions] = None) -> None:
    """Runs the TFLite debugging model with given debug options.

    Args:
      quant_debug_model_path: Path to the quantized debug TFLite model file.
      quant_debug_model_content: Content of the quantized debug TFLite model.
      float_model_path: Path to float TFLite model file.
      float_model_content: Content of the float TFLite model.
      debug_dataset: a factory function that returns dataset generator which is
        used to generate input samples (list of np.ndarray) for the model. The
        generated elements must have same types and shape as inputs to the
        model.
      debug_options: Debug options to debug the given model.

    Raises:
      ValueError: If the debugger was unable to be created.

    Attributes:
      layer_statistics: results of error metrics for each NumericVerify op
        results. in {layer_name: {metric_name: metric}} format.
      model_statistics: results of error metrics for difference between float
        and quantized models. in {metric_name: metric} format.
    """
    self._data_gen = debug_dataset
    self._debug_options = debug_options or QuantizationDebugOptions()

    input_data = next(iter(self._data_gen()))
    self._quant_interpreter = tf.lite.Interpreter(quant_debug_model_path,
                                                  quant_debug_model_content)
    if self._debug_options.model_debug_metrics:
      self._float_interpreter = tf.lite.Interpreter(float_model_path,
                                                    float_model_content)

    self._numeric_verify_tensor_details = None
    if not self._get_numeric_verify_tensor_details():
      raise ValueError('Please check if the quantized model is in debug mode')

    self._layer_debug_metrics = _DEFAULT_LAYER_DEBUG_METRICS.copy()
    if self._debug_options.layer_debug_metrics:
      self._layer_debug_metrics.update(self._debug_options.layer_debug_metrics)

    self.layer_statistics = None
    self.model_statistics = None

  def run(self) -> None:
    """Runs models and gets metrics."""
    self.layer_statistics = self._collect_layer_statistics()
    if self._debug_options.model_debug_metrics:
      self.model_statistics = self._collect_model_statistics()

  def _collect_layer_statistics(self) -> Dict[str, Dict[str, float]]:
    """Collects layer statistics by applying layer debug metrics.

    For all data from the given RepresentativeDataset, collect statistics per
    example by getting the NumericVerify op results in _quant_interpreter
    and calculating layer debug metrics on the results.

    Returns:
      aggregated per-layer statistics of NumericVerify results.
      {layer_name: {metric_name: metric}}
    """
    layer_statistics = collections.defaultdict(
        lambda: collections.defaultdict(list))

    initialize = True
    for tensor_data in self._data_gen():
      self._set_input_tensors(self._quant_interpreter, tensor_data, initialize)
      initialize = False

      # Run the model.
      self._quant_interpreter.invoke()

      # Collect the statistics of this invoke result.
      for tensor_details in self._get_numeric_verify_tensor_details():
        tensor_name = tensor_details['name']
        diffs = self._quant_interpreter.get_tensor(tensor_details['index'])
        for metric_name, metric_fn in self._layer_debug_metrics.items():
          layer_statistics[tensor_name][metric_name].append(metric_fn(diffs))

    # Calculate final aggregated metrics for each layer.
    for metrics in layer_statistics.values():
      for metric_name in metrics:
        metrics[metric_name] = np.mean(metrics[metric_name])

    return layer_statistics

  def _collect_model_statistics(self) -> Dict[str, float]:
    """Collects model output metrics.

    For all data from the given RepresentativeDataset, collect all model output
    results from float model & quantized debug model, and calculate metrics
    by using model output functions. As a result, self.model_results is filled,

    where self.model_results[model_output_function_name] = `aggregated model
    output function value` (a scalar).

    Returns:
      aggregated per-model output discrepancy mertics.
      {metric_name: aggregated_metric}
    """

    model_statistics = collections.defaultdict(list)

    initialize = True
    for tensor_data in self._data_gen():
      self._set_input_tensors(self._quant_interpreter, tensor_data, initialize)
      self._set_input_tensors(self._float_interpreter, tensor_data, initialize)
      initialize = False

      # Run the models.
      self._quant_interpreter.invoke()
      self._float_interpreter.invoke()

      # Collect the output results from both models.
      float_tensor_data = self._get_output_tensors(self._float_interpreter)
      quant_tensor_data = self._get_output_tensors(self._quant_interpreter)

      # Calculate the metrics.
      for (metric_name,
           metric_fn) in self._debug_options.model_debug_metrics.items():
        model_statistics[metric_name].append(
            metric_fn(float_tensor_data, quant_tensor_data))

    # Calculate final aggregated metrics for each outputs.
    return {
        metric_name: np.mean(metric)
        for metric_name, metric in model_statistics.items()
    }

  def _set_input_tensors(
      self,
      interpreter: tf.lite.Interpreter,
      tensor_data: Sequence[np.ndarray],
      initialize: bool,
  ) -> None:
    """Sets input tensors into TFLite model Interpreter.

    Args:
      interpreter: a tf.lite.Interpreter object with allocated tensors.
      tensor_data: a list of Numpy array data.
      initialize: set to true when input is first set for the interpreter, to
        set input shapes and allocate tensors.

    Raises:
      ValueError: when inputs can't be set, or size of provided inputs does not
      match size of model inputs.
    """
    input_indices = [
        detail['index'] for detail in interpreter.get_input_details()
    ]
    if len(input_indices) != len(tensor_data):
      raise ValueError(
          'Number of inputs provided ({}) does not match number of inputs to '
          'the model ({})'.format(len(tensor_data), len(input_indices)))

    if initialize:
      for input_idx, tensor in zip(input_indices, tensor_data):
        interpreter.resize_tensor_input(input_idx, tensor.shape)
      interpreter.allocate_tensors()

    for input_idx, tensor in zip(input_indices, tensor_data):
      interpreter.set_tensor(input_idx, tensor)

  def _get_output_tensors(self,
                          interpreter: tf.lite.Interpreter) -> List[np.ndarray]:
    """Returns output tensors of given TFLite model Interpreter.

    Args:
      interpreter: a tf.lite.Interpreter object with allocated tensors.

    Returns:
      a list of numpy arrays representing output tensor results.
    """
    return [
        interpreter.get_tensor(tensor['index'])
        for tensor in interpreter.get_output_details()
    ]

  def _get_numeric_verify_tensor_details(self) -> List[str]:
    """Returns all names of all tensors from NumericVerify op."""
    if not self._numeric_verify_tensor_details:
      self._numeric_verify_tensor_details = [
          detail for detail in self._quant_interpreter.get_tensor_details()
          if detail['name'].startswith('NumericVerify')
      ]
    return self._numeric_verify_tensor_details
