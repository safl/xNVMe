---
orphan: true
---

# HOMI: sharing a controller between processes

## What this is

HOMI holds an NVMe controller so that other processes can use it without each
of them initialising the device. Today that works only where the device is
bound to `uio_pci_generic`. This document records why, what the kernel offers
now, and the design that follows for `vfio-pci`.

It is ordered so that the evidence comes first: six measurements, then the
design they and the usage model determine, where that design belongs and what
has to be decided before it is written, then the ideas that were rejected and
why, then what has not been established and how it would be tested. The
measurements were run on 2026-08-24 against a Samsung NVMe controller on Linux
7.0.0-28-generic with an IOMMU enabled, and their commands and output are
quoted as they ran. Anything not measured is under "What is unverified" rather
than stated as fact.

## Where we are

`_rte_init()` refuses multi-process mode outside the UIO attachment mode:

```c
if (opts->shm_id && mode != XNVME_BE_UPCIE_MODE_UIO_LUT) {
        XNVME_DEBUG("FAILED: shm_id requires UIO_LUT (uio_pci_generic); mode(%d)", mode);
        return -ENOTSUP;
}
```

The reason is what the device consumes. Under `uio_pci_generic` with the IOMMU
out of the way, it consumes physical addresses, and a physical address means
the same thing in every process. Sharing then needs only two values, which the
runtime segment already publishes:

```c
char     hugepage_path[256]; ///< Path to primary's hugepage file
uint64_t hugepage_base;      ///< Primary's hugepage virtual base for secondary pointer fixup
```

A secondary opens that file by the path published for it, which is
`/proc/<pid>/fd/<fd>` for the default memfd backend, maps the same pages,
and fixes up the primary's embedded pointers by a constant offset. The
addresses in the table are already correct for it.

Under `vfio-pci` the device consumes IOVAs, which are meaningful only relative
to an address space owned by a file descriptor. Nothing about that is
published, and a POSIX shared-memory segment cannot carry a descriptor.

Worth separating from the above: `--be upcie` already drives a controller
through `vfio-pci` perfectly well in single-process mode, and HOMI already
runs under `vfio-pci` with `--be spdk`. What has never worked is two processes
sharing one controller under vfio.

## Measurement 1: a second process cannot claim the device

The obvious question is whether a secondary can simply open the character
device for itself. It can open it, and that is as far as it gets. With a
primary holding the device and working normally:

```
--- SECOND, bind only:
  open(cdev)                   ok
  BIND_IOMMUFD                 Invalid argument
  GET_REGION_INFO(BAR0)        Invalid argument
--- SECOND, with attach:
  BIND_IOMMUFD                 Invalid argument
  ATTACH_IOMMUFD_PT            Invalid argument
```

`open()` succeeding is what makes the idea look plausible.
`VFIO_DEVICE_BIND_IOMMUFD` then fails with `EINVAL`, and since `linux/vfio.h`
states that "user is restricted from accessing the device before the binding
operation is completed", everything after it fails too: no region info, so no
BAR, so no doorbell. Offering a different iommufd does not help, and neither
does declining to attach.

This matches the kernel documentation, which says devices cannot be bound to
multiple `iommufd_ctx` and that a violation fails at exactly this ioctl.

Note what does succeed in that second process: opening `/dev/iommu`,
allocating an IOAS, and mapping memory into it. That is the trap. A secondary
can build a valid-looking address space and map buffers into it, and the
result is meaningless, because the IOAS is attached to no device.

## Measurement 2: a primary can delegate to an unprivileged secondary

The second question is whether the primary can delegate. It can, and further
than expected. A root primary bound the device, mapped a memfd into its IOAS
with `IOMMU_IOAS_MAP_FILE`, and passed three descriptors over a unix socket
using `SCM_RIGHTS`: the device fd, the iommufd, and the memfd. The receiving
process ran as an ordinary user, with no root and no `CAP_SYS_ADMIN`:

```
[secondary uid=1000]
  connect                        ok
  recv fds                       ok
  device fd=4 iommufd=5 memfd=6
  GET_REGION_INFO(BAR0)          ok
  mmap(BAR0)                     ok
  NVMe CAP low dword             0x28033fff   (secondary)
  mmap(shared memfd)             ok
  shared DMA buffer              wrote 0x00c0ffee
  IOAS_MAP own buffer            ok
    -> iova                      0x400000
```

