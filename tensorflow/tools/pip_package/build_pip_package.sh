#!/usr/bin/env bash
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


set -e

function real_path() {
  [[ $1 = /* ]] && echo "$1" || echo "$PWD/${1#./}"
}

function cp_external() {
  local src_dir=$1
  local dest_dir=$2
  for f in `find "$src_dir" -maxdepth 1 -mindepth 1 ! -name '*local_config_cuda*'`; do
    cp -R "$f" "$dest_dir"
  done
}

PLATFORM="$(uname -s | tr 'A-Z' 'a-z')"
function is_windows() {
  # On windows, the shell script is actually running in msys
  if [[ "${PLATFORM}" =~ msys_nt* ]]; then
    true
  else
    false
  fi
}

function main() {
  if [ $# -lt 1 ] ; then
    echo "No destination dir provided"
    exit 1
  fi

  DEST=$(real_path $1)
  TMPDIR=$(mktemp -d -t tmp.XXXXXXXXXX)

  GPU_FLAG=""
  while true; do
    if [[ "$1" == "--gpu" ]]; then
      GPU_FLAG="--project_name tensorflow_gpu"
    elif [[ "$1" == "--gpudirect" ]]; then
      GPU_FLAG="--project_name tensorflow_gpudirect"
    fi
    shift

    if [[ -z "$1" ]]; then
      break
    fi
  done

  echo $(date) : "=== Using tmpdir: ${TMPDIR}"

  if [ ! -d bazel-bin/tensorflow ]; then
    echo "Could not find bazel-bin.  Did you run from the root of the build tree?"
    exit 1
  fi

  if is_windows; then
    rm -rf ./bazel-bin/tensorflow/tools/pip_package/simple_console_for_window_unzip
    mkdir -p ./bazel-bin/tensorflow/tools/pip_package/simple_console_for_window_unzip
    echo "Unzipping simple_console_for_windows.zip to create runfiles tree..."
    unzip -o -q ./bazel-bin/tensorflow/tools/pip_package/simple_console_for_windows.zip -d ./bazel-bin/tensorflow/tools/pip_package/simple_console_for_window_unzip
    echo "Unzip finished."
    # runfiles structure after unzip the python binary
    cp -R \
      bazel-bin/tensorflow/tools/pip_package/simple_console_for_window_unzip/runfiles/org_tensorflow/tensorflow \
      "${TMPDIR}"
    mkdir "${TMPDIR}/external"
    # Note: this makes an extra copy of org_tensorflow.
    cp_external \
      bazel-bin/tensorflow/tools/pip_package/simple_console_for_window_unzip/runfiles \
      "${TMPDIR}/external"
    RUNFILES=bazel-bin/tensorflow/tools/pip_package/simple_console_for_window_unzip/runfiles/org_tensorflow
  else
    if [ -d bazel-bin/tensorflow/tools/pip_package/build_pip_package.runfiles/org_tensorflow/external ]; then
      # Old-style runfiles structure (--legacy_external_runfiles).
      cp -R \
        bazel-bin/tensorflow/tools/pip_package/build_pip_package.runfiles/org_tensorflow/tensorflow \
        "${TMPDIR}"
      mkdir "${TMPDIR}/external"
      cp_external \
        bazel-bin/tensorflow/tools/pip_package/build_pip_package.runfiles/org_tensorflow/external \
        "${TMPDIR}/external"
      # Copy MKL libs over so they can be loaded at runtime
      so_lib_dir="bazel-bin/tensorflow/tools/pip_package/build_pip_package.runfiles/org_tensorflow/_solib_k8"
      if [ -d ${so_lib_dir} ]; then
        mkl_so_dir=$(ls ${so_lib_dir} | grep mkl)
        if [ $? -eq 0 ]; then
          mkdir "${TMPDIR}/_solib_k8"
          cp -R ${so_lib_dir}/${mkl_so_dir} "${TMPDIR}/_solib_k8"
        fi
      fi
    else
      # New-style runfiles structure (--nolegacy_external_runfiles).
      cp -R \
        bazel-bin/tensorflow/tools/pip_package/build_pip_package.runfiles/org_tensorflow/tensorflow \
        "${TMPDIR}"
      mkdir "${TMPDIR}/external"
      # Note: this makes an extra copy of org_tensorflow.
      cp_external \
        bazel-bin/tensorflow/tools/pip_package/build_pip_package.runfiles \
        "${TMPDIR}/external"
      # Copy MKL libs over so they can be loaded at runtime
      so_lib_dir="bazel-bin/tensorflow/tools/pip_package/build_pip_package.runfiles/org_tensorflow/_solib_k8"
      if [ -d ${so_lib_dir} ]; then
        mkl_so_dir=$(ls ${so_lib_dir} | grep mkl)
        if [ $? -eq 0 ]; then
          mkdir "${TMPDIR}/_solib_k8"
          cp -R ${so_lib_dir}/${mkl_so_dir} "${TMPDIR}/_solib_k8"
        fi
      fi
    fi
    RUNFILES=bazel-bin/tensorflow/tools/pip_package/build_pip_package.runfiles/org_tensorflow
  fi

  # protobuf pip package doesn't ship with header files. Copy the headers
  # over so user defined ops can be compiled.
  mkdir -p ${TMPDIR}/google
  mkdir -p ${TMPDIR}/third_party
  pushd ${RUNFILES%org_tensorflow}
  for header in $(find protobuf_archive -name \*.h); do
    mkdir -p "${TMPDIR}/google/$(dirname ${header})"
    cp "$header" "${TMPDIR}/google/$(dirname ${header})/"
  done
  popd
  cp -R $RUNFILES/third_party/eigen3 ${TMPDIR}/third_party

  cp tensorflow/tools/pip_package/MANIFEST.in ${TMPDIR}
  cp tensorflow/tools/pip_package/README ${TMPDIR}
  cp tensorflow/tools/pip_package/setup.py ${TMPDIR}

  # Before we leave the top-level directory, make sure we know how to
  # call python.
  source tools/python_bin_path.sh

  pushd ${TMPDIR}
  rm -f MANIFEST
  echo $(date) : "=== Building wheel"
  "${PYTHON_BIN_PATH:-python}" setup.py bdist_wheel ${GPU_FLAG} >/dev/null
  mkdir -p ${DEST}
  cp dist/* ${DEST}
  popd
  rm -rf ${TMPDIR}
  echo $(date) : "=== Output wheel file is in: ${DEST}"
}

main "$@"
