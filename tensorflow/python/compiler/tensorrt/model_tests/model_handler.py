# Copyright 2020 The TensorFlow Authors. All Rights Reserved.
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
"""Loads, converts, and runs sample models."""

import abc
import collections
import functools
import tempfile
import time
from typing import List, Mapping, Optional, Sequence, Union

from absl import logging
import numpy as np

from tensorflow.core.framework import graph_pb2
from tensorflow.core.framework import tensor_shape_pb2
from tensorflow.core.protobuf import config_pb2
from tensorflow.core.protobuf import meta_graph_pb2
from tensorflow.python.client import session
from tensorflow.python.compiler.tensorrt import trt_convert as trt
from tensorflow.python.framework import convert_to_constants
from tensorflow.python.framework import dtypes as tf_dtypes
from tensorflow.python.framework import importer
from tensorflow.python.framework import ops as framework_ops
from tensorflow.python.ops import random_ops
from tensorflow.python.saved_model import load as saved_model_load
from tensorflow.python.saved_model import loader as saved_model_loader
from tensorflow.python.saved_model import signature_constants
from tensorflow.python.saved_model import tag_constants


# pylint: disable=bad-whitespace
### Helper Functions
def _get_concrete_tensor_shape(
    tensor_shape: tensor_shape_pb2.TensorShapeProto,
    batch_size: Optional[int] = None) -> Sequence[int]:
  """Gets a concrete tensor shape without dynamic dimensions."""
  if tensor_shape.unknown_rank:
    raise ValueError("Cannot generates random tensors for unknown rank!")
  shape = [dim.size for dim in tensor_shape.dim]
  if not shape:
    raise ValueError("The tensor cannot have a rank of 0!")
  if shape[0] < 0:
    if batch_size is None or batch_size <= 0:
      raise ValueError("Must provide a valid batch size "
                       "as the tensor has a dynamic batch size!")
    shape[0] = batch_size
  if any(filter(lambda x: x < 0, shape)):
    raise ValueError("Cannot have dynamic dimensions except for batch size!")
  return shape


def _generate_random_tensor_v1(tensor_info: meta_graph_pb2.TensorInfo,
                               batch_size: Optional[int] = None) -> np.ndarray:
  """Generates a random tensor based on the data type and tensor shape."""
  dtype = tf_dtypes.as_dtype(tensor_info.dtype)
  shape = _get_concrete_tensor_shape(tensor_info.tensor_shape, batch_size)
  with session.Session():
    return random_ops.random_uniform(
        shape=shape, dtype=dtype, name=tensor_info.name.split(":")[0]).eval()


def _generate_random_tensor_v2(
    tensor: framework_ops.Tensor,
    batch_size: Optional[int] = None) -> framework_ops.Tensor:
  """Generates a random tensor based on the data type and tensor shape."""
  shape = _get_concrete_tensor_shape(tensor.shape.as_proto(), batch_size)
  return random_ops.random_uniform(
      shape=shape, dtype=tensor.dtype, name=tensor.name)


# Models are repeatedly loaded for different TensorRT conversion settings.
# Using cache can reduce I/O.
@functools.lru_cache()
def load_meta_graph(
    saved_model_dir: str, saved_model_tags: str,
    saved_model_signature_key: str) -> meta_graph_pb2.MetaGraphDef:
  """Loads a `tf.MetaGraphDef` in TF1."""
  with session.Session() as sess:
    meta_graph = saved_model_loader.load(
        sess=sess,
        export_dir=saved_model_dir,
        tags=saved_model_tags,
    )
    output_node_names = [
        tensor.name.split(":")[0] for tensor in
        meta_graph.signature_def[saved_model_signature_key].outputs.values()
    ]
    graph_def = (
        convert_to_constants.convert_variables_to_constants_from_session_graph(
            sess, meta_graph.graph_def, output_node_names))
    meta_graph.graph_def.CopyFrom(graph_def)
  return meta_graph


