FROM centos:6

ARG GRPC_VERSION=v1.12.x

RUN yum install -y git \
    wget

RUN wget http://people.centos.org/tru/devtools-2/devtools-2.repo -O /etc/yum.repos.d/devtools-2.repo && yum install -y centos-release-SCL

RUN yum install -y \
    autoconf \
    devtoolset-2-binutils \
    devtoolset-2-gcc \
    devtoolset-2-gcc-c++ \
    gcc \
    make \
    libtool \
    rpm-build \
    ruby193 \
    ruby193-ruby-devel \
    ruby-gems

# Enables DevToolset-2
ENV PATH /opt/rh/devtoolset-2/root/usr/bin:/opt/rh/ruby193/root/usr/bin:$PATH
ENV MANPATH /opt/rh/devtoolset-2/root/usr/share/man
ENV INFOPATH /opt/rh/devtoolset-2/root/usr/share/info
ENV PCP_DIR /opt/rh/devtoolset-2/root
ENV LD_LIBRARY_PATH /opt/rh/devtoolset-2/root/usr/lib64:/opt/rh/devtoolset-2/root/usr/lib:/opt/rh/ruby193/root/usr/lib64:/opt/rh/ruby193/root/usr/lib

# download, make, & install protobuffer
WORKDIR /tmp
RUN wget https://github.com/google/protobuf/releases/download/v3.5.1/protobuf-cpp-3.5.1.tar.gz && tar -x -z -v -f protobuf-cpp-3.5.1.tar.gz
WORKDIR /tmp/protobuf-3.5.1
RUN ./configure --prefix=/usr && \
                make && \
                make install

# download, make, & install gRPC
RUN git clone -b ${GRPC_VERSION} https://github.com/grpc/grpc /tmp/grpc
WORKDIR /tmp/grpc
RUN git submodule update --init && \
    prefix=/usr make && \
    prefix=/usr make install

COPY libdfegrpc.cc libdfegrpc.h libdfegrpc_internal.h Makefile Makefile.protos test_client.c test_synth.c ldconfig.sh /tmp/libdfegrpc/
COPY protos/ /tmp/libdfegrpc/protos/

WORKDIR /tmp/libdfegrpc
RUN make && make install && make test

# this needs to be moved to the beginning next time there's a full rebuild. for now that's too annoying.
ENV GEM_HOME=/tmp
ENV GEM_PATH=/tmp
RUN gem install --no-ri --no-rdoc fpm

RUN /tmp/bin/fpm -s dir -t rpm -n libdfegrpc -v 1.0.0 \
		--after-install ldconfig.sh \
		--after-upgrade ldconfig.sh \
		/usr/lib/libproto* \
		/usr/lib/libgrpc* \
		/usr/include/google/protobuf/* \
   		/usr/include/grpc \
		/usr/bin/protoc \
		/usr/share/grpc \
		/usr/bin/grpc_cpp_plugin \
		/usr/lib/libgpr* \
		/usr/lib/libdfegrpc.so \
		test_client=/usr/bin/dfegrpc_test_client \
		test_synth=/usr/bin/dfegrpc_test_synth

RUN mkdir /out

CMD ["sh", "-c", "cp -v /tmp/libdfegrpc/libdfegrpc*.rpm /out"]