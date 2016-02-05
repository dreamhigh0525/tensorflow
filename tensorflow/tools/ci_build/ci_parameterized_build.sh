#!/usr/bin/env bash
# Copyright 2016 Google Inc. All Rights Reserved.
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
# Usage:
#   ci_parameterized_build.sh
#
# The script obeys the following required environment variables:
#   TF_BUILD_CONTAINER_TYPE:   (CPU | GPU | ANDROID)
#   TF_BUILD_PYTHON_VERSION:   (PYTHON2 | PYTHON3)
#   TF_BUILD_IS_OPT:           (NO_OPT | OPT)
#   TF_BUILD_IS_PIP:           (NO_PIP | PIP)
#
# Note: certain combinations of parameter values are regarded
# as invalid and will cause the script to exit with code 0. For example:
#   NO_OPT & PIP     (PIP builds should always use OPT)
#   ANDROID & PIP    (Android and PIP builds are mutually exclusive)
#
# Additionally, the script follows the directions of optional environment
# variables:
#   TF_BUILD_DRY_RUN:  If it is set to any non-empty value that is not "0",
#                      the script will just generate and print the final
#                      command, but not actually run it.
#   TF_BUILD_APPEND_CI_DOCKER_EXTRA_PARAMS:
#                      String appended to the content of CI_DOCKER_EXTRA_PARAMS
#   TF_BUILD_APPEND_ARGUMENTS:
#                      Additional command line arguments for the bazel,
#                      pip.sh or android.sh command
#   TF_BUILD_BAZEL_TARGET:
#                      Used to override the default bazel build target:
#                      //tensorflow/...
#
# This script can be used by Jenkins parameterized / matrix builds.

# Helper function: Convert to lower case
to_lower () {
  echo "$1" | tr '[:upper:]' '[:lower:]'
}

# Helper function: Strip leading and trailing whitespaces
str_strip () {
  echo -e "$1" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//'
}


##########################################################
# Default configuration
CI_BUILD_DIR="tensorflow/tools/ci_build"

# Command to call when Docker is available
DOCKER_MAIN_CMD="${CI_BUILD_DIR}/ci_build.sh"
# Command to call when Docker is unavailable
NO_DOCKER_MAIN_CMD="${CI_BUILD_DIR}/builds/configured"

# Additional option flags to apply when Docker is unavailable (e.g., on Mac)
NO_DOCKER_OPT_FLAG="--linkopt=-headerpad_max_install_names"

BAZEL_CMD="bazel test"
PIP_CMD="${CI_BUILD_DIR}/builds/pip.sh"
ANDROID_CMD="${CI_BUILD_DIR}/builds/android.sh"

BAZEL_TARGET="//tensorflow/..."
##########################################################

# Convert all the required environment variables to lower case
TF_BUILD_CONTAINER_TYPE=$(to_lower ${TF_BUILD_CONTAINER_TYPE})
TF_BUILD_PYTHON_VERSION=$(to_lower ${TF_BUILD_PYTHON_VERSION})
TF_BUILD_IS_OPT=$(to_lower ${TF_BUILD_IS_OPT})
TF_BUILD_IS_PIP=$(to_lower ${TF_BUILD_IS_PIP})

# Print parameter values
echo "Required build parameters:"
echo "  TF_BUILD_CONTAINER_TYPE=${TF_BUILD_CONTAINER_TYPE}"
echo "  TF_BUILD_PYTHON_VERSION=${TF_BUILD_PYTHON_VERSION}"
echo "  TF_BUILD_IS_OPT=${TF_BUILD_IS_OPT}"
echo "  TF_BUILD_IS_PIP=${TF_BUILD_IS_PIP}"
echo "Optional build parameters:"
echo "  TF_BUILD_DRY_RUN=${TF_BUILD_DRY_RUN}"
echo "  TF_BUILD_APPEND_CI_DOCKER_EXTRA_PARAMS="\
"${TF_BUILD_APPEND_CI_DOCKER_EXTRA_PARAMS}"
echo "  TF_BUILD_APPEND_ARGUMENTS=${TF_BUILD_APPEND_ARGUMENTS}"
echo "  TF_BUILD_BAZEL_TARGET=${TF_BUILD_BAZEL_TARGET}"

# Process container type
CTYPE=${TF_BUILD_CONTAINER_TYPE}
OPT_FLAG=""
if [[ ${CTYPE} == "cpu" ]]; then
  :
elif [[ ${CTYPE} == "gpu" ]]; then
  OPT_FLAG="--config=cuda"
elif [[ ${CTYPE} == "android" ]]; then
  :