@functools.lru_cache()
def load_graph_func(saved_model_dir: str, saved_model_tags: str,
                    saved_model_signature_key: str):
  """Loads a graph function in TF2."""
  imported = saved_model_load.load(
      export_dir=saved_model_dir, tags=saved_model_tags)
  graph_func = imported.signatures[saved_model_signature_key]
  return convert_to_constants.convert_variables_to_constants_v2(graph_func)


### Test Classes
class TestResult(
    collections.namedtuple("TestResult",
                           ["outputs", "latency", "trt_convert_params"])):

  def __new__(cls,
              outputs: Mapping[str, np.ndarray],
              latency: List[float],
              trt_convert_params: trt.TrtConversionParams = None):
    return super(TestResult, cls).__new__(cls, outputs, latency,
                                          trt_convert_params)


class ModelConfig(
    collections.namedtuple("ModelConfig", [
        "saved_model_dir", "saved_model_tags", "saved_model_signature_key",
        "default_batch_size"
    ])):
  """Configurations for test models."""

  def __new__(cls,
              saved_model_dir: str,
              saved_model_tags: Sequence[str] = (tag_constants.SERVING,),
              saved_model_signature_key: str = (
                  signature_constants.DEFAULT_SERVING_SIGNATURE_DEF_KEY),
              default_batch_size: int = 1):
    return super(ModelConfig,
                 cls).__new__(cls, saved_model_dir, saved_model_tags,
                              saved_model_signature_key, default_batch_size)


class _ModelHandlerBase(metaclass=abc.ABCMeta):
  """Base class for running a model."""

  def __init__(self, model_config: ModelConfig):
    self._model_config = model_config

  def __str__(self) -> str:
    return str(self._model_config)

  def __repr__(self) -> str:
    return "{}({})".format(self.__class__.__name__, str(self))

  @property
  def model_config(self) -> ModelConfig:
    return self._model_config

  @property
  def input_tensort_names(self) -> Sequence[str]:
    """Names of input tensors."""

  @property
  def output_tensor_names(self) -> Sequence[str]:
    """Names of output tensors."""

  @abc.abstractmethod
  def generate_random_inputs(
      self,
      batch_size: Optional[int] = None
  ) -> Mapping[str, Union[np.ndarray, framework_ops.Tensor]]:
    """Generates mapping from names to input tensors."""

  @abc.abstractmethod
  def run(self,
          inputs=None,
          warmup_iterations: int = 10,
          benchmark_iterations: int = 100,
          allow_to_use_gpu: bool = False) -> TestResult:
    """Runs the model with provided or randomly generated input tensors.

    Args:
      inputs: Mapping from names to input ndarrays in TF1, or a sequence of
        tensors in TF2. If `None`, ramdomly generated inputs will be used
        instead.
      warmup_iterations: Number of inferences to warm up the runtime.
      benchmark_iterations: Number of inferences to measure the latency.
      allow_to_use_gpu: Whether it is allowed to use GPU or not.

    Returns:
      `TestResult` summarizing timing and numerics information.
    """


