#!/usr/bin/env bash
export LDFLAGS="-ltinfo -lncurses"

# configure xNVMe and build meson subprojects(SPDK)
meson setup builddir \
  --prefix=/usr \
  -Dwith-spdk=/opt/toolbox/third-party/spdk/repository

# build xNVMe
meson compile -C builddir

# install xNVMe
meson install -C builddir

# uninstall xNVMe
# cd builddir && meson --internal uninstall