else
  echo "Unrecognized value in TF_BUILD_CONTAINER_TYPE: "\
"\"${TF_BUILD_CONTAINER_TYPE}\""
  exit 1
fi

EXTRA_PARAMS=""

# Determine if Docker is available
MAIN_CMD=${DOCKER_MAIN_CMD}
if [[ -z "$(which docker)" ]]; then
  echo "It appears that Docker is not available on this system. "\
"Will perform build without Docker."
  echo "In addition, the additional option flags will be applied to the build:"
  echo "  ${NO_DOCKER_OPT_FLAG}"
  MAIN_CMD=${NO_DOCKER_MAIN_CMD}
  OPT_FLAG="${OPT_FLAG} ${NO_DOCKER_OPT_FLAG}"

fi

# Process Bazel "-c opt" flag
if [[ ${TF_BUILD_IS_OPT} == "no_opt" ]]; then
  # PIP builds are done only with the -c opt flag
  if [[ ${TF_BUILD_IS_PIP} == "pip" ]]; then
    echo "Skipping parameter combination: ${TF_BUILD_IS_OPT} & "\
"${TF_BUILD_IS_PIP}"
    exit 0
  fi

elif [[ ${TF_BUILD_IS_OPT} == "opt" ]]; then
  OPT_FLAG="${OPT_FLAG} -c opt"
else
  echo "Unrecognized value in TF_BUILD_IS_OPT: \"${TF_BUILD_IS_OPT}\""
  exit 1
fi

# Strip whitespaces from OPT_FLAG
OPT_FLAG=$(str_strip "${OPT_FLAG}")

# Process PIP install-test option
if [[ ${TF_BUILD_IS_PIP} == "no_pip" ]]; then
  # Process optional bazel target override
  if [[ ! -z "${TF_BUILD_BAZEL_TARGET}" ]]; then
    BAZEL_TARGET=${TF_BUILD_BAZEL_TARGET}
  fi

  if [[ ${CTYPE} == "cpu" ]] || [[ ${CTYPE} == "gpu" ]]; then
    # Run Bazel
    MAIN_CMD="${MAIN_CMD} ${CTYPE} ${BAZEL_CMD} ${OPT_FLAG} "\
"${TF_BUILD_APPEND_ARGUMENTS} ${BAZEL_TARGET}"
  elif [[ ${CTYPE} == "android" ]]; then
    MAIN_CMD="${MAIN_CMD} ${CTYPE} ${ANDROID_CMD} ${OPT_FLAG} "
  fi
elif [[ ${TF_BUILD_IS_PIP} == "pip" ]]; then
  # Android builds conflict with PIP builds
  if [[ ${CTYPE} == "android" ]]; then
    echo "Skipping parameter combination: ${TF_BUILD_IS_PIP} & "\
"${TF_BUILD_CONTAINER_TYPE}"
    exit 0
  fi

  MAIN_CMD="${MAIN_CMD} ${CTYPE} ${PIP_CMD} ${CTYPE} "\
"${TF_BUILD_APPEND_ARGUMENTS}"
else
  echo "Unrecognized value in TF_BUILD_IS_PIP: \"${TF_BUILD_IS_PIP}\""
  exit 1
fi

# Process Python version
if [[ ${TF_BUILD_PYTHON_VERSION} == "python2" ]]; then
  :
elif [[ ${TF_BUILD_PYTHON_VERSION} == "python3" ]]; then
  EXTRA_PARAMS="${EXTRA_PARAMS} -e PYTHON_BIN_PATH=/usr/bin/python3"
else
  echo "Unrecognized value in TF_BUILD_PYTHON_VERSION: "\
"\"${TF_BUILD_PYTHON_VERSION}\""
  exit 1
fi

# Append additional Docker extra parameters
EXTRA_PARAMS="${EXTRA_PARAMS} ${TF_BUILD_APPEND_CI_DOCKER_EXTRA_PARAMS}"

# Strip leading and trailing whitespaces
EXTRA_PARAMS=$(str_strip "${EXTRA_PARAMS}")

# Finally, do a dry run or call the command
echo "Final command assembled by parameterized build: "
echo "CI_DOCKER_EXTRA_PARAMS=\"${EXTRA_PARAMS}\" ${MAIN_CMD}"
if [[ ! -z "${TF_BUILD_DRY_RUN}" ]] && [[ ${TF_BUILD_DRY_RUN} != "0" ]]; then
  # Do a dry run: just print the final command
  echo "*** This is a DRY RUN ***"
else
  # Call the command
  echo "Executing final command..."
  CI_DOCKER_EXTRA_PARAMS="${EXTRA_PARAMS}" ${MAIN_CMD}
fi
