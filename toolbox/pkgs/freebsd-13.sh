#!/bin/tcsh
# Query the linker version
ld -v || true

# Query the (g)libc version
ldd --version || true

# Install packages via the system package-manager (pkg)
pkg install -qy \
 autoconf \
 automake \
 bash \
 cunit \
 devel/py-pyelftools \
 e2fsprogs-libuuid \
 findutils \
 git \
 gmake \
 libtool \
 meson \
 nasm \
 ncurses \
 openssl \
 patch \
 pkgconf \
 py39-pipx \
 python3 \
 wget

# Upgrade pip
python3 -m ensurepip --upgrade

# Installing meson via pip as the one available 'pkg install' currently that is 0.62.2, breaks with
# a "File name too long", 0.60 seems ok.
python3 -m pip install meson==0.60

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

