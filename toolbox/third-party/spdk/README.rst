Consumption of The SPDK NVMe driver
===================================

This directory holds the ``meson.build`` file that describes how xNVMe
consumes, links with, the SPDK NVMe driver. Specifically, then a meson option
``-Dwith-spdk=PATH`` is provided, which should point to the location of a SPDK
git repository, in which SPDK has been built.

By default, then Meson will look at ``toolbox/third-party/spdk/repository``.

For convenience, then have a look at the xNVMe getting-started-guide, to see
how to build SPDK for consumption by xNVMe.

History
-------

The xNVMe project has consumed the SPDK NVMe driver in the following ways:

on-system, this was the first approach, however, it was replaced in order to
apply non-upstream patches and to manage the build on behalf of the user. Also,
today, there are challenges with libraries such as ISA-L not being installed.

git-submodule, this was the first approach to replace the on-system consumption
of SPDK, with patches applied via xNVMe Makefile, this was done when xNVMe used
CMake. This worked fairly well, however, we repeatedly had to explain how to
use git submodules, and had issues when users upgraded xNVMe but did not bump
the git-submodule.

meson subproject, with the switch of xNVMe to Meson, then the git submodule was
replaced by a Meson subproject. It has allowed us to apply non-upstream
patches, manage the build of SPDK on behalf of the user. All-in-all, a fairly
good user experience. However, it does not do so in "proper" manner, since the
SPDK build system is not Meson, then it is "hacked" in, which leads to
grievances.

In addition to the different means of making the SPDK/NVMe source available,
then different approaches has also been taken as to how xNVMe links / embeds
the driver. In the time of xNVMe/CMake and the early stages of Meson, then
scripts would "bundle" / "embed" the SPDK static libaries into the xNVMe
library. This gave a simple linker-target for xNVMe users. However, the
approach is frowned upon by Linux distribution packagers.

Thus, this was replaced by linking with the static SPDK libraries, and adding
these to the xNVMe pkg-config file. By doing so, then the linking is managed by
pkg-config and no unconventional bundling-tricks are applied.
