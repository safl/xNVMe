.. _sec-toolchain:

Toolchain
=========

The toolchain (compiler, archiver, and linker) used for building **xNVMe**
must support **C11**, **pthreads** and on the system the following tools must
be available:

* Python (>=3.7)
* meson (>=0.58) and matching version of ninja
* make (gmake)
* gcc/mingw/clang

Along with libraries:

* glibc (>= 2.28, for **io_uring/liburing**)
* libaio-dev (>=0.3, For **xNVMe** and **SPDK**)
* libnuma-dev (>=2, For **SPDK**)
* libssl-dev (>=1.1, For **SPDK**)
* liburing (>=2.2, for **xNVMe**)
* uuid-dev (>=2.3, For **SPDK**)

xNVMe makes use of libraries and interfaces when available and will "gracefully
degrade" when a given library is not available. For example, if liburing is not
available on your system and you do not want to install it, then xNVMe will
simply build without io_uring-support.

The preferred toolchain is **gcc** and the following sections describe how to
install it and required libraries on a set of popular Linux Distributions,
FreeBSD, MacOS, and Windows.

In the following sections, the system package-manager is used whenever possible
to install the toolchain and libraries. However, on some Linux distribution
there are not recent enough versions. To circumvent that, then the packages are
installed via the Python package-manager. In some cases even a recent enough
version of Python is not available, to bootstrap it, then Python is built and
installed from source.

.. note:: When installing packages via the Python package-manager (``python3 -m
   pip install``), then packages should be installed system-wide. This is
   ensure that the installed packages behave as though they were installed
   using the system package-manager.

Package-managers: apk, pkg, dnf, yum, pacman, apt, aptitude, apt-get, pkg,
choco, brew.

.. toctree::
   :maxdepth: 2
   :includehidden:

   linux/index.rst
   freebsd/index.rst
   macos/index.rst
   windows/index.rst