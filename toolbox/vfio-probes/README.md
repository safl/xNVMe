# vfio sharing probes

Two throwaway programs that answer, by measurement, what a second process can
and cannot do with a `vfio-pci` device another process holds. They exist so
the claims in `docs/design/homi-design.md` can be repeated rather than
trusted.

Neither is part of the build, links against xNVMe, or is useful in production.
Each is a sequence of ioctls that prints what the kernel returned for every
step.

```bash
gcc -Wall -Wextra -O0 -o /tmp/vfio_cdev_probe  vfio_cdev_probe.c
gcc -Wall -Wextra -O0 -o /tmp/vfio_share_probe vfio_share_probe.c
```

Both need a device bound to `vfio-pci`, which is what makes
`/dev/vfio/devices/vfioN` appear:

```bash
DRIVER_OVERRIDE=vfio-pci ./toolbox/xnvme-driver.sh
ls /dev/vfio/devices/
```

## vfio_cdev_probe: can a second process claim the device?

```bash
vfio_cdev_probe <cdev> <attach:0|1> <hold-seconds>
```

Opens the character device, binds it to a fresh iommufd, allocates and
attaches an IOAS, maps 2 MiB, and maps BAR0, reporting each step. Run one
instance holding the device and a second against the same device to see what
the second gets. `attach=0` asks whether binding without attaching is enough
to reach the BAR.

## vfio_share_probe: can a primary delegate to an unprivileged secondary?

```bash
vfio_share_probe primary   <cdev> <socket>
vfio_share_probe secondary <socket> <ioas-id>
```

The primary binds the device, maps a memfd into its IOAS with
`IOMMU_IOAS_MAP_FILE`, and passes the device fd, the iommufd and the memfd
over a unix socket with `SCM_RIGHTS`. It prints its IOAS id before waiting,
which the secondary needs. The secondary receives the descriptors, maps BAR0,
writes to the shared buffer, and registers a buffer of its own.

Run the secondary as a different, unprivileged user, since that is the point:

```bash
vfio_share_probe primary /dev/vfio/devices/vfio0 /tmp/vs.sock &
su -s /bin/sh someuser -c "vfio_share_probe secondary /tmp/vs.sock <ioas-id>"
```

Reading the same value from BAR0 in both processes is what says the secondary
is genuinely looking at the controller rather than at a plausible mapping.