The primary read the same `0x28033fff` from its own mapping, so the secondary
is genuinely looking at the controller's registers.

Three things follow. The secondary reaches the BAR, and therefore the
doorbells. It shares DMA memory through a memfd neither process had to name in
the filesystem. And it registered memory of its own choosing into the
primary's IOAS through the passed iommufd, so it is not restricted to buffers
the primary arranged in advance.

## Measurement 3: MMIO cannot be delegated as a descriptor

The design wanted to hand a secondary the doorbells without the device, which
`VFIO_DEVICE_FEATURE_DMA_BUF` appears to offer: it exports a slice of a BAR,
named by a region index and `offset`/`length` ranges. Two things have to hold
for that to be usable, and neither does.

The descriptor carries no CPU mapping. uPCIe's `test_dmamem_vfio_bar` exports
a BAR slice and reports whether `mmap` on the result succeeds:

```
BAR dma-buf: fd=5 region=0 length=0x1000
OK: base_iova=0x1000 size=4096  cpu_va=unavailable
BAR dma-buf: fd=5 region=0 length=0x2000
OK: base_iova=0x2000 size=8192  cpu_va=unavailable
BAR dma-buf: fd=5 region=0 length=0x4000
OK: base_iova=0x4000 size=16384 cpu_va=unavailable
```

The export works at every size and each slice maps into an IOAS, and there is
no host address. That matters because the shipping GPU-initiated path reaches
a doorbell by registering a host address as I/O memory:

```c
_qpair.sqdb = bar0 + 0x1000 + ((2 * qid) << (2 + dstrd));
cuMemHostRegister(_qpair.sqdb, sizeof(uint32_t), CU_MEMHOSTREGISTER_IOMEMORY);
```

The other route would be for the GPU runtime to import the descriptor itself.
uPCIe's `upcie_vfio_bar_import_probe_{cuda,hip}` asks exactly that, reading
`CAP` rather than writing a doorbell so the host can read the same register
for comparison, and carrying a control: a runtime that will not import a
descriptor it exported itself is saying nothing about vfio.

```
warp, NVIDIA RTX A6000, driver 580.173.02 (open modules), CUDA 13.3
  host read                  0x28033fff 0x08000030
  EXPORT_DMA_BUF             ok
  self-import (control)      own dma-buf import=CUDA_ERROR_NOT_SUPPORTED(801)
  import into GPU runtime    FAILED (import(DMABUF_FD)=CUDA_ERROR_NOT_SUPPORTED(801),
                                     import(OPAQUE_FD)=CUDA_ERROR_UNKNOWN(999))

wave, AMD Radeon RX 7800 XT, ROCm
  host read                  0x28033fff 0x08002030
  EXPORT_DMA_BUF             ok
  import into GPU runtime    FAILED (hipImportExternalMemory=hipErrorOutOfMemory(2))
```

The control fails identically to the real case, so CUDA does not import
dma-bufs on this stack for any exporter, and the refusal says nothing about
vfio or about MMIO. HIP has no dma-buf handle type at all, its
`hipExternalMemoryHandleType` stopping at `NvSciBuf`. The export direction
works on both hosts and the host reads `CAP` correctly, so what is missing is
import support, not a sound descriptor.

A process that must ring a doorbell therefore has to hold the device fd and
map the BAR itself.

## Measurement 4: the delegated path works, and privilege bounds it

Measurement 2 showed a secondary receiving a device fd and reading a register
through its own mapping of BAR0. What that left untested is the step
GPU-initiated submission depends on: registering a window of that received
mapping as I/O memory, and reaching it from an SM.
`upcie_vfio_share_gpu_probe_cuda` is a primary holding the device and a
secondary holding only what it is handed, meeting over a named socket, and it
reads `CAP` rather than writing a doorbell so nothing is disturbed. Two
processes rather than a fork with a `setuid()` in it, because a process that
dropped privilege is not in the state being asked about: it keeps the
supplementary groups it started with unless they are cleared, and a uid change
clears the dumpable flag.

Started as root, the secondary gets all the way:

