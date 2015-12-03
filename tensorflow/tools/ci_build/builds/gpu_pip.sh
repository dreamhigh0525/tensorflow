#!/usr/bin/env bash
# Copyright 2015 Google Inc. All Rights Reserved.
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

set -e

export TF_NEED_CUDA=1
./configure
bazel build -c opt --config=cuda //tensorflow/tools/pip_package:build_pip_package
rm -rf /root/.cache/tensorflow-pip
bazel-bin/tensorflow/tools/pip_package/build_pip_package /root/.cache/tensorflow-pip
