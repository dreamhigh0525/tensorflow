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
"""Tests for quantize_model."""
from typing import Iterable, List, Mapping, Optional, Sequence, Set, Tuple
import warnings

from absl.testing import parameterized
import numpy as np
import tensorflow  # pylint: disable=unused-import

from tensorflow.compiler.mlir.quantization.tensorflow import quantization_options_pb2 as quant_opts_pb2
from tensorflow.compiler.mlir.quantization.tensorflow.python import quantize_model
from tensorflow.compiler.mlir.quantization.tensorflow.python import representative_dataset as repr_dataset
from tensorflow.core.framework import function_pb2
from tensorflow.core.framework import node_def_pb2
from tensorflow.core.protobuf import meta_graph_pb2
from tensorflow.python.client import session
from tensorflow.python.data.ops import dataset_ops
from tensorflow.python.eager import def_function
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.framework import tensor_spec
from tensorflow.python.framework import test_util
from tensorflow.python.module import module
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import nn_ops
from tensorflow.python.ops import random_ops
from tensorflow.python.ops import variables
from tensorflow.python.platform import test
from tensorflow.python.saved_model import builder
from tensorflow.python.saved_model import loader_impl as saved_model_loader
from tensorflow.python.saved_model import save as saved_model_save
from tensorflow.python.saved_model import signature_constants
from tensorflow.python.saved_model import signature_def_utils_impl
from tensorflow.python.saved_model import tag_constants
from tensorflow.python.trackable import autotrackable
from tensorflow.python.types import core

# Type aliases for quantization method protobuf enums.
_Method = quant_opts_pb2.QuantizationMethod.Method
_ExperimentalMethod = quant_opts_pb2.QuantizationMethod.ExperimentalMethod


def _is_quantized_function(func: function_pb2.FunctionDef) -> bool:
  """Determine whether a FunctionDef is quantized.

  Args:
    func: A FunctionDef object.

  Returns:
    True iff `func` is quantized.
  """
  return func.signature.name.startswith('quantized_')


def _contains_op_with_name(nodes: Iterable[node_def_pb2.NodeDef],
                           op_name: str) -> bool:
  """Determine whether there is a node whose operation name matches `op_name`.

  Args:
    nodes: Iterable of NodeDefs.
    op_name: Name of the op to match.

  Returns:
    True iff there exists a node whose name matches `op_name`.
  """
  return any(node.op == op_name for node in nodes)


def _contains_quantized_function_call(
    meta_graphdef: meta_graph_pb2.MetaGraphDef) -> bool:
  """Determines if the graph def has quantized function call.

  Args:
    meta_graphdef: A MetaGraphDef object.

  Returns:
    True if and only if the graph def contains a quantized function call.
  """
  return any(
      map(_is_quantized_function, meta_graphdef.graph_def.library.function))


def _contains_op(meta_graphdef: meta_graph_pb2.MetaGraphDef,
                 op_name: str) -> bool:
  """Determines if the graph def contains the given op.

  Args:
    meta_graphdef: A MetaGraphDef object.
    op_name: Name of the operation to find within the graph.

  Returns:
    True if and only if the graph def contains an op named `op_name`.
  """
  # Check the main graph
  if _contains_op_with_name(
      nodes=meta_graphdef.graph_def.node, op_name=op_name):
    return True

  # Check the graph genederated from user defined functions
  return any(
      _contains_op_with_name(nodes=func.node_def, op_name=op_name)
      for func in meta_graphdef.graph_def.library.function)


def _create_simple_tf1_conv_model(
    use_variable_for_filter=False) -> Tuple[core.Tensor, core.Tensor]:
  """Creates a basic convolution model.

  This is intended to be used for TF1 (graph mode) tests.

  Args:
    use_variable_for_filter: Setting this to `True` makes the filter for the
      conv operation a `tf.Variable`.

  Returns:
    in_placeholder: Input tensor placeholder.
    output_tensor: The resulting tensor of the convolution operation.
  """
  in_placeholder = array_ops.placeholder(dtypes.float32, shape=[1, 3, 4, 3])

  filters = random_ops.random_uniform(shape=(2, 3, 3, 2), minval=-1., maxval=1.)
  if use_variable_for_filter:
    filters = variables.Variable(filters)

  output_tensor = nn_ops.conv2d(
      in_placeholder,
      filters,
      strides=[1, 1, 2, 1],
      dilations=[1, 1, 1, 1],
      padding='SAME',
      data_format='NHWC')

  return in_placeholder, output_tensor


def _create_simple_tf1_gather_model(
    use_variable_for_filter=False) -> Tuple[core.Tensor, core.Tensor]:
  """Creates a basic gather model.

  This is intended to be used for TF1 (graph mode) tests.

  Args:
    use_variable_for_filter: Setting this to `True` makes the filter for the
      gather operation a `tf.Variable`.

  Returns:
    in_placeholder: Input tensor placeholder.
    output_tensor: The resulting tensor of the gather operation.
  """
  in_placeholder = array_ops.placeholder(dtypes.int64, shape=(6))

  filters = random_ops.random_uniform(shape=(64, 512), minval=-1., maxval=1.)
  if use_variable_for_filter:
    filters = variables.Variable(filters)

  output_tensor = array_ops.gather_v2(filters, in_placeholder)

  return in_placeholder, output_tensor


def _create_data_generator(
    input_key: str,
    shape: Sequence[int],
    minval=-1.,
    maxval=1.,
    dtype=dtypes.float32,
    num_examples=8) -> repr_dataset.RepresentativeDataset:
  """Creates a data generator to be used as representative dataset.

  Supports generating random value input tensors mapped by the `input_key`.

  Args:
    input_key: The string key that identifies the created tensor as an input.
    shape: Shape of the tensor data.
    minval: The lower bound of the generated input
    maxval: The upper bound of the generated input
    dtype: The type of the generated input - usually dtypes.float32 for float
      and dtypes.int64 for int
    num_examples: Number of examples in the representative dataset.

  Yields:
    data_gen: A `quantize_model._RepresentativeSample` filled with random
      values.
  """
  for _ in range(num_examples):
    yield {input_key: random_ops.random_uniform(shape, minval, maxval, dtype)}


def _save_tf1_model(sess: session.Session, saved_model_path: str,
                    signature_key: str, tags: Set[str],
                    inputs: Mapping[str, core.Tensor],
                    outputs: Mapping[str, core.Tensor]) -> None:
  """Saves a TF1 model.

  Args:
    sess: Current tf.Session object.
    saved_model_path: Directory to save the model.
    signature_key: The key to the SignatureDef that inputs & outputs correspond
      to.
    tags: Set of tags associated with the model.
    inputs: Input name -> input tensor mapping.
    outputs: Output name -> output tensor mapping.
  """
  v1_builder = builder.SavedModelBuilder(saved_model_path)
  sig_def = signature_def_utils_impl.predict_signature_def(
      inputs=inputs, outputs=outputs)

  v1_builder.add_meta_graph_and_variables(
      sess, tags, signature_def_map={signature_key: sig_def})
  v1_builder.save()


def _create_and_save_tf1_gather_model(saved_model_path: str,
                                      signature_key: str,
                                      tags: Set[str],
                                      input_key: str,
                                      output_key: str,
                                      use_variable=False) -> core.Tensor:
  """Creates and saves a simple gather model.

  This is intended to be used for TF1 (graph mode) tests.

  Args:
    saved_model_path: Directory to save the model.
    signature_key: The key to the SignatureDef that inputs & outputs correspond
      to.
    tags: Set of tags associated with the model.
    input_key: The key to the input tensor.
    output_key: The key to the output tensor.
    use_variable: Setting this to `True` makes the filter for the gather
      operation a `tf.Variable`.

  Returns:
    in_placeholder: The placeholder tensor used as an input to the model.
  """
  with ops.Graph().as_default(), session.Session() as sess:
    in_placeholder, output_tensor = _create_simple_tf1_gather_model(
        use_variable_for_filter=use_variable)

    if use_variable:
      sess.run(variables.global_variables_initializer())

    _save_tf1_model(
        sess,
        saved_model_path,
        signature_key,
        tags,
        inputs={input_key: in_placeholder},
        outputs={output_key: output_tensor})

    return in_placeholder


