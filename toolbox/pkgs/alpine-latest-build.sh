#!/usr/bin/env bash
# configure xNVMe with SPDK and libvfn disabled
meson setup builddir

# build xNVMe
meson compile -C builddir

# install xNVMe
meson install -C builddir

# uninstall xNVMe
# cd builddir && meson --internal uninstall
