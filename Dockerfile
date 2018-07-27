FROM centos:6
LABEL maintainer="daniel.collins@usan.com"

RUN curl -L http://people.centos.org/tru/devtools-2/devtools-2.repo > /etc/yum.repos.d/devtools-2.repo && \
    yum install -y centos-release-SCL

RUN yum install -y \
    autoconf \
    devtoolset-2-binutils \
    devtoolset-2-gcc \
    devtoolset-2-gcc-c++ \
    gcc \
    git \
    make \
    libtool \
    rpm-build \
    ruby193 \
    ruby193-ruby-devel \
    ruby-gems \
    wget

# Enables DevToolset-2 and ruby193
ENV PATH /opt/rh/devtoolset-2/root/usr/bin:/opt/rh/ruby193/root/usr/bin:$PATH
ENV MANPATH /opt/rh/devtoolset-2/root/usr/share/man
ENV INFOPATH /opt/rh/devtoolset-2/root/usr/share/info
ENV PCP_DIR /opt/rh/devtoolset-2/root
ENV LD_LIBRARY_PATH /opt/rh/devtoolset-2/root/usr/lib64:/opt/rh/devtoolset-2/root/usr/lib:/opt/rh/ruby193/root/usr/lib64:/opt/rh/ruby193/root/usr/lib

ENV GEM_HOME=/usr
ENV GEM_PATH=/usr

RUN gem install --no-ri --no-rdoc fpm
