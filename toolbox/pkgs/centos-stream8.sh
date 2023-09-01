#!/usr/bin/env bash
# Query the linker version
ld -v || true

# Query the (g)libc version
ldd --version || true

# This repos has CUnit-devel
dnf install -y 'dnf-command(config-manager)' || true
dnf config-manager --set-enabled powertools || true

# Install packages via the system package-manager (dnf)
dnf install -y \
 CUnit-devel \
 autoconf \
 bash \
 diffutils \
 findutils \
 gcc \
 gcc-c++ \
 git \
 libaio-devel \
 libffi-devel \
 libtool \
 libuuid-devel \
 make \
 nasm \
 ncurses \
 numactl-devel \
 openssl-devel \
 patch \
 pkgconfig \
 procps \
 unzip \
 wget \
 zlib-devel

# Clone, build and install liburing v2.2
#
# Assumptions:
#
# - Dependencies for building liburing are met (system packages etc.)
# - Commands are executed with sufficient privileges (sudo/root)
#
git clone https://github.com/axboe/liburing.git toolbox/third-party/liburing/repository

pushd toolbox/third-party/liburing/repository
git checkout liburing-2.2
./configure --libdir=/usr/lib64 --libdevdir=/usr/lib64
make
make install
popd

#
# Download, configure, and install Python v3.8.18
#
# Assumptions:
#
# - Dependencies for building Python3 are met (system packages etc.)
# - Commands are executed with sufficient privileges (sudo/root)
#
# Download and extract
pushd /tmp
wget https://www.python.org/ftp/python/3.8.18/Python-3.8.18.tgz
tar xzf Python-3.8.18.tgz
popd
mv /tmp/Python-3.8.18 toolbox/third-party/python3/src

# Configure and build
pushd toolbox/third-party/python3/src
./configure --enable-optimizations --enable-shared
make altinstall -j $(nproc)
popd

# Setup handling of python3
ln -s /usr/local/bin/python3.8 /usr/local/bin/python3
hash -d python3 || true

# Avoid error with "libpython*so.1.0: cannot open shared object file: No such file or directory"
ldconfig /usr/local/lib

# Install packages via the Python package-manager (pip)
python3 -m pip install --upgrade pip
python3 -m pip install \
 meson \
 ninja \
 pipx \
 pyelftools

#
# Clone, build and install libvfn
#
# Assumptions:
#
# - Dependencies for building libvfn are met (system packages etc.)
# - Commands are executed with sufficient privileges (sudo/root)
#
git clone https://github.com/OpenMPDK/libvfn.git toolbox/third-party/libvfn/repository

pushd toolbox/third-party/libvfn/repository
git checkout v2.0.2
meson setup builddir -Dlibnvme="disabled" -Ddocs="disabled" --prefix=/usr
meson compile -C builddir
meson install -C builddir
popd

#
# Clone, configure, and build SPDK
#
# Assumptions:
#
# - Dependencies for building SPDK are met (system packages etc.)
#
git clone https://github.com/spdk/spdk.git toolbox/third-party/spdk/repository

# Checkout v22.09 and update submodules
pushd toolbox/third-party/spdk/repository 
git checkout v22.09
git submodule update --init --recursive

# Apply patches to SPDK
find ../patches -type f -name 'spdk-0*.patch' -print0 | sort -z | xargs -t -0 -n 1 patch -p1 --forward -i || true

# Configure and build SPDK
./configure \
  --disable-apps \
  --disable-examples \
  --disable-tests \
  --disable-unit-tests \
  --without-crypto \
  --without-fuse \
  --without-idxd \
  --without-iscsi-initiator \
  --without-nvme-cuse \
  --without-ocf \
  --without-rbd \
  --without-reduce \
  --without-shared \
  --without-uring \
  --without-usdt \
  --without-vhost \
  --without-virtio \
  --without-vtune \
  --without-xnvme
make -j

popd