```
[secondary] running as                   uid=0 euid=0 gid=0
[secondary] recv device fd               ok
[secondary] host read                    0x28033fff 0x08000030
[secondary] cuMemHostRegister(IOMEMORY)  ok
[secondary] kernel read                  0x28033fff 0x08000030
```

The SM reads what the primary reads, so a passed descriptor is enough to drive
a controller from a GPU kernel. Started as uid 1000, under `setpriv --reuid
--regid --clear-groups` so that it never held privilege, the same secondary
receives the descriptor, maps BAR0 and reads the same register, then stops:

```
[secondary] running as                   uid=1000 euid=1000 gid=1000
[secondary] cuMemHostRegister(IOMEMORY)  CUDA_ERROR_NOT_PERMITTED
```

The probe's standalone mode is the control for that. It opens the device
itself, with no delegation anywhere, and with the cdev and `/dev/iommu`
chowned to the user it gets as far and no further:

```
[standalone] running as                   uid=1000 euid=1000 gid=1000
[standalone] bind and attach              ok
[standalone] host read                    0x28033fff 0x08000030
[standalone] cuMemHostRegister(IOMEMORY)  CUDA_ERROR_NOT_PERMITTED
```

The same binary as root reads `CAP` from a kernel. So the descriptor delegates
the device; `CU_MEMHOSTREGISTER_IOMEMORY` delegates nothing and wants
privilege of the calling process, whether or not anything was passed to it.
Which capability short of root suffices was not tested.

On AMD the question stops earlier. `hipHostRegisterIoMemory` is documented in
`hip_runtime_api.h` as "Not supported", and the registration returns
`hipErrorInvalidValue` as root, so there is no privilege boundary to find
because the route is absent. That matches uPCIe having no HIP counterpart to
`nvme_qpair_cuda.h`. GPU-initiated submission is an NVIDIA-only capability
today, and nothing in this design changes that either way.

The run also settles something in passing: an unprivileged process can bind,
attach and map BAR0 of a vfio device when udev gives it the nodes. It did no
DMA, so this is about reaching the device rather than about driving it. That
is the model libvirt uses, and it is why `vfio-pci` is the only attachment
mode where the question arises at all. Under `uio_pci_generic`,
`pci_bar_map()` opens `/sys/bus/pci/devices/<bdf>/resource0`, which is
root-only, and the physical addresses that path needs come from pagemap, which
wants `CAP_SYS_ADMIN`.

## Measurement 5: GPU memory cannot enter an IOAS yet

Under `uio_pci_generic` a controller consumes physical addresses and reaches a
GPU allocation through its dma-buf scatter list. Under `vfio-pci` it consumes
IOVAs, so the allocation has to be mapped with `IOMMU_IOAS_MAP_FILE`.
uPCIe's `upcie_vram_ioas_probe_{cuda,hip}` asks whether it can be, mapping a
`memfd` first as a control, since a `MAP_FILE` that refused everything would
say nothing about GPU memory in particular. On Linux 7.0.0-28-generic:

```
warp, NVIDIA RTX A6000, CUDA 13.3
  MAP_FILE(memfd) [control]          ok, iova=0x200000
  MAP_FILE(GPU dma-buf)              Operation not supported

wave, AMD Radeon RX 7800 XT, ROCm
  MAP_FILE(memfd) [control]          ok, iova=0x200000
  MAP_FILE(GPU dma-buf)              Operation not supported
```

Both vendors, same answer. This is not about delegation or about sharing: a
single process, on its own controller, cannot have it DMA into VRAM under an
IOMMU. It is why `xnvme_be_upcie_cuda_ctrlr_init` refuses `vfio-pci` before it
starts, and why uPCIe's `iommufd.h` says as much in the header where the call
lives.

The kernel is expected to grow this. So the position taken here is to build
for the kernel this is heading toward rather than the one that is installed:
the vfio path is written as though `MAP_FILE` accepted a GPU dma-buf, the gate
is one call in one place, and when it opens nothing else has to change. Until
then GPU consumers stay on `uio_pci_generic`, where the same code already
works, and experiments run on kernels carrying the support out of tree.
Nothing about that makes the work speculative: CPU-submitted I/O into host
memory under an IOMMU exercises every other step of the same path, on hardware
that answers today.

## Measurement 6: what the delegation costs, and what outlives it

