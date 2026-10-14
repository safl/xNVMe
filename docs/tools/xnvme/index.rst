.. _sec-tools-xnvme:

xnvme
#####

``xnvme`` is the entry point to the **xNVMe** command line. Run it without
arguments for the commands to start with and the groups the rest are in:

.. literalinclude:: xnvme_overview.cmd
   :language: bash

.. literalinclude:: xnvme_overview.out
   :language: bash

Run a group to list its commands, and append ``--help`` to any command for its
arguments. Commands such as ``perf``, ``homi`` and ``qublk``
are separate programs, which ``xnvme`` looks up on its exec path,
the install directory unless ``XNVME_EXEC_PATH`` names others;
``xnvme --exec-path`` prints it.

``xnvme env`` says, as YAML, whether the host is ready for the userspace
drivers: the IOMMU groups, whether each kernel module is available, built in
and loaded, the hugepages of each size and NUMA node, where hugetlbfs is
mounted, memlock, the device nodes, and the NVMe controllers by driver, each
with a ``fix`` where something is missing. It then prints the library: the
version, the build configuration and the backends. ``xnvme env system`` and
``xnvme env library`` print one of the two. It changes nothing.
The fixes name the ``hugepages`` and ``iommu`` tools, installed with
``pipx install hugepages iommu``.

``xnvme list`` shows the NVMe controller functions on the PCI bus, found with
``xnvme_scan()``, as YAML: the driver each is on and its IOMMU group, read from
sysfs without opening anything. ``xnvme driver-attach <bdf>`` moves a
controller to a userspace driver, and ``xnvme driver-detach <bdf>`` returns
it to the kernel ``nvme`` driver; both print the outcome as YAML, each
controller with the driver it is on, whether that changed, and why not. On Linux that is ``vfio-pci``, or ``uio_pci_generic`` without an IOMMU; on
FreeBSD, ``nic_uio``. Only the named controller is rebound. One whose
namespaces are mounted, held by device-mapper, md or LVM, used as swap or open
in a process is refused, as is one sharing its IOMMU group with a device on
another driver; one whose namespace carries a filesystem or partition table is
refused unless ``--force``. The binding is done by the platform, through
``xnvme_driver_attach()`` and ``xnvme_driver_detach()`` in libxnvme, and is not
supported on macOS and Windows. Nothing is recorded on the side: what is
shown is what sysfs says, and whether a controller is in use is checked when
it is to move, which ``--dry-run`` does without moving it.

``xnvme cmd-admin``, ``cmd-io`` and ``cmd-iov`` send a raw command, and take
the options of ``nvme admin-passthru`` and ``nvme io-passthru``, the same long
names and the same letters, so a command line moves between the two by
changing the program and the subcommand. The device is any **xNVMe** URI, so
the same command reaches a controller on a userspace driver, which
``nvme-cli`` cannot:

.. code-block:: bash

   nvme  admin-passthru /dev/nvme0    --opcode=0x06 --cdw10=1 --data-len=4096 -r
   xnvme cmd-admin      /dev/nvme0n1  --opcode=0x06 --cdw10=1 --data-len=4096 -r
   xnvme cmd-admin      0000:03:00.0  --opcode=0x06 --cdw10=1 --data-len=4096 -r --be upcie

The options choosing the path, ``--be``, ``--admin``, ``--sync``, ``--async``,
``--dev-nsid`` and ``--homi-id``, are long options only. ``cmd-iov`` sends the
data in ``--vec-cnt`` equal segments. ``--timeout`` and ``--output-format json``
are refused rather than ignored.

The admin commands with their output decoded, Identify, the log pages,
features, format, sanitize, FDP and the resets, are ``xnvme test adm``, which
the tests run through every backend. For a controller on the kernel driver,
``nvme-cli`` is the tool for these; ``xnvme cmd-admin`` sends any of them raw,
on any backend.

The installed test programs are listed and run the same way:

.. literalinclude:: xnvme_test.cmd
   :language: bash

.. literalinclude:: xnvme_test.out
   :language: bash

Every built-in command at once:

.. literalinclude:: xnvme_usage.out
   :language: bash

Subcommands
===========

.. toctree::

   info/index
   enum/index
