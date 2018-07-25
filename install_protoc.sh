#! /bin/bash

RELEASE="v1.12.x"
LATEST=$(curl -s -L https://grpc.io/release)

log() {
    printf "\e[93m${1}\e[39m\n"
}
logerror() {
    printf "\e[91m${1}\e[39m\n"
}

if [ X"$RELEASE" != X"$LATEST" ] ; then
    printf "\e[91mWARNING - Latest release is ${LATEST}, this script is installing ${RELEASE}\e[39m\n"
fi

log "Installing build prerequisites"
if [ -e /etc/redhat-release ] ; then 
    sudo yum install -y gcc gcc-c++ git autoconf libtool #clang 
elif grep ubuntu /etc/os-release ; then
    sudo apt-get install -y gcc g++ git autoconf libtool
else
    logerror "I don't know how to build on this OS"
    exit 1
fi

pushd /tmp
if [ ! -e protobuf-cpp-3.5.1.tar.gz ]; then
    log "Getting protobuf-cpp-3.5.1.tar.gz"
    wget https://github.com/google/protobuf/releases/download/v3.5.1/protobuf-cpp-3.5.1.tar.gz
else 
    log "Protobuf release archive already downloaded"
fi

if [ ! -e protobuf-3.5.1 ]; then
    log "Extracting protobuf source"
    tar -x -z -f protobuf-cpp-3.5.1.tar.gz
else 
    log "Protobuf release archive already untarred"
fi

if [ ! -x "$(command -v protoc)" ]; then
    log "Building protobuffer..."
    pushd protobuf-3.5.1
    ./configure --prefix=/usr && make && sudo make install
    popd
else
    log "Protobuffer already built"
fi

export PKG_CONFIG_PATH=/usr/lib/pkgconfig:${PKG_CONFIG_PATH}

if [ ! -e grpc ]; then 
    log "Cloning grpc"
    git clone -b $RELEASE https://github.com/grpc/grpc
    cd grpc/
else
    log "gRPC already cloned, updating"
    cd grpc/
    git pull
fi

if [ ! -e third_party/protobuf/.git ] ; then
    log "Cloning submodules"
    git submodule update --init
else
    log "Updating submodules"
    git submodule update
fi

log "Building gRPC"
prefix=/usr make

log "Installing gRPC"
sudo sh -c 'prefix=/usr make install'
sudo ldconfig

log "Done"
popd