#!/bin/bash
# Copyright 2022 Google LLC.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


#
# Build YDF CLI binaries and pack them in a zip file.
#
# Arguments:
#
#   INSTALL_DEPENDENCIES: Install the build dependencies. Useful to build in a
#     fresh docker. See "build_binary_release_in_docker.sh"
#
#   BUILD: Build the library.
#
#   PACK: Pack the library in a zip file.
#
# Usage example:
#  INSTALL_DEPENDENCIES=1 BUILD=1 PACK=1 ./tools/build_binary_release.sh

set -vex

# Install the build dependencies
if [[ ! -z ${INSTALL_DEPENDENCIES+z} ]]; then

apt-get update
apt-get -y --no-install-recommends install \
  ca-certificates \
  build-essential \
  g++-10 \
  clang-10 \
  git \
  python3 \
  python3-pip \
  zip \
  wget

python3 -m pip install numpy

wget -O bazelisk https://github.com/bazelbuild/bazelisk/releases/download/v1.14.0/bazelisk-linux-amd64
chmod +x bazelisk

fi

# Build the CLI
if [[ ! -z ${BUILD+z} ]]; then
  # TensorFlow compatible build.
  cp -f WORKSPACE_WITH_TF WORKSPACE

  BAZEL="./bazelisk"
  FLAGS="--config=linux_cpp17 --config=linux_avx2 --features=-fully_static_link --config=use_tensorflow_io --repo_env=CC=clang-10"
  ${BAZEL} build //yggdrasil_decision_forests/cli/...:all \
    //yggdrasil_decision_forests/utils/distribute/implementations/grpc:grpc_worker_main ${FLAGS}

  chmod -R a+rw .
fi

# Pack the CLI in a zip file
if [[ ! -z ${PACK+z} ]]; then
  CLI="bazel-bin/yggdrasil_decision_forests/cli"

  cp -f configure/cli_readme.txt ${CLI}/README
  cp -f documentation/cli.txt ${CLI}/

  pushd ${CLI}
  zip -j cli_linux.zip \
    README \
    cli.txt \
    train \
    show_model \
    show_dataspec \
    predict \
    infer_dataspec \
    evaluate \
    convert_dataset \
    benchmark_inference \
    edit_model \
    utils/synthetic_dataset \
    compute_variable_importances \
    ../utils/distribute/implementations/grpc/grpc_worker_main
  popd

  mkdir -p dist
  mv ${CLI}/cli_linux.zip dist/
  zip dist/cli_linux.zip LICENSE CHANGELOG.md

  chmod -R a+rw .
fi
