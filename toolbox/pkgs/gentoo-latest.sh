#!/usr/bin/env bash
# Query the linker version
ld -v || true

# Query the (g)libc version
ldd --version || true

echo ""
echo "build requires linking against ncurses AND tinfo, run the following before compilation:"
echo "export LDFLAGS=\"-ltinfo -lncurses\""

emerge-webrsync
emerge \
 bash \
 dev-lang/nasm \
 dev-libs/libaio \
 dev-libs/openssl \
 dev-python/pip \
 dev-python/pyelftools \
 dev-util/cunit \
 dev-util/meson \
 dev-util/pkgconf \
 dev-vcs/git \
 findutils \
 make \
 patch \
 sys-libs/liburing \
 sys-libs/ncurses \
 sys-process/numactl

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

# Install packages via the Python package-manager (pip)
python3 -m pip install --break-system-packages --upgrade pip
python3 -m pip install --break-system-packages \
 pipx

