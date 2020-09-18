#!/usr/bin/env bash
# Copyright 2019 The TensorFlow Authors. All Rights Reserved.
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
#
# Tests the microcontroller code for bluepill platform

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR=${SCRIPT_DIR}/../../../../..
cd "${ROOT_DIR}"
pwd

source tensorflow/lite/micro/tools/ci_build/helper_functions.sh

readable_run make -f tensorflow/lite/micro/tools/make/Makefile clean

TARGET=bluepill

# TODO(b/143715361): downloading first to allow for parallel builds.
readable_run make -f tensorflow/lite/micro/tools/make/Makefile TARGET=${TARGET} third_party_downloads

# check that the release build is ok.
readable_run make -f tensorflow/lite/micro/tools/make/Makefile clean
readable_run make -j8 -f tensorflow/lite/micro/tools/make/Makefile TARGET=${TARGET} build BUILD_TYPE=release

# TODO(b/168334217): enable debug build once it does not fail.
#readable_run make -f tensorflow/lite/micro/tools/make/Makefile clean
#readable_run make -j8 -f tensorflow/lite/micro/tools/make/Makefile TARGET=${TARGET} build BUILD_TYPE=debug

readable_run make -f tensorflow/lite/micro/tools/make/Makefile clean
readable_run make -j8 -f tensorflow/lite/micro/tools/make/Makefile TARGET=${TARGET} build

# TODO(b/149597202): Running tests via renode are disabled as part of the
# continuous integration until we can get Docker running inside Docker. However,
# if this script is run locally, the tests will still be run.
if [[ ${1} != "PRESUBMIT" ]]; then
readable_run make -f tensorflow/lite/micro/tools/make/Makefile TARGET=${TARGET} test
fi