Two things the design leaned on were assumptions.
`upcie_vfio_delegate_probe` puts both to the kernel, with no GPU involved.

The first is accounting. The design withheld the iommufd so that pinned pages
would be charged to homi rather than to an unprivileged secondary that would
otherwise need its `RLIMIT_MEMLOCK` raised. With that limit set to 64 KiB, on
the secondary and separately on the primary, every 2 MiB mapping still
succeeded: the secondary's `MAP_FILE` of a memfd, its `IOMMU_IOAS_MAP` of
anonymous memory, and the primary's `MAP_FILE` on its behalf. The limit bounds
none of it, so there is no ulimit to spare anyone.

The second is lifetime. After the primary exits, closing its device fd and its
iommufd:

```
[secondary] primary is gone                ok
[secondary] host read after                0x28033fff
[secondary] map after, via passed fd       ok, iova=0x800000
```

The secondary still reads `CAP` through its own mapping and still maps new
memory through the iommufd it was handed. The descriptors keep both the device
and the address space alive, so a secondary is not obliged to die with the
process that handed them over.

## The design

The measurements settle the mechanism, and the way HOMI is used settles the
rest. homi runs first and holds the controller. A program starts later,
attaches, does I/O, and launches GPU kernels that submit I/O of their own.
That program is not a child of homi and cannot be made one, so everything
below follows from delegating to an unrelated process that arrives at an
arbitrary time.

That last part, the GPU kernels, is the half measurement 5 says the installed
kernel cannot serve yet. It stays in the usage model rather than being written
out of it, because the code is being built for the kernel this is heading
toward; what changes when the capability lands is a `MAP_FILE` that stops
returning `ENOTSUP`, and nothing else here.

**A secondary attaches over a unix socket, and only over a unix socket.**
`SCM_RIGHTS` is the sole mechanism Linux offers for moving a descriptor
between unrelated processes. The alternatives are enumerated under rejected
ideas below; none of them survives the usage model. The socket carries attach,
and the control-plane requests below, and is never on the I/O path.

**The secondary receives the device fd.** This is not the decision the design
started with. BAR0 can be exported as a dma-buf with
`VFIO_DEVICE_FEATURE_DMA_BUF`, which takes a region index and an array of
`offset`/`length` ranges, and a doorbell slice alone would have given a
secondary the MMIO its accelerator needs and no route to `CC`, `CSTS`, or a
controller reset. Measurement 3 shows that route
does not exist. So the device fd crosses, the secondary maps BAR0 itself, and
the authority to reset the controller comes with it.

**A heap is passed as a descriptor, not as a path.** The host heap is already
a `memfd`, since `hostmem_hugepage` defaults to `MFD_HUGETLB` and reaches for
`hugetlbfs` only in its other backend. What is published today is
`/proc/<pid>/fd/<fd>`, which the secondary re-opens. Passing the descriptor
itself removes that indirection and with it a constraint nobody chose:
re-opening another process's `/proc/PID/fd/N` requires ptrace-mode access, so
secondaries have to share the primary's uid, which is the opposite of what
this design is for. Device heaps are dma-bufs, so once the path is gone both
kinds of heap are the same shape rather than one being a case apart.

**The iommufd crosses too, and secondaries register their own memory.** This
reverses an earlier decision, and measurement 6 is why. The reason given for
withholding it was that registering through homi would charge the pinned pages
to the privileged process; the limit that argument rests on bounds nothing
here. Withholding it also protects nothing, since a secondary holding the
device fd already has every authority over the controller. And it costs a
great deal: `IOMMU_IOAS_MAP` takes a `user_va` in the caller's address space,
so homi cannot map a secondary's anonymous memory at all, and what a secondary
most wants registered is exactly that, the buffers `xnvme_mem_map` exists for.
So the secondary registers for itself, as it did in measurement 2, and the
socket keeps the handshake and the queue grant.

**The socket replaces the named objects.** Multi-process mode currently names
five things in the filesystem so that two processes can find each other: the
runtime segment, one segment per controller, two election locks, and the
hugepage backing file. One socket address replaces all five. Binding the
address is the election, which is the test the `flock()` on the role file
performs today. An
abstract-namespace address vanishes with the last holder, so there is no
debris to detect and no magic or version stamping needed to make a reused name
safe; peers are authenticated with `SO_PEERCRED` instead of filesystem
permissions.

