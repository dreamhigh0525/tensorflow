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
# pylint: disable=invalid-name
"""MobileNet v2 models for Keras.
"""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from keras_applications import mobilenet_v2

from tensorflow.python.util.tf_export import tf_export

MobileNetV2 = mobilenet_v2.MobileNetV2
decode_predictions = mobilenet_v2.decode_predictions
preprocess_input = mobilenet_v2.preprocess_input

tf_export('keras.applications.mobilenet_v2.MobileNetV2',
          'keras.applications.MobileNetV2')(MobileNetV2)
tf_export('keras.applications.mobilenet_v2.preprocess_input')(preprocess_input)
