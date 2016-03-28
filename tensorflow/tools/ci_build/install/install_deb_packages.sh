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

# Install FFmpeg from an alternate repository.
apt-get install -y software-properties-common
add-apt-repository -y ppa:mc3man/trusty-media
apt-get update

# Install dependencies from ubuntu deb repository.
apt-get update
apt-get install -y \
    bc \
    build-essential \
    cmake \
    curl \
    ffmpeg \
    git \
    openjdk-8-jdk \
    openjdk-8-jre-headless \
    pkg-config \
    python-dev \
    python-numpy \
    python-pip \
    python-scipy \
    python-virtualenv \
    python3-dev \
    python3-numpy \
    python3-pip \
    python3-scipy \
    sudo \
    swig \
    unzip \
    wget \
    zip \
    zlib1g-dev
apt-get clean
rm -rf /var/lib/apt/lists/*
