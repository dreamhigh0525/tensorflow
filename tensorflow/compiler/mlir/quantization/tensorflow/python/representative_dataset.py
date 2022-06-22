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
"""Defines types required for representative datasets for quantization."""

from typing import Callable, Iterable, Mapping, Tuple, Union

from tensorflow.python.types import core

# A representative sample should be either:
# 1. (signature_key, {input_name -> input_tensor}) tuple, or
# 2. {input_name -> input_tensor} mappings.
# TODO(b/236218728): Support data types other than Tensor (such as np.ndarrays).
RepresentativeSample = Union[Tuple[str, Mapping[str, core.Tensor]],
                             Mapping[str, core.Tensor]]

# A representative dataset should be a callable that returns an iterable
# of representative samples.
RepresentativeDataset = Callable[[], Iterable[RepresentativeSample]]