**The controller record lives at a heap offset, always.** It carries a
refcount, queue-allocation state and the embedded `struct nvme_controller`,
and it is live state rather than handshake data, so it stays in memory both
processes map. What it stops needing is a name and a segment of its own: the
handshake says which offset it sits at, and a runtime that shares with nobody
puts it in the same place. The per-runtime segment disappears entirely, since
every field in it exists only to substitute for a channel.

**A closed connection is how a secondary's death is observed.** A program that
is killed mid-I/O is the normal case. Today the primary infers it from a
refcount that a dead process never decremented, which is why stale-segment
detection exists at all. A connection closes when the process dies, whatever
it was doing, and that is when its queues are reaped. This is worth more than
the descriptor passing the socket was introduced for.

**On this path the primary is always homi.** An application does not become
primary by setting `shm_id`, so no library user grows an `accept()` loop. The
rule is that multi-process under an IOMMU means running homi.

**Secondaries are inside one trust domain.** It follows from the decision
above rather than from a preference. A secondary that maps BAR0 can write `CC`
and reset the controller, and holding a dup of the device fd it can also
detach the device from the IOAS or reset it by ioctl. Nothing in the
delegation can prevent any of that. Where that is unacceptable, the answer is
not a variation on this design; it is the kernel driver, which arbitrates
because it owns the device.

**Unprivileged secondaries submit from the CPU, not from the GPU, and on AMD
nobody does.** Measurement 4 draws this line and it is not ours to move.
Ringing a doorbell from the host is an ordinary store into a mapping the
secondary already has, so an unprivileged process can drive a delegated
controller. Submitting from a GPU kernel needs `CU_MEMHOSTREGISTER_IOMEMORY`,
which is refused to it, and the standalone control shows the refusal is not
about delegation: a process that opens the device itself is refused
identically. GPU-initiated I/O is therefore privileged with or without homi,
which narrows what delegation buys for those consumers to sharing a controller
rather than avoiding root.

**The vfio path is written for the kernel it is heading toward.** Measurement
5 says a controller under an IOMMU cannot DMA into VRAM on the kernel
installed today. Rather than design around that, the code assumes
`IOMMU_IOAS_MAP_FILE` takes a GPU dma-buf and fails where the kernel refuses,
once, with an error that names the call. Two things follow for the
implementation. The CUDA and HIP backends stop rejecting `vfio-pci` up front,
since an early guess about what the kernel supports is worse than a precise
failure at the point of use. And the tests for the GPU path under vfio skip
when the capability is absent rather than assert that it is, so that a kernel
carrying the support turns them green instead of turning them red.

**Multi-process stops being a mode.** Every process is a primary. A secondary
is not a different kind of thing, it is a primary whose descriptors and record
arrived over a socket rather than being made locally, and a lone process is a
primary with no listener. `shm_id` stops switching between two paths and the
`if (shm)` branching goes with it. The point of this is not tidiness: today
the shared structures are exercised only when someone asks for multi-process,
whereas under one construction path every ordinary single-process run
exercises them.

What does not unify is what the table holds. Under `uio_pci_generic` it holds
physical addresses, meaningful in any process; under `vfio-pci` it holds
IOVAs, meaningful only in the address space that owns them. The registry
treats the table as opaque, so that difference stays where it is rather than
leaking outward.

## Where this belongs

The mechanism belongs in uPCIe, which is where every object it moves already
lives. `include/upcie/iommufd.h` carries open, close, `ioas_alloc`, `map` and
`map_file`; `dmabuf.h` and `dmamem_dmabuf.h` exist; `pci.h` owns the BARs; and
`nvme_controller` is a uPCIe struct. Reconstructing a controller from a
received descriptor set belongs next to the struct being reconstructed, and
the capability probe that decides whether GPU memory can enter an IOAS belongs
next to `iommufd.h`, where the call it asks about lives.

The layering is wrong today in the other direction.
`lib/upcie/xnvme_be_upcie_mproc.c` fixes up uPCIe's pointers across processes
by a published base offset, which is xNVMe reaching into a lower layer. This
work is the occasion to correct that: the file should get smaller, keeping the
backend wiring and losing the struct surgery.