def _create_gather_model(use_variable):

  class GatherModel(autotrackable.AutoTrackable):
    """A simple model with a single gather."""

    def __init__(self, use_variable):
      """Initializes a GatherModel.

      Args:
        use_variable: If True, creates a variable for weight.
      """
      super(GatherModel, self).__init__()
      w_val = np.random.randint(low=0, high=100, size=(64, 512), dtype=np.int64)
      if use_variable:
        self.w = variables.Variable(w_val)
      else:
        self.w = w_val

    @def_function.function(input_signature=[
        tensor_spec.TensorSpec(
            shape=[6], dtype=dtypes.int64, name='input_tensor')
    ])
    def __call__(self, input_tensor: core.Tensor) -> Mapping[str, core.Tensor]:
      """Performs a gather operation."""
      out = array_ops.gather_v2(self.w, input_tensor)
      return {'output': out}

  return GatherModel(use_variable)


def _create_and_save_tf1_conv_model(saved_model_path: str,
                                    signature_key: str,
                                    tags: Set[str],
                                    input_key: str,
                                    output_key: str,
                                    use_variable=False) -> core.Tensor:
  """Creates and saves a simple convolution model.

  This is intended to be used for TF1 (graph mode) tests.

  Args:
    saved_model_path: Directory to save the model.
    signature_key: The key to the SignatureDef that inputs & outputs correspond
      to.
    tags: Set of tags associated with the model.
    input_key: The key to the input tensor.
    output_key: The key to the output tensor.
    use_variable: Setting this to `True` makes the filter for the conv operation
      a `tf.Variable`.

  Returns:
    in_placeholder: The placeholder tensor used as an input to the model.
  """
  with ops.Graph().as_default(), session.Session() as sess:
    in_placeholder, output_tensor = _create_simple_tf1_conv_model(
        use_variable_for_filter=use_variable)

    if use_variable:
      sess.run(variables.global_variables_initializer())

    _save_tf1_model(
        sess,
        saved_model_path,
        signature_key,
        tags,
        inputs={input_key: in_placeholder},
        outputs={output_key: output_tensor})

  return in_placeholder


class MatmulModel(module.Module):
  """A simple model with a single matmul.

  Bias and activation function are optional.
  """

  def __init__(self,
               has_bias: bool = False,
               activation_fn: Optional[ops.Operation] = None) -> None:
    """Initializes a MatmulModel.

    Args:
      has_bias: If True, creates and adds a bias term.
      activation_fn: The activation function to be used. No activation function
        if None.
    """
    self.has_bias = has_bias
    self.activation_fn = activation_fn

  @def_function.function(input_signature=[
      tensor_spec.TensorSpec(
          shape=(1, 4), dtype=dtypes.float32, name='input_tensor')
  ])
  def matmul(self, input_tensor: core.Tensor) -> Mapping[str, core.Tensor]:
    """Performs a matrix multiplication.

    Depending on self.has_bias and self.activation_fn, it may add a bias term or
    go through the activaction function.

    Args:
      input_tensor: Input tensor to matmul with the filter.

    Returns:
      A map of: output key -> output result.
    """
    filters = np.random.uniform(low=-1.0, high=1.0, size=(4, 3))
    bias = np.random.uniform(low=-1.0, high=1.0, size=(3,))
    out = math_ops.matmul(input_tensor, filters)

    if self.has_bias:
      out = nn_ops.bias_add(out, bias)

    if self.activation_fn is not None:
      out = self.activation_fn(out)

    return {'output': out}


class MultipleSignatureModel(module.Module):
  """A model with 2 signatures.

  Used to test where the quantizer has to handle multiple signatures.
  """

  @def_function.function(input_signature=[
      tensor_spec.TensorSpec(shape=[1, 4], dtype=dtypes.float32)
  ])
  def matmul(self, matmul_input: core.Tensor) -> Mapping[str, core.Tensor]:
    """Performs a matrix multiplication.

    Args:
      matmul_input: Input tensor to matmul with the filter.

    Returns:
      A map of: output key -> output result.
    """
    filters = random_ops.random_uniform(shape=(4, 3), minval=-1.0, maxval=1.0)
    out = math_ops.matmul(matmul_input, filters)

    return {'output': out}

  @def_function.function(input_signature=[
      tensor_spec.TensorSpec(shape=(1, 3, 4, 3), dtype=dtypes.float32)
  ])
  def conv(self, conv_input: core.Tensor) -> Mapping[str, core.Tensor]:
    """Performs a 2D convolution operation.

    Args:
      conv_input: Input tensor to perform convolution on.

    Returns:
      A map of: output key -> output result.
    """
    filters = np.random.uniform(
        low=-10, high=10, size=(2, 3, 3, 2)).astype('f4')
    out = nn_ops.conv2d(
        conv_input,
        filters,
        strides=[1, 1, 2, 1],
        dilations=[1, 1, 1, 1],
        padding='SAME',
        data_format='NHWC')

    return {'output': out}


@test_util.run_all_in_graph_and_eager_modes
class QuantizationMethodTest(test.TestCase):
  """Test cases regarding the use of QuantizationMethod proto.

  Run all tests cases in both the graph mode (default in TF1) and the eager mode
  (default in TF2) to ensure support for when TF2 is disabled.
  """

  class SimpleModel(module.Module):

    @def_function.function(input_signature=[
        tensor_spec.TensorSpec(shape=[1, 4], dtype=dtypes.float32)
    ])
    def __call__(self, input_tensor: core.Tensor) -> Mapping[str, core.Tensor]:
      """Performs a matrix multiplication.

      Args:
        input_tensor: Input tensor to matmul with the filter.

      Returns:
        A map of: output key -> output result.
      """
      filters = np.random.uniform(low=-1.0, high=1.0, size=(4, 3)).astype('f4')

      out = math_ops.matmul(input_tensor, filters)
      return {'output': out}

  def _simple_model_data_gen(self) -> repr_dataset.RepresentativeDataset:
    """Creates an interable of representative samples.

    Yields:
      Representative samples, which is basically a mapping of: input key ->
      input value.
    """
    for _ in range(8):
      yield {
          'input_tensor':
              ops.convert_to_tensor(
                  np.random.uniform(low=0, high=150, size=(1, 4)).astype('f4')),
      }

  def test_static_range_quantization_by_default(self):
    model = self.SimpleModel()

    input_saved_model_path = self.create_tempdir('input').full_path
    saved_model_save.save(model, input_saved_model_path)

    # Use default QuantizationOptions.
    converted_model = quantize_model.quantize(
        input_saved_model_path,
        representative_dataset=self._simple_model_data_gen())

    self.assertIsNotNone(converted_model)
    self.assertCountEqual(converted_model.signatures._signatures.keys(),
                          {'serving_default'})

    # Indirectly prove that it is performing a static-range quantization
    # by checking that it complains about representative_dataset when it is
    # not provided.
    with self.assertRaisesRegex(ValueError, 'representative_dataset'):
      quantize_model.quantize(input_saved_model_path)

  def test_method_unspecified_raises_value_error(self):
    model = self.SimpleModel()

    input_saved_model_path = self.create_tempdir('input').full_path
    saved_model_save.save(model, input_saved_model_path)

    options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            method=_Method.METHOD_UNSPECIFIED))

    with self.assertRaises(ValueError):
      quantize_model.quantize(
          input_saved_model_path, quantization_options=options)

  def test_invalid_method_raises_value_error(self):
    model = self.SimpleModel()

    input_saved_model_path = self.create_tempdir('input').full_path
    saved_model_save.save(model, input_saved_model_path)

    # Set an invalid value of -1 to QuantizationMethod.method.
    options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(method=-1))

    with self.assertRaises(ValueError):
      quantize_model.quantize(
          input_saved_model_path, quantization_options=options)


