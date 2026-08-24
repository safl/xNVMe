(sec-backends-upcie-host)=

# uPCIe (host memory)

The **upcie** backend allocates I/O buffers from a DMA heap backed by
**hugepages** in host memory. Physical addresses are resolved via
`/proc/self/pagemap` and cached at allocation time, so no address translation
is needed on the I/O path.

(sec-backends-upcie-host-config)=

## System Configuration

### Hugepages

`xnvme-driver` configures hugepages as part of device setup. To allocate them
manually:

```bash
echo 128 | tee -a /proc/sys/vm/nr_hugepages
ulimit -l unlimited
```

Each hugepage is 2 MiB by default. The number required depends on the total
DMA buffer space needed by the application.

(sec-backends-upcie-host-limitations)=

## Limitations

- **No `buf_realloc`.** Buffer reallocation is not implemented and returns
  `ENOSYS`.
- **Memory mapping is one-way.** `mem_map` registers caller-allocated memory,
  but only when the device is opened through vfio-cdev; without an address
  space to map into it returns `ENOTSUP`. `mem_unmap` returns `ENOSYS`, since
  iommufd unmaps by IOVA and length, neither of which a caller handing back an
  address has. A range stays mapped for the lifetime of the process.
- **Controller reset** and **subsystem reset** are not supported. These
  operations disable the controller and re-enable it, but do not re-program the
  admin submission/completion queues. Without full re-initialization the
  controller times out waiting for the first admin command after re-enable.
  Both pseudo commands return `ENOSYS`.