The protocol belongs with the mechanism, in uPCIe. A wire format only one
program speaks is not a protocol, and the precedent is libvfio-user defining
one that QEMU consumes. What cannot go there is the daemon: uPCIe is
header-only static inlines, which suits encode, decode, export and attach, and
does not suit an accept loop with policy and configuration. So homi stays an
xNVMe tool, where its tests, documentation and systemd unit already are.
Moving it out later becomes a packaging decision rather than a redesign,
precisely because the protocol would not be xNVMe's.

## What must be decided before implementation

**Whether admin submission becomes a request to homi.** Cross-process admin is
serialised today by a `PTHREAD_MUTEX_ROBUST` process-shared mutex with
`EOWNERDEAD` recovery. If homi owns the admin queue and secondaries ask, that
mutex and its recovery path disappear, and admin is off the fast path so it
costs nothing. This decides how much shared state is left, so it comes first.

**What a secondary does when homi exits.** Measurement 6 says the kernel
permits it to carry on: the device stays bound, the BAR stays mapped, and the
IOAS still takes new mappings, because the descriptors hold them. So the
decision is not what the kernel allows but what homi promises. Carrying on
means a runtime with no arbiter, where queue grants and the shared record have
no owner; dying with homi means a restart is fatal to every consumer. The
third option is handover, which `IOMMU_IOAS_CHANGE_PROCESS` exists for.

**How queues are granted.** A shared bitmap suits peers. With an arbiter the
grant belongs in the handshake, so homi can bound what one secondary takes and
reap it on disconnect.

**Whether the UIO path moves onto the socket too.** One construction path
argues that it should. It is a behavioural break for existing `--shm_id` users
on `uio_pci_generic` and needs a migration story rather than a footnote.

**What replaces `shm_id` on the API.** A socket address, keyed to what: one
homi per controller, or one for several. This is public surface, so it reaches
the ctypes bindings, apigen and the `-Dbe_upcie=false` stub build, none of
which an ordinary Linux build exercises.

**Who may attach.** An abstract-namespace address has no filesystem
permissions, so authorisation is `SO_PEERCRED` against a policy homi carries.
Unprivileged secondaries are the entire point, so which users may attach is a
configuration item rather than a detail.

**Where a secondary's DMA memory comes from.** Today every process allocates
its own hugepage heap and `_rte_init()` runs the full mode init in
secondaries, which works only because physical addressing under UIO makes any
process's hugepage device-visible. Under vfio a secondary without the iommufd
cannot create DMA-able memory at all. Passing the primary's heap descriptor is
not by itself an answer, because `dmamem_heap` is a process-local allocator
with process-local metadata: either it becomes shared state with a
cross-process allocator, or allocation becomes a request to homi, or each
secondary brings its own `memfd` for homi to map. Nothing works until this is
chosen.

**How a homi-registered buffer is translated on the submit path.** Registering
returns an IOVA; the secondary's submit path needs to turn a virtual address
inside that buffer into it. On the vfio path the host heap translates
arithmetically from one contiguous base, and the registry that would carry
arbitrary ranges is VA-indexed, per-process, and populated with physical
addresses. Neither answers this, and it is the same structure xnvme/xnvme#745
is moving.

**What a secondary can register, now that it registers for itself.**
Measurement 5 says GPU memory cannot enter an IOAS on this kernel, so under
vfio the registerable set is host memory until that changes. What
`xnvme_mem_map` should do on the vfio path in the meantime, refuse or fall
back, is not decided.

**Teardown ordering when a connection drops.** Reaping on disconnect needs an
order: delete the submission and completion queues, drain what is outstanding,
unmap the IOAS ranges, release the heap. Unmapping before the queues are gone
invites the controller to DMA through addresses that no longer resolve.

**How a version mismatch is detected.** Dropping the segment drops its magic
and version stamping, and the record still embeds `struct nvme_controller`,
whose layout moves with uPCIe. The handshake has to carry something that says
so, and what that is has not been decided.

**homi's concurrency model.** Whether the accept loop is a thread, whether one
secondary's registration or admin request blocks another, what a partially
attached secondary leaves behind when it fails halfway, and what a secondary
does while homi is still binding, which today is three separate one-second
spin loops.