class ModelHandlerV1(_ModelHandlerBase):
  """Runs a model in TF1."""

  @property
  def meta_graph(self) -> meta_graph_pb2.MetaGraphDef:
    return load_meta_graph(
        saved_model_dir=self.model_config.saved_model_dir,
        saved_model_tags=self.model_config.saved_model_tags,
        saved_model_signature_key=self.model_config.saved_model_signature_key)

  @property
  def input_tensor_info(self) -> Mapping[str, meta_graph_pb2.TensorInfo]:
    return self.meta_graph.signature_def[
        self.model_config.saved_model_signature_key].inputs

  @property
  def output_tensor_info(self) -> Mapping[str, meta_graph_pb2.TensorInfo]:
    return self.meta_graph.signature_def[
        self.model_config.saved_model_signature_key].outputs

  @property
  def input_tensort_names(self) -> Sequence[str]:
    return [info.name for info in self.input_tensor_info.values()]

  @property
  def output_tensor_names(self) -> Sequence[str]:
    return [info.name for info in self.output_tensor_info.values()]

  def generate_random_inputs(self,
                             batch_size: Optional[int] = None
                            ) -> Mapping[str, np.ndarray]:
    batch_size = batch_size or self.model_config.default_batch_size
    return {
        tensor_info.name: _generate_random_tensor_v1(tensor_info, batch_size)
        for tensor_info in self.input_tensor_info.values()
    }

  def run(self,
          inputs: Optional[Mapping[str, np.ndarray]] = None,
          warmup_iterations=10,
          benchmark_iterations=100,
          allow_to_use_gpu=False) -> TestResult:
    inputs = inputs or self.generate_random_inputs()
    config_proto = None
    if not allow_to_use_gpu:
      config_proto = config_pb2.ConfigProto(device_count={"CPU": 1, "GPU": 0})
    with session.Session(config=config_proto) as sess:
      importer.import_graph_def(self.meta_graph.graph_def)
      try:
        for _ in range(warmup_iterations):
          sess.run(fetches=self.output_tensor_names, feed_dict=inputs)
        latency = []
        for _ in range(benchmark_iterations):
          before = time.time()
          outputs = sess.run(fetches=self.output_tensor_names, feed_dict=inputs)
          latency.append(time.time() - before)
      except Exception as exc:
        raise RuntimeError("Failed to run model inference! "
                           "Model information: {}".format(str(self))) from exc
      outputs = dict(zip(self.output_tensor_names, outputs))
    return TestResult(latency=latency, outputs=outputs if inputs else None)


class ModelHandlerV2(_ModelHandlerBase):
  """Runs a model in TF2."""

  @property
  def graph_func(self):
    graph_func = load_graph_func(
        saved_model_dir=self.model_config.saved_model_dir,
        saved_model_tags=self.model_config.saved_model_tags,
        saved_model_signature_key=self.model_config.saved_model_signature_key)
    return convert_to_constants.convert_variables_to_constants_v2(graph_func)

  @property
  def input_tensor_names(self):
    return [tensor.name for tensor in self.graph_func.inputs]

  @property
  def output_tensor_names(self):
    return [tensor.name for tensor in self.graph_func.outputs]

  def generate_random_inputs(self,
                             batch_size: Optional[int] = None
                            ) -> Sequence[framework_ops.Tensor]:
    batch_size = batch_size or self.model_config.default_batch_size
    return [
        _generate_random_tensor_v2(tensor, batch_size)
        for tensor in self.graph_func.inputs
    ]

  def run(self,
          inputs: Optional[Sequence[framework_ops.Tensor]] = None,
          warmup_iterations=10,
          benchmark_iterations=100,
          allow_to_use_gpu=False) -> TestResult:
    inputs = inputs or self.generate_random_inputs()
    try:
      device = "/device:gpu:0" if allow_to_use_gpu else "/device:cpu:0"
      with framework_ops.device(device):
        for _ in range(warmup_iterations):
          self.graph_func(*inputs)
        latency = []
        for _ in range(benchmark_iterations):
          before = time.time()
          outputs = self.graph_func(*inputs)
          latency.append(time.time() - before)
    except Exception as exc:
      raise RuntimeError("Failed to run model inference! "
                         "Model information: {}".format(str(self))) from exc
    outputs = dict(zip(self.output_tensor_names, outputs))
    return TestResult(latency=latency, outputs=outputs if inputs else None)


