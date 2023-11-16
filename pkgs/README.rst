Packages
========

This is the home for packages managed by upstream xNVMe. That is, the stuff
needed to maintain packages for Linux Distributions, FreeBSD (ports), macOS
(brew), and Windows (chocolatey).


Debian packages
===============

https://www.debian.org/doc/manuals/developers-reference/new-maintainer.en.html#new-maintainer

Subscribe to:

* debian-devel@lists.debian.org
* debian-devel-announce@lists.debian.org

Packaging Environment
---------------------

This is of course a Debian machine :) Install some stuff::

  apt-get install \
    autoconf \
    automake \
    autotools-dev \
    autotools-dev \
    debhelper \
    debmake \
    devscripts \
    dh-make \
    fakeroot \
    file \
    git \
    gnu-standards
    gnupg \
    lintian \
    patch \
    patchutils \
    quilt \
    xutils-dev \

dh_make --createorig