**The kernel floor, and what happens below it.** Every measurement here used
the vfio cdev with iommufd on Linux 7.0. `IOMMU_IOAS_MAP_FILE` arrived in 6.14
and the cdev interface earlier; what the vfio path does on a kernel without
them, and what becomes of `XNVME_BE_UPCIE_MODE_VFIO_TYPE1`, is unstated.

**Sequencing.** Making every heap a descriptor and every process a primary
touches every runtime init, including UIO and both GPU heaps, and it lands on
uPCIe structures that xnvme/xnvme#745 is still moving. It goes after that, as
a uPCIe change, before any socket code is written.

## What the kernel offers that DPDK's era did not

The primitive is the one DPDK uses, and the reason is worth stating: in vfio
the descriptor is the capability. That is the design intent, and it is how
libvirt runs QEMU unprivileged. The exclusivity in measurement 1 is not an
oversight to work around; it is what makes delegation safe to offer.

What has changed is the shape of the API. `IOMMU_IOAS_MAP_FILE` maps a
descriptor into an address space rather than a virtual address range, which is
what lets DMA memory be shared without a path. `VFIO_DEVICE_FEATURE_DMA_BUF`
exports a slice of a region as a dma-buf, which looked like a way to delegate
a doorbell without the device until measurement 3. An implementation today is
smaller than DPDK's:
one device fd and one iommufd, with no group or container juggling.

## Rejected design ideas

**Have the primary ring the doorbell.** Secondaries would hold no MMIO at all,
which disposes of the trust question and of most of the socket. It is
disqualified because the doorbell has to be reachable from whatever submits.
GPU kernels submit their own I/O, and routing their doorbell writes back
through the host reintroduces exactly the round trip the device-initiated
modes exist to remove. This is not a latency trade to weigh; it contradicts
the feature.

**Let the secondary open the character device itself.** Measured, and it does
not work. `open()` succeeds, which is what makes it look plausible, and
`VFIO_DEVICE_BIND_IOMMUFD` then fails with `EINVAL` because a device cannot be
bound to two `iommufd_ctx`. Everything after the bind fails with it. See
measurement 1.

**Reopen the primary's descriptor through `/proc/PID/fd/N`.** This is how the
heap is imported today and it works there, because a `memfd` is a plain file
and re-opening its inode yields the same memory. It does not generalise to the
device: opening a character device that way re-invokes the driver's `open()`,
so the result is a fresh, unbound vfio file, which is the case measurement 1
already rejected. Both uses are gated by a ptrace access mode, which is why
the heap import is what forces secondaries to share the primary's uid today.

**Pull the descriptor with `pidfd_getfd(2)`.** It exists, and it runs the
wrong way. The caller pulls from the target, gated by
`PTRACE_MODE_ATTACH_REALCREDS`, so an unprivileged secondary cannot pull from
a root primary; arranging for it to be able to would grant ptrace-grade access
over the primary, which is more than the descriptor being delegated. There is
no push counterpart: the 6.x syscall table carries `pidfd_send_signal`,
`pidfd_open` and `pidfd_getfd`, and nothing that writes a descriptor into
another process. Yama's `ptrace_scope=1` would restrict it to descendants,
where inheritance is simpler anyway.

**Inherit the descriptors across `fork()` and `exec()`.** No socket, no
protocol, and the descriptors cross by not being `CLOEXEC`. It is disqualified
by the usage model: homi is already running when a program decides to attach,
and that program is started by a user or a shell rather than by homi.
Requiring homi to be the parent would make it a supervisor and would remove
the property that any process can attach to a running runtime. systemd's
`LISTEN_FDS` is the same mechanism with the same requirement.

**Use systemd's descriptor store to hand descriptors out.** `FDSTORE=1` is
`SCM_RIGHTS` underneath and it addresses a different question: it lets homi
restart without dropping the device. It cannot deliver a descriptor to an
arbitrary peer.

**Treat `IOMMU_IOAS_CHANGE_PROCESS` as sharing.** It transfers pinned memory
accounting to the calling process and supports only maps created with
`IOMMU_IOAS_MAP_FILE`. It is handover, aimed at userland live update, and is
relevant to a primary that restarts under live secondaries rather than to
delegation.

**Give each secondary its own address space with PASID.**
`VFIO_DEVICE_ATTACH_PASID` exists on this kernel, although the upstream
documentation still describes PASID as unsupported. PASID is the hardware
answer to per-process address spaces, and it is disqualified because it would
require the controller to associate queues with PASIDs, which is not how NVMe
generally works.