class _TrtModelHandlerBase(_ModelHandlerBase):
  """Base class for converting and running a model."""

  def __init__(
      self,
      model_config: ModelConfig,
      trt_convert_params: trt.TrtConversionParams,
  ):
    super(_TrtModelHandlerBase, self).__init__(model_config)
    self._trt_convert_params = trt_convert_params

    self._converter = self._create_converter(trt_convert_params)
    logging.info("Converting to TensorRT!")
    self._check_conversion(self._converter.convert())
    self._conversion_is_saved = False

  @abc.abstractmethod
  def _create_converter(self, trt_convert_params: trt.TrtConversionParams):
    """Creates a converter for the corresponding TF version."""

  @abc.abstractmethod
  def _check_conversion(self, conversion_output):
    """Checks if conversion output has any TensorRT engines."""

  def _check_contains_trt_engine(self, graph_def: graph_pb2.GraphDef):
    if "TRTEngineOp" not in [node.op for node in graph_def.node]:
      raise RuntimeError("Failed to convert to TensorRT! "
                         "Model Information: {}".format(str(self)))

  def __str__(self) -> str:
    base = super(_TrtModelHandlerBase, self).__str__()
    return "{}, TrtConversionParams: {}".format(base,
                                                str(self._trt_convert_params))

  @property
  def trt_convert_params(self) -> trt.TrtConversionParams:
    return self._trt_convert_params

  def save(self,
           output_saved_model_dir: Optional[str] = None,
           overwrite=True) -> None:
    """Saves a TensorRT converted model."""
    if self._conversion_is_saved and not overwrite:
      return
    output_saved_model_dir = output_saved_model_dir or tempfile.mkdtemp()
    logging.info("Saving TensorRT model to %s!", output_saved_model_dir)
    self._converter.save(output_saved_model_dir)
    self._model_config = self.model_config._replace(
        saved_model_dir=output_saved_model_dir)
    self._conversion_is_saved = True


class TrtModelHandlerV1(_TrtModelHandlerBase, ModelHandlerV1):
  """Converts a TF1 model with TensorRT and runs the converted model."""

  def _create_converter(self, trt_convert_params: trt.TrtConversionParams):
    conversion_nodes_denylist = self.output_tensor_names
    return trt.TrtGraphConverter(
        input_saved_model_dir=self.model_config.saved_model_dir,
        input_saved_model_tags=self.model_config.saved_model_tags,
        input_saved_model_signature_key=(
            self.model_config.saved_model_signature_key),
        nodes_denylist=conversion_nodes_denylist,
        max_batch_size=trt_convert_params.max_batch_size,
        max_workspace_size_bytes=trt_convert_params.max_workspace_size_bytes,
        precision_mode=trt_convert_params.precision_mode,
        minimum_segment_size=trt_convert_params.minimum_segment_size,
        is_dynamic_op=trt_convert_params.is_dynamic_op,
        maximum_cached_engines=trt_convert_params.maximum_cached_engines,
        use_calibration=trt_convert_params.use_calibration,
    )

  _check_conversion = _TrtModelHandlerBase._check_contains_trt_engine

  def run(self,
          inputs: Optional[Mapping[str, np.ndarray]] = None,
          warmup_iterations=10,
          benchmark_iterations=100) -> TestResult:
    self.save(overwrite=False)
    logging.info("Running with TensorRT!")
    test_result = ModelHandlerV1.run(
        self,
        inputs,
        warmup_iterations,
        benchmark_iterations,
        allow_to_use_gpu=True)
    return test_result._replace(trt_convert_params=self._trt_convert_params)


class TrtModelHandlerV2(_TrtModelHandlerBase, ModelHandlerV2):
  """Converts a TF2 model with TensorRT and runs the converted model."""

  def _create_converter(self, trt_convert_params: trt.TrtConversionParams):
    return trt.TrtGraphConverterV2(
        input_saved_model_dir=self.model_config.saved_model_dir,
        input_saved_model_tags=self.model_config.saved_model_tags,
        input_saved_model_signature_key=(
            self.model_config.saved_model_signature_key),
        conversion_params=trt_convert_params)

  def _check_conversion(self, graph_func):
    graph_def = graph_func.graph.as_graph_def()
    self._check_contains_trt_engine(graph_def)

  def run(self,
          inputs: Optional[Sequence[framework_ops.Tensor]] = None,
          warmup_iterations=10,
          benchmark_iterations=100) -> TestResult:
    self.save(overwrite=False)
    logging.info("Running with TensorRT!")
    test_result = ModelHandlerV2.run(
        self,
        inputs,
        warmup_iterations,
        benchmark_iterations,
        allow_to_use_gpu=True)
    return test_result._replace(trt_convert_params=self._trt_convert_params)
