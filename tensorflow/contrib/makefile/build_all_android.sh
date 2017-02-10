#!/usr/bin/env bash
# Copyright 2016 The TensorFlow Authors. All Rights Reserved.
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
# This is a composite script to build all for Android OS

set -e

usage() {
  echo "Usage: NDK_ROOT=<path to ndk root> $(basename "$0") [-t:]"
  echo "-s [sub_makefiles] sub makefiles separated by white space"
  echo "-t [build_target] build target for Android makefile [default=all]"
  echo "-T only build tensorflow"
  echo "-x use hexagon library located at tensorflow/contrib/makefile/downloads/hexagon"
  echo "-X download hexagon deps and run hexagon_graph_execution"
  exit 1
}

if [[ -z "${NDK_ROOT}" ]]; then
    echo "NDK_ROOT should be set as an environment variable" 1>&2
    exit 1
fi

while getopts "s:t:TxX" opt_name; do
  case "$opt_name" in
    s) SUB_MAKEFILES="${OPTARG}";;
    t) BUILD_TARGET="${OPTARG}";;
    T) ONLY_MAKE_TENSORFLOW="true";;
    x) USE_HEXAGON="true";;
    X) DOWNLOAD_AND_USE_HEXAGON="true";;
    *) usage;;
  esac
done
shift $((OPTIND - 1))

# Make sure we're in the correct directory, at the root of the source tree.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"/../../../

source "${SCRIPT_DIR}/build_helper.subr"
JOB_COUNT="${JOB_COUNT:-$(get_job_count)}"

HEXAGON_DOWNLOAD_PATH="tensorflow/contrib/makefile/downloads/hexagon"

if [[ "${ONLY_MAKE_TENSORFLOW}" != "true" ]]; then
  # Remove any old files first.
  make -f tensorflow/contrib/makefile/Makefile clean
  rm -rf tensorflow/contrib/makefile/downloads
  # Pull down the required versions of the frameworks we need.
  tensorflow/contrib/makefile/download_dependencies.sh
  # Compile protobuf for the target Android device architectures.
  CC_PREFIX="${CC_PREFIX}" NDK_ROOT="${NDK_ROOT}" \
tensorflow/contrib/makefile/compile_android_protobuf.sh -c
else
  # Only clean files generated by make
  make -f tensorflow/contrib/makefile/Makefile clean_except_protobuf_libs
fi

if [[ "${DOWNLOAD_AND_USE_HEXAGON}" == "true" ]]; then
    URL_BASE="https://storage.googleapis.com/download.tensorflow.org"

    rm -rf "${HEXAGON_DOWNLOAD_PATH}"
    mkdir -p "${HEXAGON_DOWNLOAD_PATH}/libs"

    download_and_push "${URL_BASE}/deps/hexagon/libhexagon_controller.so" \
"${HEXAGON_DOWNLOAD_PATH}/libs/libhexagon_controller.so" "/data/local/tmp"

    download_and_push "${URL_BASE}/deps/hexagon/libhexagon_nn_skel.so" \
"${HEXAGON_DOWNLOAD_PATH}/libs/libhexagon_nn_skel.so" "/vendor/lib/rfsa/adsp"

    download_and_push "${URL_BASE}/example_images/img_299x299.jpg" \
"${HEXAGON_DOWNLOAD_PATH}/img_299x299.jpg" "/data/local/tmp"

    USE_HEXAGON="true"
    SUB_MAKEFILES="$(pwd)/tensorflow/contrib/makefile/sub_makefiles/hexagon_graph_execution/Makefile.in"
    BUILD_TARGET="hexagon_graph_execution"
fi

if [[ "${USE_HEXAGON}" == "true" ]]; then
    HEXAGON_PARENT_DIR=$(cd "${HEXAGON_DOWNLOAD_PATH}" && pwd)
    HEXAGON_LIBS="${HEXAGON_PARENT_DIR}/libs"
    HEXAGON_INCLUDE=$(cd "tensorflow/core/platform/hexagon" && pwd)
fi

if [[ -z "${BUILD_TARGET}" ]]; then
    make -j"${JOB_COUNT}" -f tensorflow/contrib/makefile/Makefile \
         TARGET=ANDROID NDK_ROOT="${NDK_ROOT}" CC_PREFIX="${CC_PREFIX}" \
HEXAGON_LIBS="${HEXAGON_LIBS}" HEXAGON_INCLUDE="${HEXAGON_INCLUDE}" \
SUB_MAKEFILES="${SUB_MAKEFILES}"
else
    make -j"${JOB_COUNT}" -f tensorflow/contrib/makefile/Makefile \
         TARGET=ANDROID NDK_ROOT="${NDK_ROOT}" CC_PREFIX="${CC_PREFIX}" \
HEXAGON_LIBS="${HEXAGON_LIBS}" HEXAGON_INCLUDE="${HEXAGON_INCLUDE}" \
SUB_MAKEFILES="${SUB_MAKEFILES}" "${BUILD_TARGET}"
fi

if [[ "${DOWNLOAD_AND_USE_HEXAGON}" == "true" ]]; then
    ANDROID_EXEC_FILE_MODE=755
    echo "Run hexagon_graph_execution"
    adb push -p "./tensorflow/contrib/makefile/gen/bin/hexagon_graph_execution" "/data/local/tmp/"
    adb wait-for-device
    adb shell chmod "${ANDROID_EXEC_FILE_MODE}" "/data/local/tmp/hexagon_graph_execution"
    adb wait-for-device
    adb shell 'LD_LIBRARY_PATH=/data/local/tmp:$LD_LIBRARY_PATH' \
    "/data/local/tmp/hexagon_graph_execution"
fi
