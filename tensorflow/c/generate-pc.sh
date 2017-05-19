#!/usr/bin/env bash
# Copyright 2017 The TensorFlow Authors. All Rights Reserved.
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

TF_PREFIX='/usr/local'

usage() {
    echo "Usage: $0 OPTIONS"
    echo -e "-p, --prefix\tset installation prefix (default: /usr/local)"
    echo -e "-v, --version\tset TensorFlow version"
    echo -e "-h, --help\tdisplay this message"
}

# read the options
ARGS=`getopt -o p:v:h --long prefix:,version:,help -n $0 -- "$@"`
eval set -- "$ARGS"

# extract options and their arguments into variables.
while true ; do
    case "$1" in
        -h|--help) usage ; exit ;;
        -p|--prefix)
            case "$2" in
                "") shift 2 ;;
                *) TF_PREFIX=$2 ; shift 2 ;;
            esac ;;
        -v|--version)
            case "$2" in
                "") shift 2 ;;
                *) TF_VERSION=$2 ; shift 2 ;;
            esac ;;
        --) shift ; echo "Try '$0 --help' for more information."; exit 1 ;;
        *) echo "Internal error! Try '$0 --help' for more information." ; exit 1 ;;
    esac
done

echo "Generating pkgconfig file for TensorFlow $TF_VERSION in $TF_PREFIX"

cat << EOF > tensorflow.pc
prefix=${TF_PREFIX}
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: TensorFlow
Version: ${TF_VERSION}
Description: Library for computation using data flow graphs for scalable machine learning
Requires:
Libs: -L\${libdir} -ltensorflow
Cflags: -I\${includedir}
EOF
