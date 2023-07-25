#!/bin/bash
#
# Script to install dependencies for EloqDoc
# Usage: ./install_eloq_dependancy.sh <TEMP_DIR>
#   TEMP_DIR: Required. Directory for downloads and installations

set -eo pipefail

# ------------- Logging functions -------------
# Define color codes
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[0;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1" >&2
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1" >&2
}

# ------------- Command line argument parsing -------------
if [ $# -lt 1 ]; then
    log_error "TEMP_DIR is required"
    echo "Usage: $0 <TEMP_DIR>"
    exit 1
fi

# Set TEMP_DIR
TEMP_DIR=$1
log_info "Using directory for downloads and installations: ${TEMP_DIR}"

# Create directory if it doesn't exist
mkdir -p "$TEMP_DIR"

export DEBIAN_FRONTEND=noninteractive
export CASSANDRA_VERSION=4.0.6

# ------------- System packages installation -------------
log_info "Installing required system packages"
sudo apt update
# Consolidated all apt-get installations into a single command
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    jq sudo vim wget curl gdb libcurl4-openssl-dev build-essential \
    libncurses5-dev libncursesw5-dev gnutls-dev bison zlib1g-dev ccache \
    cmake ninja-build libuv1-dev git g++ make openjdk-11-jdk openssh-client \
    libssl-dev libgflags-dev libleveldb-dev libsnappy-dev openssl lcov \
    libbz2-dev liblz4-dev libzstd-dev libboost-context-dev ca-certificates \
    libc-ares-dev libc-ares2 m4 pkg-config tar libreadline-dev libsqlite3-dev \
    llvm xz-utils tk-dev libffi-dev liblzma-dev

# ------------- Cassandra installation -------------
log_info "Installing Apache Cassandra ${CASSANDRA_VERSION}"
cd $TEMP_DIR
if [ ! -d "apache-cassandra" ]; then
    wget https://archive.apache.org/dist/cassandra/${CASSANDRA_VERSION}/apache-cassandra-${CASSANDRA_VERSION}-bin.tar.gz &&
        tar xzvf apache-cassandra-${CASSANDRA_VERSION}-bin.tar.gz &&
        mv apache-cassandra-${CASSANDRA_VERSION} apache-cassandra &&
        rm -rf apache-cassandra-${CASSANDRA_VERSION}-bin.tar.gz
fi

# ------------- Mimalloc installation -------------
log_info "Installing mimalloc"
cd $TEMP_DIR
if [ ! -d "mimalloc" ]; then
    git clone https://github.com/monographdb/mimalloc.git mimalloc
fi
cd mimalloc && \
git checkout monograph-v2.1.2 && \
mkdir -p bld && cd bld && \
cmake .. && \
make -j6 && \
sudo make install

# ------------- Protobuf installation -------------
# Compile protobuf from source code. Protobuf version needs to be compatible
# with both brpc and grpc. It cannot be too high or too low.
log_info "Installing protobuf"
cd $TEMP_DIR
if [ ! -d "protobuf" ]; then
    mkdir -p protobuf
fi
cd protobuf
curl -fsSL https://github.com/protocolbuffers/protobuf/archive/refs/tags/v21.12.tar.gz | \
tar -xzf - --strip-components=1 && \
cmake \
-DCMAKE_BUILD_TYPE=Release \
-DBUILD_SHARED_LIBS=yes \
-Dprotobuf_BUILD_TESTS=OFF \
-Dprotobuf_ABSL_PROVIDER=package \
-S . -B cmake-out && \
cmake --build cmake-out -- -j ${NCPU:-4} && \
sudo cmake --build cmake-out --target install -- -j ${NCPU:-4} && \
sudo ldconfig

# ------------- Glog installation -------------
log_info "Installing glog"
cd $TEMP_DIR
if [ ! -d "glog" ]; then
    git clone https://github.com/monographdb/glog.git glog
fi
cd glog && \
cmake -S . -B build -G "Unix Makefiles" && \
cmake --build build -j6 && \
sudo cmake --build build --target install 

# ------------- Liburing installation -------------
log_info "Installing liburing"
cd $TEMP_DIR
if [ ! -d "liburing" ]; then
    git clone https://github.com/axboe/liburing.git liburing
fi
cd liburing && \
git checkout tags/liburing-2.6 && \
./configure --cc=gcc --cxx=g++ && \
make -j4 && \
sudo make install

# ------------- BRPC installation -------------
# Build brpc after protobuf and glog
log_info "Installing brpc"
cd $TEMP_DIR
if [ ! -d "brpc" ]; then
    git clone https://github.com/monographdb/brpc.git brpc
fi
cd brpc && \
mkdir -p build && cd build && \
cmake .. \
-DWITH_GLOG=ON \
-DIO_URING_ENABLED=ON \
-DBUILD_SHARED_LIBS=ON && \
cmake --build . -j6 && \
sudo cp -r ./output/include/* /usr/include/ && \
sudo cp ./output/lib/* /usr/lib/

# ------------- BRAFT installation -------------
log_info "Installing braft"
cd $TEMP_DIR
if [ ! -d "braft" ]; then
    git clone https://github.com/monographdb/braft.git braft
fi
cd braft && \
sed -i 's/libbrpc.a//g' CMakeLists.txt && \
mkdir -p bld && cd bld && \
cmake .. -DBRPC_WITH_GLOG=ON && \
cmake --build . -j6 && \
sudo cp -r ./output/include/* /usr/include/ && \
sudo cp ./output/lib/* /usr/lib/

# ------------- Cuckoo filter installation -------------
log_info "Installing cuckoo filter"
cd $TEMP_DIR
if [ ! -d "cuckoofilter" ]; then
    git clone https://github.com/monographdb/cuckoofilter.git cuckoofilter
fi
cd cuckoofilter && \
sudo make install

# ------------- AWS SDK installation -------------
log_info "Installing AWS SDK"
cd $TEMP_DIR
if [ ! -d "aws" ]; then
    git clone --recurse-submodules https://github.com/aws/aws-sdk-cpp.git aws
fi
cd aws && \
git checkout tags/1.11.446 && \
mkdir -p bld && cd bld && \
cmake .. \
-DCMAKE_BUILD_TYPE=RelWithDebInfo \
-DCMAKE_INSTALL_PREFIX=./output/ \
-DENABLE_TESTING=OFF \
-DBUILD_SHARED_LIBS=ON \
-DFORCE_SHARED_CRT=OFF \
-DBUILD_ONLY="dynamodb;sqs;s3;kinesis;kafka;transfer" && \
cmake --build . --config RelWithDebInfo -j6 && \
cmake --install . --config RelWithDebInfo && \
sudo cp -r ./output/include/* /usr/include/ && \
sudo cp -r ./output/lib/* /usr/lib/ 

# ------------- RocksDB installation -------------
log_info "Installing RocksDB"
cd $TEMP_DIR
if [ ! -d "rocksdb" ]; then
    git clone https://github.com/facebook/rocksdb.git rocksdb
fi
cd rocksdb && \
git checkout tags/v9.1.0 && \
USE_RTTI=1 PORTABLE=1 ROCKSDB_DISABLE_TCMALLOC=1 ROCKSDB_DISABLE_JEMALLOC=1 make -j8 shared_lib && \
sudo make install-shared && \
cd ../ && \
sudo ldconfig

# ------------- Prometheus CPP installation -------------
log_info "Installing prometheus-cpp"
cd $TEMP_DIR
if [ ! -d "prometheus-cpp" ]; then
    git clone https://github.com/jupp0r/prometheus-cpp.git prometheus-cpp
fi
cd prometheus-cpp && \
git checkout tags/v1.1.0 && \
git submodule init && git submodule update && \
mkdir -p _build && cd _build && \
cmake .. -DBUILD_SHARED_LIBS=ON && \
cmake --build . -j6 && \
sudo cmake --install .

# ------------- Python 2.7.18 installation with pyenv -------------
# Python 2.7.18 is required for building Eloqdoc
log_info "Installing Python 2.7.18 with pyenv (required for Eloqdoc build process)"

# Install pyenv
cd $TEMP_DIR
curl -fsSL https://pyenv.run | bash

# Set up pyenv environment
echo 'export PYENV_ROOT="$HOME/.pyenv"' >> ~/.bashrc
echo '[[ -d $PYENV_ROOT/bin ]] && export PATH="$PYENV_ROOT/bin:$PATH"' >> ~/.bashrc
echo 'eval "$(pyenv init - bash)"' >> ~/.bashrc
source ~/.bashrc

# Install
pyenv install 2.7.18
pyenv global 2.7.18

log_info "EloqDoc dependency installation completed successfully"