class StaticRangeQuantizationTest(test.TestCase, parameterized.TestCase):

  def _any_warning_contains(
      self, substring: str,
      warnings_list: List[warnings.WarningMessage]) -> bool:
    """Returns True if any of the warnings contains a given substring.

    Args:
      substring: A piece of string to check whether it exists in the warning
        message.
      warnings_list: A list of `WarningMessage`s.

    Returns:
      True if and only if the substring exists in any of the warnings in
      `warnings_list`.
    """
    return any(
        map(lambda warning: substring in str(warning.message), warnings_list))

  @parameterized.named_parameters(
      ('none', None, False),
      ('relu', nn_ops.relu, False),
      ('relu6', nn_ops.relu6, False),
      ('with_bias', None, True),
      ('with_bias_and_relu', nn_ops.relu, True),
      ('with_bias_and_relu6', nn_ops.relu6, True),
  )
  @test_util.run_in_graph_and_eager_modes
  def test_qat_conv_model(self, activation_fn: Optional[ops.Operation],
                          has_bias: bool):

    class ConvModel(module.Module):

      @def_function.function(input_signature=[
          tensor_spec.TensorSpec(
              name='input', shape=[1, 3, 4, 3], dtype=dtypes.float32),
          tensor_spec.TensorSpec(
              name='filter', shape=[2, 3, 3, 2], dtype=dtypes.float32),
      ])
      def conv(self, input_tensor: core.Tensor,
               filter_tensor: core.Tensor) -> Mapping[str, core.Tensor]:
        """Performs a 2D convolution operation.

        Args:
          input_tensor: Input tensor to perform convolution on.
          filter_tensor: Filter tensor to perform convolution with.

        Returns:
          A map of: output key -> output result.
        """
        q_input = array_ops.fake_quant_with_min_max_args(
            input_tensor, min=-0.1, max=0.2, num_bits=8, narrow_range=False)
        q_filters = array_ops.fake_quant_with_min_max_args(
            filter_tensor, min=-1.0, max=2.0, num_bits=8, narrow_range=False)
        bias = array_ops.constant([0, 0], dtype=dtypes.float32)
        out = nn_ops.conv2d(
            q_input,
            q_filters,
            strides=[1, 1, 2, 1],
            dilations=[1, 1, 1, 1],
            padding='SAME',
            data_format='NHWC')
        if has_bias:
          out = nn_ops.bias_add(out, bias, data_format='NHWC')
        if activation_fn is not None:
          out = activation_fn(out)
        q_out = array_ops.fake_quant_with_min_max_args(
            out, min=-0.3, max=0.4, num_bits=8, narrow_range=False)
        return {'output': q_out}

    model = ConvModel()
    input_saved_model_path = self.create_tempdir('input').full_path
    saved_model_save.save(model, input_saved_model_path)

    signature_key = signature_constants.DEFAULT_SERVING_SIGNATURE_DEF_KEY
    tags = [tag_constants.SERVING]
    output_directory = self.create_tempdir().full_path

    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.STATIC_RANGE))

    converted_model = quantize_model.quantize(input_saved_model_path,
                                              [signature_key], tags,
                                              output_directory,
                                              quantization_options)
    self.assertIsNotNone(converted_model)
    self.assertCountEqual(converted_model.signatures._signatures.keys(),
                          {signature_key})

    input_data = np.random.uniform(
        low=-0.1, high=0.2, size=(1, 3, 4, 3)).astype('f4')
    filter_data = np.random.uniform(
        low=-0.5, high=0.5, size=(2, 3, 3, 2)).astype('f4')

    expected_outputs = model.conv(input_data, filter_data)
    got_outputs = converted_model.signatures[signature_key](
        input=ops.convert_to_tensor(input_data),
        filter=ops.convert_to_tensor(filter_data))
    # TODO(b/215633216): Check if the accuracy is acceptable.
    self.assertAllClose(expected_outputs, got_outputs, atol=0.01)

    output_loader = saved_model_loader.SavedModelLoader(output_directory)
    output_meta_graphdef = output_loader.get_meta_graph_def_from_tags(tags)
    self.assertTrue(_contains_quantized_function_call(output_meta_graphdef))

  # Run this test only with the eager mode.
  @test_util.run_v2_only
  def test_ptq_model_with_variable(self):

    class ConvModelWithVariable(module.Module):
      """A simple model that performs a single convolution to the input tensor.

      It keeps the filter as a tf.Variable.
      """

      def __init__(self) -> None:
        """Initializes the filter variable."""
        self.filters = variables.Variable(
            random_ops.random_uniform(
                shape=(2, 3, 3, 2), minval=-1., maxval=1.))

      @def_function.function(input_signature=[
          tensor_spec.TensorSpec(
              name='input', shape=(1, 3, 4, 3), dtype=dtypes.float32),
      ])
      def __call__(self, x: core.Tensor) -> Mapping[str, core.Tensor]:
        """Performs a 2D convolution operation.

        Args:
          x: Input tensor to perform convolution on.

        Returns:
          A map of: output key -> output result.
        """
        out = nn_ops.conv2d(
            x,
            self.filters,
            strides=[1, 1, 2, 1],
            dilations=[1, 1, 1, 1],
            padding='SAME',
            data_format='NHWC')
        return {'output': out}

    def gen_data() -> repr_dataset.RepresentativeDataset:
      """Creates an interable of representative samples.

      Yields:
        Representative samples, which is basically a mapping of: input key ->
        input value.
      """
      for _ in range(8):
        yield {
            'input':
                random_ops.random_uniform(
                    shape=(1, 3, 4, 3), minval=0, maxval=150)
        }

    model = ConvModelWithVariable()
    input_saved_model_path = self.create_tempdir('input').full_path
    saved_model_save.save(model, input_saved_model_path)

    signature_keys = [signature_constants.DEFAULT_SERVING_SIGNATURE_DEF_KEY]
    tags = {tag_constants.SERVING}
    output_directory = self.create_tempdir().full_path

    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.STATIC_RANGE))

    converted_model = quantize_model.quantize(
        input_saved_model_path,
        signature_keys,
        tags,
        output_directory,
        quantization_options,
        representative_dataset=gen_data())

    self.assertIsNotNone(converted_model)
    self.assertCountEqual(converted_model.signatures._signatures.keys(),
                          signature_keys)

    output_loader = saved_model_loader.SavedModelLoader(output_directory)
    output_meta_graphdef = output_loader.get_meta_graph_def_from_tags(tags)
    self.assertTrue(_contains_quantized_function_call(output_meta_graphdef))

  @parameterized.named_parameters(
      ('none', None, False, False),
      ('relu', nn_ops.relu, False, False),
      ('relu6', nn_ops.relu6, False, False),
      ('bn', None, False, True),
      ('bn_and_relu', nn_ops.relu, False, True),
      ('with_bias', None, True, False),
      ('with_bias_and_bn', None, True, True),
      ('with_bias_and_bn_and_relu', nn_ops.relu, True, True),
      ('with_bias_and_relu', nn_ops.relu, True, False),
      ('with_bias_and_relu6', nn_ops.relu6, True, False),
  )
  @test_util.run_in_graph_and_eager_modes
  def test_conv_ptq_model(self, activation_fn: Optional[ops.Operation],
                          has_bias: bool, has_bn: bool):

    class ConvModel(module.Module):

      @def_function.function(input_signature=[
          tensor_spec.TensorSpec(shape=[1, 3, 4, 3], dtype=dtypes.float32)
      ])
      def conv(self, input_tensor: core.Tensor) -> Mapping[str, core.Tensor]:
        """Performs a 2D convolution operation.

        Args:
          input_tensor: Input tensor to perform convolution on.

        Returns:
          A map of: output key -> output result.
        """
        filters = np.random.uniform(
            low=-10, high=10, size=(2, 3, 3, 2)).astype('f4')
        bias = np.random.uniform(low=0, high=10, size=(2)).astype('f4')
        scale, offset = [1.0, 1.0], [0.5, 0.5]
        mean, variance = scale, offset
        out = nn_ops.conv2d(
            input_tensor,
            filters,
            strides=[1, 1, 2, 1],
            dilations=[1, 1, 1, 1],
            padding='SAME',
            data_format='NHWC')
        if has_bias:
          out = nn_ops.bias_add(out, bias)
        if has_bn:
          # Fusing is supported for non-training case.
          out, _, _, _, _, _ = nn_ops.fused_batch_norm_v3(
              out, scale, offset, mean, variance, is_training=False)
        if activation_fn is not None:
          out = activation_fn(out)
        return {'output': out}

    model = ConvModel()
    input_saved_model_path = self.create_tempdir('input').full_path
    saved_model_save.save(model, input_saved_model_path)

    def data_gen() -> repr_dataset.RepresentativeDataset:
      for _ in range(8):
        yield {
            'input_tensor':
                ops.convert_to_tensor(
                    np.random.uniform(low=0, high=150,
                                      size=(1, 3, 4, 3)).astype('f4')),
        }

    tags = [tag_constants.SERVING]
    output_directory = self.create_tempdir().full_path

    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.STATIC_RANGE))

    converted_model = quantize_model.quantize(
        input_saved_model_path, ['serving_default'],
        tags,
        output_directory,
        quantization_options,
        representative_dataset=data_gen())
    self.assertIsNotNone(converted_model)
    self.assertCountEqual(converted_model.signatures._signatures.keys(),
                          {'serving_default'})

    output_loader = saved_model_loader.SavedModelLoader(output_directory)
    output_meta_graphdef = output_loader.get_meta_graph_def_from_tags(tags)
    self.assertTrue(_contains_quantized_function_call(output_meta_graphdef))
    self.assertFalse(_contains_op(output_meta_graphdef, 'FusedBatchNormV3'))

  @parameterized.named_parameters(
      ('none', None, False, False),
      ('relu', nn_ops.relu, False, False),
      ('relu6', nn_ops.relu6, False, False),
      ('bn', None, False, True),
      ('bn_and_relu', nn_ops.relu, False, True),
      ('with_bias', None, True, False),
      ('with_bias_and_bn', None, True, True),
      ('with_bias_and_bn_and_relu', nn_ops.relu, True, True),
      ('with_bias_and_relu', nn_ops.relu, True, False),
      ('with_bias_and_relu6', nn_ops.relu6, True, False),
  )
  @test_util.run_in_graph_and_eager_modes
  def test_depthwise_conv_ptq_model(self,
                                    activation_fn: Optional[ops.Operation],
                                    has_bias: bool, has_bn: bool):

    class DepthwiseConvModel(module.Module):

      @def_function.function(input_signature=[
          tensor_spec.TensorSpec(shape=[1, 3, 4, 3], dtype=dtypes.float32)
      ])
      def conv(self, input_tensor: core.Tensor) -> Mapping[str, core.Tensor]:
        """Performs a 2D convolution operation.

        Args:
          input_tensor: Input tensor to perform convolution on.

        Returns:
          A map of: output key -> output result.
        """
        filters = np.random.uniform(
            low=-10, high=10, size=(2, 3, 3, 1)).astype('f4')
        bias = np.random.uniform(low=0, high=10, size=(3)).astype('f4')
        scale, offset = [1.0, 1.0, 1.0], [0.5, 0.5, 0.5]
        mean, variance = scale, offset
        out = nn_ops.depthwise_conv2d_native(
            input_tensor,
            filters,
            strides=[1, 2, 2, 1],
            dilations=[1, 1, 1, 1],
            padding='SAME',
            data_format='NHWC')
        if has_bias:
          out = nn_ops.bias_add(out, bias)
        if has_bn:
          # Fusing is supported for non-training case.
          out, _, _, _, _, _ = nn_ops.fused_batch_norm_v3(
              out, scale, offset, mean, variance, is_training=False)
        if activation_fn is not None:
          out = activation_fn(out)
        return {'output': out}

    model = DepthwiseConvModel()
    input_saved_model_path = self.create_tempdir('input').full_path
    saved_model_save.save(model, input_saved_model_path)

    def data_gen() -> repr_dataset.RepresentativeDataset:
      for _ in range(8):
        yield {
            'input_tensor':
                ops.convert_to_tensor(
                    np.random.uniform(low=0, high=150,
                                      size=(1, 3, 4, 3)).astype('f4')),
        }

    tags = [tag_constants.SERVING]
    output_directory = self.create_tempdir().full_path

    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.STATIC_RANGE))

    converted_model = quantize_model.quantize(
        input_saved_model_path, ['serving_default'],
        tags,
        output_directory,
        quantization_options,
        representative_dataset=data_gen())
    self.assertIsNotNone(converted_model)
    self.assertCountEqual(converted_model.signatures._signatures.keys(),
                          {'serving_default'})

    output_loader = saved_model_loader.SavedModelLoader(output_directory)
    output_meta_graphdef = output_loader.get_meta_graph_def_from_tags(tags)
    self.assertTrue(_contains_quantized_function_call(output_meta_graphdef))
    self.assertFalse(_contains_op(output_meta_graphdef, 'FusedBatchNormV3'))

  @parameterized.named_parameters(
      ('none', None, False),
      ('relu', nn_ops.relu, False),
      ('relu6', nn_ops.relu6, False),
      ('with_bias', None, True),
      ('with_bias_and_relu', nn_ops.relu, True),
      ('with_bias_and_relu6', nn_ops.relu6, True),
  )
  @test_util.run_in_graph_and_eager_modes
  def test_matmul_ptq_model(self, activation_fn: Optional[ops.Operation],
                            has_bias: bool):
    model = MatmulModel(has_bias, activation_fn)
    input_saved_model_path = self.create_tempdir('input').full_path
    saved_model_save.save(model, input_saved_model_path)

    def data_gen() -> repr_dataset.RepresentativeDataset:
      for _ in range(8):
        yield {
            'input_tensor':
                ops.convert_to_tensor(
                    np.random.uniform(low=0, high=5, size=(1, 4)).astype('f4')),
        }

    tags = [tag_constants.SERVING]
    output_directory = self.create_tempdir().full_path

    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.STATIC_RANGE))

    converted_model = quantize_model.quantize(
        input_saved_model_path, ['serving_default'],
        tags,
        output_directory,
        quantization_options,
        representative_dataset=data_gen())
    self.assertIsNotNone(converted_model)
    self.assertCountEqual(converted_model.signatures._signatures.keys(),
                          {'serving_default'})

    output_loader = saved_model_loader.SavedModelLoader(output_directory)
    output_meta_graphdef = output_loader.get_meta_graph_def_from_tags(tags)
    self.assertTrue(_contains_quantized_function_call(output_meta_graphdef))

  @parameterized.named_parameters(
      ('use_constant', False),
      ('use_variable', True),
  )
  @test_util.run_v2_only
  def test_gather_model(self, use_variable):

    model = _create_gather_model(use_variable)

    input_saved_model_path = self.create_tempdir('input').full_path
    saved_model_save.save(model, input_saved_model_path)

    tags = [tag_constants.SERVING]
    output_directory = self.create_tempdir().full_path

    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.STATIC_RANGE))

    data_gen = _create_data_generator(
        input_key='input_tensor',
        shape=[6],
        minval=0,
        maxval=10,
        dtype=dtypes.int64)

    converted_model = quantize_model.quantize(input_saved_model_path,
                                              ['serving_default'], tags,
                                              output_directory,
                                              quantization_options, data_gen)

    self.assertIsNotNone(converted_model)
    self.assertCountEqual(converted_model.signatures._signatures.keys(),
                          {'serving_default'})

    output_loader = saved_model_loader.SavedModelLoader(output_directory)
    output_meta_graphdef = output_loader.get_meta_graph_def_from_tags(tags)
    # Currently gather is not supported.
    self.assertFalse(_contains_quantized_function_call(output_meta_graphdef))

  @test_util.run_in_graph_and_eager_modes
  def test_model_ptq_use_representative_samples_list(self):
    model = MatmulModel()
    input_savedmodel_dir = self.create_tempdir('input').full_path
    saved_model_save.save(model, input_savedmodel_dir)

    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.STATIC_RANGE))
    output_savedmodel_dir = self.create_tempdir().full_path
    tags = {tag_constants.SERVING}

    representative_dataset: repr_dataset.RepresentativeDataset = [{
        'input_tensor': random_ops.random_uniform(shape=(1, 4)),
    } for _ in range(8)]

    converted_model = quantize_model.quantize(
        input_savedmodel_dir, ['serving_default'],
        output_directory=output_savedmodel_dir,
        quantization_options=quantization_options,
        representative_dataset=representative_dataset)

    self.assertIsNotNone(converted_model)
    self.assertCountEqual(converted_model.signatures._signatures.keys(),
                          {'serving_default'})
    output_loader = saved_model_loader.SavedModelLoader(output_savedmodel_dir)
    output_meta_graphdef = output_loader.get_meta_graph_def_from_tags(tags)
    self.assertTrue(_contains_quantized_function_call(output_meta_graphdef))

  @test_util.run_in_graph_and_eager_modes
  def test_model_ptq_use_ndarray_representative_dataset(self):
    model = MatmulModel()
    input_savedmodel_dir = self.create_tempdir('input').full_path
    saved_model_save.save(model, input_savedmodel_dir)

    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.STATIC_RANGE))
    output_savedmodel_dir = self.create_tempdir().full_path
    tags = {tag_constants.SERVING}

    # Use np.ndarrays instead of tf.Tensors for the representative dataset.
    representative_dataset = [{
        'input_tensor': np.random.uniform(size=(1, 4)).astype(np.float32),
    } for _ in range(4)]

    converted_model = quantize_model.quantize(
        input_savedmodel_dir, ['serving_default'],
        tags=tags,
        output_directory=output_savedmodel_dir,
        quantization_options=quantization_options,
        representative_dataset=representative_dataset)

    self.assertIsNotNone(converted_model)
    self.assertCountEqual(converted_model.signatures._signatures.keys(),
                          {'serving_default'})
    output_loader = saved_model_loader.SavedModelLoader(output_savedmodel_dir)
    output_meta_graphdef = output_loader.get_meta_graph_def_from_tags(tags)
    self.assertTrue(_contains_quantized_function_call(output_meta_graphdef))

  @test_util.run_in_graph_and_eager_modes
  def test_model_ptq_use_python_list_representative_dataset(self):
    model = MatmulModel()
    input_savedmodel_dir = self.create_tempdir('input').full_path
    saved_model_save.save(model, input_savedmodel_dir)

    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.STATIC_RANGE))
    output_savedmodel_dir = self.create_tempdir().full_path
    tags = {tag_constants.SERVING}

    # Use plain python lists as representative samples.
    representative_dataset = [{
        'input_tensor': [[0.1, 0.2, 0.3, 0.4]],
    } for _ in range(4)]

    converted_model = quantize_model.quantize(
        input_savedmodel_dir, ['serving_default'],
        tags=tags,
        output_directory=output_savedmodel_dir,
        quantization_options=quantization_options,
        representative_dataset=representative_dataset)

    self.assertIsNotNone(converted_model)
    self.assertCountEqual(converted_model.signatures._signatures.keys(),
                          {'serving_default'})
    output_loader = saved_model_loader.SavedModelLoader(output_savedmodel_dir)
    output_meta_graphdef = output_loader.get_meta_graph_def_from_tags(tags)
    self.assertTrue(_contains_quantized_function_call(output_meta_graphdef))

  # tf.data.Dataset is as an Iterable (thus can be used as representative
  # dataset) only in TF2 (eager mode).
  @test_util.run_v2_only
  def test_model_ptq_use_tf_dataset_for_representative_dataset(self):
    model = MatmulModel()
    input_savedmodel_dir = self.create_tempdir('input').full_path
    saved_model_save.save(model, input_savedmodel_dir)

    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.STATIC_RANGE))
    output_savedmodel_dir = self.create_tempdir().full_path
    tags = {tag_constants.SERVING}

    representative_samples = [{
        'input_tensor': random_ops.random_uniform(shape=(1, 4)),
    } for _ in range(8)]

    # Construct a tf.data.Dataset from the representative samples.
    representative_dataset = dataset_ops.DatasetV2.from_generator(
        lambda: representative_samples,
        output_signature={
            'input_tensor':
                tensor_spec.TensorSpec(shape=(1, 4), dtype=dtypes.float32),
        })

    converted_model = quantize_model.quantize(
        input_savedmodel_dir, ['serving_default'],
        output_directory=output_savedmodel_dir,
        quantization_options=quantization_options,
        representative_dataset=representative_dataset)

    self.assertIsNotNone(converted_model)
    self.assertCountEqual(converted_model.signatures._signatures.keys(),
                          {'serving_default'})
    output_loader = saved_model_loader.SavedModelLoader(output_savedmodel_dir)
    output_meta_graphdef = output_loader.get_meta_graph_def_from_tags(tags)
    self.assertTrue(_contains_quantized_function_call(output_meta_graphdef))

  @test_util.run_in_graph_and_eager_modes
  def test_model_ptq_no_representative_sample_shows_warnings(self):
    model = MatmulModel()
    input_savedmodel_dir = self.create_tempdir('input').full_path
    output_savedmodel_dir = self.create_tempdir().full_path
    saved_model_save.save(model, input_savedmodel_dir)

    tags = [tag_constants.SERVING]
    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.STATIC_RANGE))

    with warnings.catch_warnings(record=True) as warnings_list:
      converted_model = quantize_model.quantize(
          input_savedmodel_dir,
          ['serving_default'],
          tags,
          output_savedmodel_dir,
          quantization_options,
          # Put no sample into the representative dataset to make calibration
          # impossible.
          representative_dataset=[])

      self.assertNotEmpty(warnings_list)

      # Warning message should contain the function name.
      self.assertTrue(self._any_warning_contains('matmul', warnings_list))
      self.assertTrue(
          self._any_warning_contains('does not have min or max values',
                                     warnings_list))

    self.assertIsNotNone(converted_model)
    self.assertCountEqual(converted_model.signatures._signatures.keys(),
                          {'serving_default'})
    output_loader = saved_model_loader.SavedModelLoader(output_savedmodel_dir)
    output_meta_graphdef = output_loader.get_meta_graph_def_from_tags(tags)
    # Model is not quantized because there was no sample data for calibration.
    self.assertFalse(_contains_quantized_function_call(output_meta_graphdef))

  @test_util.run_in_graph_and_eager_modes
  def test_model_ptq_with_uncalibrated_subgraph(self):

    class IfModel(module.Module):
      """A model that contains a branching op."""

      @def_function.function(input_signature=[
          tensor_spec.TensorSpec(shape=[1, 4], dtype=dtypes.float32)
      ])
      def model_fn(self, x: core.Tensor) -> Mapping[str, core.Tensor]:
        """Runs the input tensor to a branched operations.

        The graph is branched by a condition whether the sum of elements of `x`
        is greater than 10.

        Args:
          x: Input tensor.

        Returns:
          A map of: output key -> output result.
        """
        if math_ops.reduce_sum(x) > 10.0:
          filters = np.random.uniform(
              low=-1.0, high=1.0, size=(4, 3)).astype('f4')
          bias = np.random.uniform(low=-1.0, high=1.0, size=(3,)).astype('f4')
          out = math_ops.matmul(x, filters)
          out = nn_ops.bias_add(out, bias)
          return {'output': out}

        filters = np.random.uniform(
            low=-1.0, high=1.0, size=(4, 3)).astype('f4')
        bias = np.random.uniform(low=-1.0, high=1.0, size=(3,)).astype('f4')
        out = math_ops.matmul(x, filters)
        out = nn_ops.bias_add(out, bias)
        return {'output': out}

    model = IfModel()
    input_saved_model_path = self.create_tempdir('input').full_path
    saved_model_save.save(model, input_saved_model_path)

    def data_gen() -> repr_dataset.RepresentativeDataset:
      for _ in range(8):
        yield {
            'x':
                ops.convert_to_tensor(
                    np.random.uniform(low=0.0, high=1.0,
                                      size=(1, 4)).astype('f4')),
        }

    tags = [tag_constants.SERVING]
    output_directory = self.create_tempdir().full_path

    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.STATIC_RANGE))

    with warnings.catch_warnings(record=True) as warnings_list:
      converted_model = quantize_model.quantize(
          input_saved_model_path, ['serving_default'],
          tags,
          output_directory,
          quantization_options,
          representative_dataset=data_gen())

      self.assertNotEmpty(warnings_list)

      # Warning message should contain the function name. The uncalibrated path
      # is when the condition is true, so 'cond_true' function must be part of
      # the warning message.
      self.assertTrue(self._any_warning_contains('cond_true', warnings_list))
      self.assertFalse(self._any_warning_contains('cond_false', warnings_list))
      self.assertTrue(
          self._any_warning_contains('does not have min or max values',
                                     warnings_list))

    self.assertIsNotNone(converted_model)
    self.assertCountEqual(converted_model.signatures._signatures.keys(),
                          {'serving_default'})
    output_loader = saved_model_loader.SavedModelLoader(output_directory)
    output_meta_graphdef = output_loader.get_meta_graph_def_from_tags(tags)
    self.assertTrue(_contains_quantized_function_call(output_meta_graphdef))

  # Run this test only with the eager mode.
  @test_util.run_v2_only
  def test_ptq_model_with_multiple_signatures(self):
    # Create and save a model having 2 signatures.
    model = MultipleSignatureModel()

    signatures = {
        'sig1':
            model.matmul.get_concrete_function(
                tensor_spec.TensorSpec(shape=(1, 4), dtype=dtypes.float32)),
        'sig2':
            model.conv.get_concrete_function(
                tensor_spec.TensorSpec(
                    shape=(1, 3, 4, 3), dtype=dtypes.float32)),
    }
    input_saved_model_path = self.create_tempdir('input').full_path
    saved_model_save.save(model, input_saved_model_path, signatures=signatures)

    output_directory = self.create_tempdir().full_path
    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.STATIC_RANGE))

    def data_gen_sig1() -> repr_dataset.RepresentativeDataset:
      """Generates tuple-style samples for signature 'sig1'.

      The first element of the tuple identifies the signature key the input data
      is for.

      Yields:
        Representative sample for 'sig1'.
      """
      for _ in range(4):
        yield {'matmul_input': random_ops.random_uniform(shape=(1, 4))}

    def data_gen_sig2() -> repr_dataset.RepresentativeDataset:
      """Generates tuple-style samples for signature 'sig2'.

      The first element of the tuple identifies the signature key the input data
      is for.

      Yields:
        Representative sample for 'sig2'.
      """
      for _ in range(4):
        yield {'conv_input': random_ops.random_uniform(shape=(1, 3, 4, 3))}

    tags = {tag_constants.SERVING}
    converted_model = quantize_model.quantize(
        input_saved_model_path,
        signature_keys=['sig1', 'sig2'],
        tags=tags,
        output_directory=output_directory,
        quantization_options=quantization_options,
        representative_dataset={
            'sig1': data_gen_sig1(),
            'sig2': data_gen_sig2(),
        })
    self.assertIsNotNone(converted_model)
    self.assertCountEqual(converted_model.signatures._signatures.keys(),
                          {'sig1', 'sig2'})

    output_loader = saved_model_loader.SavedModelLoader(output_directory)
    output_meta_graphdef = output_loader.get_meta_graph_def_from_tags(tags)
    self.assertTrue(_contains_quantized_function_call(output_meta_graphdef))

  # Run this test only with the eager mode.
  @test_util.run_v2_only
  def test_ptq_multiple_signatures_invalid_dataset_raises_value_error(self):
    # Create and save a model having 2 signatures.
    model = MultipleSignatureModel()

    signatures = {
        'sig1':
            model.matmul.get_concrete_function(
                tensor_spec.TensorSpec(shape=(1, 4), dtype=dtypes.float32)),
        'sig2':
            model.conv.get_concrete_function(
                tensor_spec.TensorSpec(
                    shape=(1, 3, 4, 3), dtype=dtypes.float32)),
    }
    input_saved_model_path = self.create_tempdir('input').full_path
    saved_model_save.save(model, input_saved_model_path, signatures=signatures)

    output_directory = self.create_tempdir().full_path
    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.STATIC_RANGE))

    # Use a dict-style samples instead of tuple-style samples. This is invalid
    # because for a model multiple signatures one must use tuple-style samples.
    invalid_dataset: repr_dataset.RepresentativeDataset = [{
        'matmul_input': random_ops.random_uniform(shape=(1, 4))
    } for _ in range(8)]

    with self.assertRaisesRegex(ValueError, 'Invalid representative dataset.'):
      quantize_model.quantize(
          input_saved_model_path,
          signature_keys=['sig1', 'sig2'],
          tags={tag_constants.SERVING},
          output_directory=output_directory,
          quantization_options=quantization_options,
          representative_dataset=invalid_dataset)

  @test_util.run_in_graph_and_eager_modes
  def test_ptq_model_with_tf1_saved_model_with_variable_for_conv2d(self):
    input_saved_model_path = self.create_tempdir('input').full_path
    signature_key = signature_constants.DEFAULT_SERVING_SIGNATURE_DEF_KEY
    tags = {tag_constants.SERVING}

    input_placeholder = _create_and_save_tf1_conv_model(
        input_saved_model_path,
        signature_key,
        tags,
        input_key='x',
        output_key='output',
        use_variable=True)

    signature_keys = [signature_key]
    output_directory = self.create_tempdir().full_path

    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.STATIC_RANGE))

    data_gen = _create_data_generator(
        input_key='x', shape=input_placeholder.shape)

    converted_model = quantize_model.quantize(
        input_saved_model_path,
        signature_keys,
        tags,
        output_directory,
        quantization_options,
        representative_dataset=data_gen)

    self.assertIsNotNone(converted_model)
    self.assertCountEqual(converted_model.signatures._signatures.keys(),
                          signature_keys)

    output_loader = saved_model_loader.SavedModelLoader(output_directory)
    output_meta_graphdef = output_loader.get_meta_graph_def_from_tags(tags)
    self.assertTrue(_contains_quantized_function_call(output_meta_graphdef))

  @parameterized.named_parameters(
      ('use_constant', False),
      ('use_variable', True),
  )
  @test_util.run_in_graph_and_eager_modes
  def test_ptq_model_with_tf1_saved_model_with_variable_for_gather(
      self, use_variable):
    input_saved_model_path = self.create_tempdir('input').full_path
    signature_key = signature_constants.DEFAULT_SERVING_SIGNATURE_DEF_KEY
    tags = {tag_constants.SERVING}

    input_placeholder = _create_and_save_tf1_gather_model(
        input_saved_model_path,
        signature_key,
        tags,
        input_key='x',
        output_key='output',
        use_variable=use_variable)

    signature_keys = [signature_key]
    output_directory = self.create_tempdir().full_path

    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.STATIC_RANGE))

    data_gen = _create_data_generator(
        input_key='x',
        shape=input_placeholder.shape,
        minval=0,
        maxval=10,
        dtype=dtypes.int64)

    converted_model = quantize_model.quantize(
        input_saved_model_path,
        signature_keys,
        tags,
        output_directory,
        quantization_options,
        representative_dataset=data_gen)

    self.assertIsNotNone(converted_model)
    self.assertCountEqual(converted_model.signatures._signatures.keys(),
                          signature_keys)

    output_loader = saved_model_loader.SavedModelLoader(output_directory)
    output_meta_graphdef = output_loader.get_meta_graph_def_from_tags(tags)
    # Quantization is not currently supported for gather.
    self.assertFalse(_contains_quantized_function_call(output_meta_graphdef))

  @test_util.run_in_graph_and_eager_modes
  def test_ptq_model_with_tf1_saved_model(self):
    input_saved_model_path = self.create_tempdir('input').full_path
    tags = {tag_constants.SERVING}
    signature_key = signature_constants.DEFAULT_SERVING_SIGNATURE_DEF_KEY

    input_placeholder = _create_and_save_tf1_conv_model(
        input_saved_model_path,
        signature_key,
        tags,
        input_key='p',
        output_key='output',
        use_variable=False)

    signature_keys = [signature_key]
    output_directory = self.create_tempdir().full_path

    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.STATIC_RANGE))

    data_gen = _create_data_generator(
        input_key='p', shape=input_placeholder.shape)

    converted_model = quantize_model.quantize(
        input_saved_model_path,
        signature_keys,
        tags,
        output_directory,
        quantization_options,
        representative_dataset=data_gen)

    self.assertIsNotNone(converted_model)
    self.assertCountEqual(converted_model.signatures._signatures.keys(),
                          signature_keys)

    output_loader = saved_model_loader.SavedModelLoader(output_directory)
    output_meta_graphdef = output_loader.get_meta_graph_def_from_tags(tags)
    self.assertTrue(_contains_quantized_function_call(output_meta_graphdef))

  @test_util.run_in_graph_and_eager_modes
  def test_ptq_model_with_tf1_saved_model_multiple_signatures(self):
    input_saved_model_path = self.create_tempdir('input').full_path
    tags = {tag_constants.SERVING}

    # Create two models and add them to a same SavedModel under different
    # signature keys.
    with ops.Graph().as_default(), session.Session() as sess:
      in_placeholder_1, output_tensor_1 = _create_simple_tf1_conv_model()
      sig_def_1 = signature_def_utils_impl.predict_signature_def(
          inputs={'x1': in_placeholder_1}, outputs={'output1': output_tensor_1})

      in_placeholder_2, output_tensor_2 = _create_simple_tf1_conv_model()
      sig_def_2 = signature_def_utils_impl.predict_signature_def(
          inputs={'x2': in_placeholder_2}, outputs={'output2': output_tensor_2})

      v1_builder = builder.SavedModelBuilder(input_saved_model_path)
      v1_builder.add_meta_graph_and_variables(
          sess, tags, signature_def_map={
              'sig1': sig_def_1,
              'sig2': sig_def_2,
          })

      v1_builder.save()

    output_directory = self.create_tempdir().full_path
    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.STATIC_RANGE))

    def data_gen_sig1() -> repr_dataset.RepresentativeDataset:
      """Generates tuple-style samples.

      The first element of the tuple identifies the signature key the input data
      is for.

      Yields:
        Representative samples for signature 'sig1'.
      """
      for _ in range(4):
        yield {'x1': random_ops.random_uniform(shape=in_placeholder_1.shape)}

    def data_gen_sig2() -> repr_dataset.RepresentativeDataset:
      """Generates tuple-style samples.

      The first element of the tuple identifies the signature key the input data
      is for.

      Yields:
        Representative samples for signature 'sig2'.
      """
      for _ in range(4):
        yield {'x2': random_ops.random_uniform(shape=in_placeholder_2.shape)}

    converted_model = quantize_model.quantize(
        input_saved_model_path,
        signature_keys=['sig1', 'sig2'],
        tags=tags,
        output_directory=output_directory,
        quantization_options=quantization_options,
        representative_dataset={
            'sig1': data_gen_sig1(),
            'sig2': data_gen_sig2(),
        })

    self.assertIsNotNone(converted_model)
    self.assertCountEqual(converted_model.signatures._signatures.keys(),
                          {'sig1', 'sig2'})

    output_loader = saved_model_loader.SavedModelLoader(output_directory)
    output_meta_graphdef = output_loader.get_meta_graph_def_from_tags(tags)
    self.assertTrue(_contains_quantized_function_call(output_meta_graphdef))

  @test_util.run_in_graph_and_eager_modes
  def test_ptq_model_with_tf1_saved_model_invalid_input_key_raises_value_error(
      self):
    input_saved_model_path = self.create_tempdir('input').full_path
    signature_key = signature_constants.DEFAULT_SERVING_SIGNATURE_DEF_KEY
    tags = {tag_constants.SERVING}

    input_placeholder = _create_and_save_tf1_conv_model(
        input_saved_model_path,
        signature_key,
        tags,
        input_key='x',
        output_key='output',
        use_variable=False)

    signature_keys = [signature_key]
    output_directory = self.create_tempdir().full_path

    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.STATIC_RANGE))

    # Representative generator function that yields with an invalid input key.
    invalid_data_gen = _create_data_generator(
        input_key='invalid_input_key', shape=input_placeholder.shape)

    with self.assertRaisesRegex(
        ValueError,
        'Failed to run graph for post-training quantization calibration'):
      quantize_model.quantize(
          input_saved_model_path,
          signature_keys,
          tags,
          output_directory,
          quantization_options,
          representative_dataset=invalid_data_gen)

  @test_util.run_in_graph_and_eager_modes
  def test_ptq_model_with_non_default_tags(self):
    input_saved_model_path = self.create_tempdir('input').full_path
    signature_key = signature_constants.DEFAULT_SERVING_SIGNATURE_DEF_KEY
    # Use a different set of tags other than {"serve"}.
    tags = {tag_constants.TRAINING, tag_constants.GPU}

    # Non-default tags are usually used when saving multiple metagraphs in TF1.
    input_placeholder = _create_and_save_tf1_conv_model(
        input_saved_model_path,
        signature_key,
        tags,
        input_key='input',
        output_key='output',
        use_variable=True)

    signature_keys = [signature_key]
    output_directory = self.create_tempdir().full_path

    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.STATIC_RANGE))

    data_gen = _create_data_generator(
        input_key='input', shape=input_placeholder.shape)

    converted_model = quantize_model.quantize(
        input_saved_model_path,
        signature_keys,
        tags,
        output_directory,
        quantization_options,
        representative_dataset=data_gen)

    self.assertIsNotNone(converted_model)
    self.assertCountEqual(converted_model.signatures._signatures.keys(),
                          signature_keys)

    output_loader = saved_model_loader.SavedModelLoader(output_directory)
    output_meta_graphdef = output_loader.get_meta_graph_def_from_tags(tags)
    self.assertTrue(_contains_quantized_function_call(output_meta_graphdef))

  @test_util.run_in_graph_and_eager_modes
  def test_ptq_model_with_wrong_tags_raises_error(self):
    input_saved_model_path = self.create_tempdir('input').full_path
    signature_key = signature_constants.DEFAULT_SERVING_SIGNATURE_DEF_KEY
    save_tags = {tag_constants.TRAINING, tag_constants.GPU}

    input_placeholder = _create_and_save_tf1_conv_model(
        input_saved_model_path,
        signature_key,
        save_tags,
        input_key='input',
        output_key='output',
        use_variable=True)

    signature_keys = [signature_key]
    output_directory = self.create_tempdir().full_path

    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.STATIC_RANGE))

    # Try to use a different set of tags to quantize.
    tags = {tag_constants.SERVING}
    data_gen = _create_data_generator(
        input_key='input', shape=input_placeholder.shape)
    with self.assertRaisesRegex(RuntimeError,
                                'Failed to retrieve MetaGraphDef'):
      quantize_model.quantize(
          input_saved_model_path,
          signature_keys,
          tags,
          output_directory,
          quantization_options,
          representative_dataset=data_gen)