**Export the doorbells as a dma-buf and keep the device fd.** The design was
written around this and measurement 3 removed it. The export works; the
descriptor has no CPU mapping to give `cuMemHostRegister`, and neither runtime
imports it. It is recorded here because it is the first thing anyone will
propose on reading that vfio can export a BAR slice, and because it is the one
rejected idea that could stop being rejected. Two changes would revive it, and
neither is ours to make. If a GPU stack supported dma-buf import, the
descriptor could become a device pointer; NVIDIA's refusal is above the open
kernel modules, which do carry a generic importer in
`nv_dma_import_from_fd()`, so patching what is open does not reach it.
Alternatively, if the vfio-pci exporter implemented CPU `mmap`, the shipping
`cuMemHostRegister` path would work unchanged on both vendors with nothing
closed involved; that export was written for peer-to-peer DMA with no CPU
access, and revoking such mappings on device reset is the objection to
answer.

**Have every process attach through homi.** The appeal is one path with no
special case for a lone process. It is disqualified because a runtime that
shares with nobody would then require a daemon to be running, so `xnvme info`
against a controller would depend on homi. Construction unifies instead: a
lone process is a primary with no listener, which gives one path without
making anything depend on a service.

**Keep the named segments and add a socket only for descriptors.** Two
rendezvous mechanisms, two lifetime models, and stale-segment detection
retained for the case the socket was introduced to fix. If the socket exists
at all it should be the only way in.

## What is unverified

**Whether an abstract-namespace address is reachable at all.** Abstract
sockets are per network namespace, so a secondary in a container cannot reach
homi through one, where a filesystem socket could be bind-mounted in. For a
design aimed at GPU workloads that is not hypothetical.

**What a runtime with no arbiter does.** Measurement 6 shows a secondary can
keep mapping and reading after homi exits. What it cannot do is coordinate:
nothing hands out queue identifiers or owns the shared record. Whether a
runtime in that state should keep serving I/O, or stop, has not been decided
and cannot be measured.

## Testing

CI has no GPU, and its IOMMU-enabled job excludes uPCIe, so nothing in this
design is covered by what runs today. That is an argument for putting the UIO
path on the socket as well rather than a reason to accept the gap: attach, the
grant, disconnect-driven reaping and a version mismatch between a homi and a
differently-built secondary are all exercisable on `uio_pci_generic` against
emulated NVMe, which CI already provisions.

What stays lab-only is everything involving a GPU, on warp and wave, and under
the stance above those tests skip on a kernel that cannot map GPU memory into
an IOAS rather than asserting that it cannot. That should be recorded as
permanently uncovered by CI rather than left to be discovered when someone
asks why the GPU path has no test.

Two questions that gated this design were uPCIe questions and were answered
there: whether MMIO can be delegated as a descriptor, by
`upcie_vfio_bar_import_probe_{cuda,hip}`, and whether GPU memory can enter an
IOAS, by `upcie_vram_ioas_probe_{cuda,hip}`. What remains under "What is
unverified" does not block starting; what does block starting is the list of
decisions above it.

## Reproducing

The two programs used are in `toolbox/vfio-probes/`, with a README covering
how to build and run them. Neither is part of the build or links against
xNVMe; each is a sequence of ioctls that prints what the kernel returned at
every step.

`vfio_cdev_probe` produces the first measurement: run one instance holding a
device and a second against the same device. `vfio_share_probe` produces the
second: a primary that passes its descriptors over a unix socket, and a
secondary run under a different user with `su` that receives them.

Both need a device bound to `vfio-pci`, which makes `/dev/vfio/devices/vfioN`
appear.

The last three measurements are uPCIe's, since they are about uPCIe's
primitives: `test_dmamem_vfio_bar` for the export and its missing CPU mapping,
`upcie_vfio_bar_import_probe_{cuda,hip}` for the import,
`upcie_vfio_share_gpu_probe_cuda` for the delegated path, which serves one
secondary over a named socket so it can be started as any user, and
`upcie_vram_ioas_probe_{cuda,hip}` for GPU memory in an IOAS. The findings are
recorded in that project's `tools/README.md`.
