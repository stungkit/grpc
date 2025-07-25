# Copyright 2018 The gRPC Authors
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

FROM debian:11

#=================
# Basic C core dependencies

# C/C++ dependencies according to https://github.com/grpc/grpc/blob/master/BUILDING.md
RUN apt-get update && apt-get install -y \
  build-essential \
  autoconf \
  libtool \
  pkg-config \
  && apt-get clean

# GCC
RUN apt-get update && apt-get install -y \
  gcc \
  g++ \
  && apt-get clean

# libc6
RUN apt-get update && apt-get install -y \
  libc6 \
  libc6-dbg \
  libc6-dev \
  && apt-get clean

# Tools
RUN apt-get update && apt-get install -y \
  bzip2 \
  curl \
  dnsutils \
  git \
  lcov \
  make \
  strace \
  time \
  unzip \
  wget \
  zip \
  && apt-get clean

#=================
# Setup git to access working directory across docker boundary.
# This avoids the "fatal: detected dubious ownership in repository XYZ"
# git error.

RUN git config --global --add safe.directory '*'
RUN git config --global protocol.file.allow always



RUN mkdir /var/local/jenkins


#=================
# Compile CPython 3.10.3 from source

RUN apt-get update && apt-get install -y zlib1g-dev libssl-dev libsqlite3-dev && apt-get clean
RUN apt-get update && apt-get install -y jq build-essential libffi-dev && apt-get clean

RUN cd /tmp && \
    wget -q https://www.python.org/ftp/python/3.10.3/Python-3.10.3.tgz && \
    tar xzvf Python-3.10.3.tgz && \
    cd Python-3.10.3 && \
    ./configure && \
    make -j4 && \
    make install

RUN cd /tmp && \
    echo "f276ffcd05bccafe46da023d0a5bb04a Python-3.10.3.tgz" > checksum.md5 && \
    md5sum -c checksum.md5

RUN python3.10 -m ensurepip && \
    python3.10 -m pip install coverage

#=================
# Compile CPython 3.11.4 from source

RUN apt-get update && apt-get install -y zlib1g-dev libssl-dev libsqlite3-dev && apt-get clean
RUN apt-get update && apt-get install -y jq build-essential libffi-dev && apt-get clean

RUN cd /tmp && \
    wget -q https://www.python.org/ftp/python/3.11.4/Python-3.11.4.tgz && \
    tar xzvf Python-3.11.4.tgz && \
    cd Python-3.11.4 && \
    ./configure && \
    make -j4 && \
    make install


RUN cd /tmp && \
    echo "bf6ec50f2f3bfa6ffbdb385286f2c628  Python-3.11.4.tgz" > checksum.md5 && \
    md5sum -c checksum.md5

RUN python3.11 -m ensurepip && \
    python3.11 -m pip install coverage

#=================
# Compile CPython 3.12.0 from source

RUN apt-get update && apt-get install -y zlib1g-dev libssl-dev libsqlite3-dev && apt-get clean
RUN apt-get update && apt-get install -y jq build-essential libffi-dev && apt-get clean

RUN cd /tmp && \
    wget -q https://www.python.org/ftp/python/3.12.0/Python-3.12.0.tgz && \
    tar xzvf Python-3.12.0.tgz && \
    cd Python-3.12.0 && \
    ./configure && \
    make -j4 && \
    make install


RUN cd /tmp && \
    echo "d6eda3e1399cef5dfde7c4f319b0596c  Python-3.12.0.tgz" > checksum.md5 && \
    md5sum -c checksum.md5

RUN python3.12 -m ensurepip && \
    python3.12 -m pip install coverage

#=================
# Compile CPython 3.13.0rc2 from source

RUN apt-get update && apt-get install -y zlib1g-dev libssl-dev libsqlite3-dev && apt-get clean
RUN apt-get update && apt-get install -y jq build-essential libffi-dev && apt-get clean

RUN cd /tmp && \
    wget -q https://www.python.org/ftp/python/3.13.0/Python-3.13.0rc2.tgz && \
    tar xzvf Python-3.13.0rc2.tgz && \
    cd Python-3.13.0rc2 && \
    ./configure && \
    make -j4 && \
    make install


RUN cd /tmp && \
    echo "ad7f44153649e27ec385e7633e853e03  Python-3.13.0rc2.tgz" > checksum.md5 && \
    md5sum -c checksum.md5

RUN python3.13 -m ensurepip && \
    python3.13 -m pip install coverage


# Python test coverage requires libsqlite3, and it have
# to be installed before Python.
RUN apt-get update && apt-get install -y libsqlite3-dev

# 3.9 is the default python3 version on debian11
RUN apt-get update && apt-get install -y python3.9 python3.9-dev python3-pip

# for Python test coverage reporting
RUN python3.9 -m pip install coverage

# six is required by the run_tests.py test harness
RUN python3.9 -m pip install six

# Google Cloud Platform API libraries
# These are needed for uploading test results to BigQuery (e.g. by tools/run_tests scripts)
RUN python3 -m pip install --upgrade google-auth==1.23.0 google-api-python-client==1.12.8 oauth2client==4.1.0


#=================
# Install cmake
# Note that this step should be only used for distributions that have new enough cmake to satisfy gRPC's cmake version requirement.

RUN apt-get update && apt-get install -y cmake && apt-get clean

#=================
# Install ccache

# Install ccache from source since ccache 3.x packaged with most linux distributions
# does not support Redis backend for caching.
RUN curl -sSL -o ccache.tar.gz https://github.com/ccache/ccache/releases/download/v4.11.3/ccache-4.11.3.tar.gz \
    && tar -zxf ccache.tar.gz \
    && cd ccache-4.11.3 \
    && mkdir build && cd build \
    && cmake -DCMAKE_BUILD_TYPE=Release -DZSTD_FROM_INTERNET=ON -DHIREDIS_FROM_INTERNET=ON .. \
    && make -j4 && make install \
    && cd ../.. \
    && rm -rf ccache-4.11.3 ccache.tar.gz