class DynamicRangeQuantizationTest(test.TestCase, parameterized.TestCase):
  """Test cases for dynamic range quantization.

  Run all tests cases in both the graph mode (default in TF1) and the eager mode
  (default in TF2) to ensure support for when TF2 is disabled.
  """

  @test_util.run_in_graph_and_eager_modes
  def test_matmul_model(self):

    class SimpleMatmulModel(module.Module):

      @def_function.function(input_signature=[
          tensor_spec.TensorSpec(shape=[1, 4], dtype=dtypes.float32)
      ])
      def matmul(self, input_tensor: core.Tensor) -> Mapping[str, core.Tensor]:
        """Performs a matrix multiplication.

        Args:
          input_tensor: Input tensor to matmul with the filter.

        Returns:
          A map of: output key -> output result.
        """
        filters = np.random.uniform(
            low=-1.0, high=1.0, size=(4, 3)).astype('f4')
        out = math_ops.matmul(input_tensor, filters)
        return {'output': out}

    model = SimpleMatmulModel()
    input_saved_model_path = self.create_tempdir('input').full_path
    saved_model_save.save(model, input_saved_model_path)

    tags = [tag_constants.SERVING]
    output_directory = self.create_tempdir().full_path

    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.DYNAMIC_RANGE))

    converted_model = quantize_model.quantize(input_saved_model_path,
                                              ['serving_default'], tags,
                                              output_directory,
                                              quantization_options)
    self.assertIsNotNone(converted_model)
    self.assertCountEqual(converted_model.signatures._signatures.keys(),
                          {'serving_default'})

    output_loader = saved_model_loader.SavedModelLoader(output_directory)
    output_meta_graphdef = output_loader.get_meta_graph_def_from_tags(tags)
    self.assertTrue(_contains_quantized_function_call(output_meta_graphdef))

  @test_util.run_in_graph_and_eager_modes
  def test_conv_model(self):

    class ConvModel(module.Module):

      @def_function.function(input_signature=[
          tensor_spec.TensorSpec(shape=[1, 3, 4, 3], dtype=dtypes.float32)
      ])
      def conv(self, input_tensor: core.Tensor) -> Mapping[str, core.Tensor]:
        """Performs a 2D convolution operation.

        Args:
          input_tensor: Input tensor to perform convolution on.

        Returns:
          A map of: output key -> output result.
        """
        filters = np.random.uniform(
            low=-10, high=10, size=(2, 3, 3, 2)).astype('f4')
        bias = np.random.uniform(low=0, high=10, size=(2)).astype('f4')
        out = nn_ops.conv2d(
            input_tensor,
            filters,
            strides=[1, 1, 2, 1],
            dilations=[1, 1, 1, 1],
            padding='SAME',
            data_format='NHWC')
        out = nn_ops.bias_add(out, bias, data_format='NHWC')
        out = nn_ops.relu6(out)
        return {'output': out}

    model = ConvModel()
    input_saved_model_path = self.create_tempdir('input').full_path
    saved_model_save.save(model, input_saved_model_path)

    tags = [tag_constants.SERVING]
    output_directory = self.create_tempdir().full_path

    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.DYNAMIC_RANGE))

    converted_model = quantize_model.quantize(input_saved_model_path,
                                              ['serving_default'], tags,
                                              output_directory,
                                              quantization_options)

    self.assertIsNotNone(converted_model)
    self.assertCountEqual(converted_model.signatures._signatures.keys(),
                          {'serving_default'})

    output_loader = saved_model_loader.SavedModelLoader(output_directory)
    output_meta_graphdef = output_loader.get_meta_graph_def_from_tags(tags)
    # Currently conv is not supported.
    self.assertFalse(_contains_quantized_function_call(output_meta_graphdef))

  @parameterized.named_parameters(
      ('use_constant', False),
      ('use_variable', True),
  )
  @test_util.run_v2_only
  def test_gather_model(self, use_variable):

    model = _create_gather_model(use_variable)
    input_saved_model_path = self.create_tempdir('input').full_path
    saved_model_save.save(model, input_saved_model_path)

    tags = [tag_constants.SERVING]
    output_directory = self.create_tempdir().full_path

    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.DYNAMIC_RANGE))

    converted_model = quantize_model.quantize(input_saved_model_path,
                                              ['serving_default'], tags,
                                              output_directory,
                                              quantization_options)

    self.assertIsNotNone(converted_model)
    self.assertCountEqual(converted_model.signatures._signatures.keys(),
                          {'serving_default'})

    output_loader = saved_model_loader.SavedModelLoader(output_directory)
    output_meta_graphdef = output_loader.get_meta_graph_def_from_tags(tags)
    # Currently gather is not supported.
    self.assertFalse(_contains_quantized_function_call(output_meta_graphdef))

  @test_util.run_in_graph_and_eager_modes
  def test_conv_model_with_wrong_tags_raises_error(self):
    input_saved_model_path = self.create_tempdir('input').full_path
    signature_key = signature_constants.DEFAULT_SERVING_SIGNATURE_DEF_KEY
    save_tags = {tag_constants.TRAINING, tag_constants.GPU}

    input_placeholder = _create_and_save_tf1_conv_model(
        input_saved_model_path,
        signature_key,
        save_tags,
        input_key='input',
        output_key='output',
        use_variable=True)

    signature_keys = [signature_key]
    output_directory = self.create_tempdir().full_path

    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.DYNAMIC_RANGE))

    # Try to use a different set of tags to quantize.
    tags = {tag_constants.SERVING}
    data_gen = _create_data_generator(
        input_key='input', shape=input_placeholder.shape)
    with self.assertRaisesRegex(RuntimeError,
                                'Failed to retrieve MetaGraphDef'):
      quantize_model.quantize(
          input_saved_model_path,
          signature_keys,
          tags,
          output_directory,
          quantization_options,
          representative_dataset=data_gen)

  @parameterized.named_parameters(
      ('use_constant', False),
      ('use_variable', True),
  )
  @test_util.run_in_graph_and_eager_modes
  def test_gather_model_tf1(self, use_variable):
    input_saved_model_path = self.create_tempdir('input').full_path
    signature_key = signature_constants.DEFAULT_SERVING_SIGNATURE_DEF_KEY
    tags = {tag_constants.SERVING}

    _ = _create_and_save_tf1_gather_model(
        input_saved_model_path,
        signature_key,
        tags,
        input_key='x',
        output_key='output',
        use_variable=use_variable)

    signature_keys = [signature_key]
    output_directory = self.create_tempdir().full_path

    quantization_options = quant_opts_pb2.QuantizationOptions(
        quantization_method=quant_opts_pb2.QuantizationMethod(
            experimental_method=_ExperimentalMethod.DYNAMIC_RANGE))

    converted_model = quantize_model.quantize(input_saved_model_path,
                                              signature_keys, tags,
                                              output_directory,
                                              quantization_options)

    self.assertIsNotNone(converted_model)
    self.assertCountEqual(converted_model.signatures._signatures.keys(),
                          signature_keys)

    output_loader = saved_model_loader.SavedModelLoader(output_directory)
    output_meta_graphdef = output_loader.get_meta_graph_def_from_tags(tags)
    # Quantization is not currently supported for gather.
    self.assertFalse(_contains_quantized_function_call(output_meta_graphdef))


if __name__ == '__main__':
  test.main()
