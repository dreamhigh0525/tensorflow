#!/bin/bash
# Copyright 2015 The TensorFlow Authors. All Rights Reserved.
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

#=============================================================================
# this script must be run from the root of the tensorflow git repository.
#
# it *can* be run outside docker/kokoro, on a dev machine, as long as cmake and
# ninja-build packages are installed, and the tensorflow PIP package (the one
# under test, presumably) is installed.
#
# the script creates a few directories under /tmp, see a few lines below.
#
# under kokoro, this is run by learning/brain/testing/kokoro/rel/docker/aot_compile.sh
#=============================================================================
set -euo pipefail -o history

echo "Building x_matmul_y models"
python3 \
  tensorflow/python/tools/make_aot_compile_models.py \
  --out_dir=/tmp/saved_models

# LINT.IfChange
GEN_ROOT=/tmp/generated_models
GEN_PREFIX="${GEN_ROOT}/tensorflow/python/tools"
PROJECT=/tmp/project
TF_THIRD_PARTY=/tmp/tf_third_party
# LINT.ThenChange(//tensorflow/tools/pip_package/xla_build/pip_test/CMakeLists.txt)

export PATH=$PATH:${HOME}/.local/bin

rm -rf "${GEN_ROOT}" "${PROJECT}" "${TF_THIRD_PARTY}"

# We don't want to -Itensorflow, to avoid unwanted dependencies.
echo "Copying third_party stuff (for eigen)"
mkdir -p "${TF_THIRD_PARTY}"
cp -rf third_party "${TF_THIRD_PARTY}/"

echo "AOT models"
saved_model_cli aot_compile_cpu \
  --dir tensorflow/cc/saved_model/testdata/VarsAndArithmeticObjectGraph \
  --output_prefix "${GEN_PREFIX}/aot_compiled_vars_and_arithmetic" \
  --variables_to_feed variable_x \
  --cpp_class VarsAndArithmetic --signature_def_key serving_default --tag_set serve

saved_model_cli aot_compile_cpu \
  --dir tensorflow/cc/saved_model/testdata/VarsAndArithmeticObjectGraph \
  --output_prefix "${GEN_PREFIX}/aot_compiled_vars_and_arithmetic_frozen" \
  --cpp_class VarsAndArithmeticFrozen --signature_def_key serving_default --tag_set serve

saved_model_cli aot_compile_cpu \
  --dir /tmp/saved_models/x_matmul_y_small \
  --output_prefix "${GEN_PREFIX}/aot_compiled_x_matmul_y_small" \
  --cpp_class XMatmulYSmall --signature_def_key serving_default --tag_set serve

saved_model_cli aot_compile_cpu \
  --dir /tmp/saved_models/x_matmul_y_large \
  --output_prefix "${GEN_PREFIX}/aot_compiled_x_matmul_y_large" \
  --cpp_class XMatmulYLarge --signature_def_key serving_default --tag_set serve

saved_model_cli aot_compile_cpu \
  --dir /tmp/saved_models/x_matmul_y_large \
  --output_prefix "${GEN_PREFIX}/aot_compiled_x_matmul_y_large_multithreaded" \
  --multithreading True \
  --cpp_class XMatmulYLargeMultithreaded --signature_def_key serving_default --tag_set serve

saved_model_cli aot_compile_cpu \
  --dir tensorflow/cc/saved_model/testdata/x_plus_y_v2_debuginfo \
  --output_prefix "${GEN_PREFIX}/aot_compiled_x_plus_y" \
  --cpp_class XPlusY --signature_def_key serving_default --tag_set serve

echo "Creaating project and copying object files"
mkdir -p "${PROJECT}"
cp -f \
  "${GEN_PREFIX}/aot_compiled_vars_and_arithmetic.o" \
  "${GEN_PREFIX}/aot_compiled_vars_and_arithmetic_frozen.o" \
  "${GEN_PREFIX}/aot_compiled_x_matmul_y_small.o" \
  "${GEN_PREFIX}/aot_compiled_x_matmul_y_large.o" \
  "${GEN_PREFIX}/aot_compiled_x_matmul_y_large_multithreaded.o" \
  "${GEN_PREFIX}/aot_compiled_x_plus_y.o" \
  "${PROJECT}"

echo "Copying build and source files"
cp tensorflow/tools/pip_package/xla_build/pip_test/CMakeLists.txt \
  tensorflow/python/tools/aot_compiled_test.cc \
  "${PROJECT}"

echo "Building"
mkdir "${PROJECT}/build"
cmake -GNinja -S "${PROJECT}" -B "${PROJECT}/build" -DCMAKE_BUILD_TYPE=Release
ninja -C "${PROJECT}/build"

echo "Running test"
"${PROJECT}/build/aot_compiled_test"
