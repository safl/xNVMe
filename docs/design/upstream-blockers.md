---
orphan: true
---

# What we are waiting on, and who can unblock it

## What this is

A record of the things outside this project that stop parts of it from
working, each with what it blocks, what was measured, and who would have to
agree for it to change. It exists so that engaging upstream is a matter of
picking an item rather than reconstructing the argument, and so that a
capability arriving is noticed rather than rediscovered.

Every entry says how it was established. Where a maintainer is named, treat it
as a starting point and confirm with the kernel's own
`scripts/get_maintainer.pl` before sending anything: the names here are from
recollection of who works on these subsystems, not from a lookup.

## 1. `IOMMU_IOAS_MAP_FILE` refuses dma-bufs exported by GPU runtimes

**Blocks.** An NVMe controller behind an IOMMU DMAing into GPU memory. That is
the whole of the GPU story on `vfio-pci`: without it, GPU consumers stay on
`uio_pci_generic`, which means no IOMMU, and it is why the multi-process work
serves CPU-submitted I/O into host memory on the vfio path.

**Measured.** `upcie_vram_ioas_probe_{cuda,hip}` on Linux 7.0.0-28-generic:
mapping a `memfd` succeeds, mapping a dma-buf exported by CUDA or by HIP
returns `ENOTSUP`, on both an NVIDIA RTX A6000 and an AMD Radeon RX 7800 XT.
uPCIe's `iommufd.h` documents the same restriction as of 6.19.

**Who.** iommufd, and the dma-buf side of it. Jason Gunthorpe and Kevin Tian
for iommufd; Christian König and Sumit Semwal for dma-buf. Lists:
`iommu@lists.linux.dev`, `linux-media@vger.kernel.org`, `linux-kernel`.

**What the argument has to answer.** vfio-pci exports MMIO as a dma-buf and
iommufd accepts that, so the mechanism exists and the question is which
exporters it may take. A GPU exporter can move or revoke its pages, which
mapping into an IOAS has to survive; `move_notify` is the hook, and whether
iommufd wants to carry that is the discussion rather than whether the ioctl
can be relaxed.

## 2. The vfio-pci dma-buf has no CPU mapping

**Blocks.** Delegating a controller's doorbells to another process without
handing it the device descriptor. With a CPU mapping, a consumer could be
given a dma-buf covering only the doorbell page and would have no route to
`CC`, `CSTS` or a controller reset; without one, the device fd itself has to
cross and consumers land inside one trust domain.

**Measured.** `test_dmamem_vfio_bar` reports `cpu_va=unavailable` for every
slice size tried, and the design records the consequence.

**Who.** vfio: Alex Williamson. The export series came from Leon Romanovsky
with Jason Gunthorpe. Lists: `kvm@vger.kernel.org`, `linux-kernel`.

**What the argument has to answer.** The export was written for peer-to-peer
DMA, where no CPU access is wanted. Adding `mmap` means the mapping has to be
revoked when the device resets, which vfio already does for mappings made
through the device fd, so the machinery exists; the question is whether the
exporter should carry it.

## 3. CUDA does not import dma-bufs

**Blocks.** The alternative to item 2: if the GPU runtime imported the
descriptor itself, a consumer would need no CPU mapping of the doorbell.

**Measured.** `upcie_vfio_bar_import_probe_cuda` with
`CU_EXTERNAL_MEMORY_HANDLE_TYPE_DMABUF_FD` returns `CUDA_ERROR_NOT_SUPPORTED`,
and so does the control that imports a dma-buf CUDA exported itself, so this
is not about vfio or about MMIO. Nothing appears in the kernel log while it
happens, and the open kernel modules' importer prints on both of its failure
paths, which places the refusal above them.

**Who.** NVIDIA. The decision is in the closed userspace driver or in the RM
blob; `open-gpu-kernel-modules` carries a generic importer that nothing calls.
Engagement is a developer-forum report or an issue against
`NVIDIA/open-gpu-kernel-modules` asking where the capability is gated.

## 4. HIP has no I/O-memory host registration

**Blocks.** GPU-initiated NVMe submission on AMD, entirely. A GPU kernel rings
a doorbell by writing a host mapping of the BAR registered as I/O memory; on
CUDA that is `cuMemHostRegister(CU_MEMHOSTREGISTER_IOMEMORY)`, and ROCm has no
equivalent.

**Measured.** `hipHostRegisterIoMemory` is documented in
`hip_runtime_api.h` as "Not supported", and the registration returns
`hipErrorInvalidValue` even as root. uPCIe has no HIP counterpart to
`nvme_qpair_cuda.h`, which is the same fact from the other side.

**Who.** AMD, via the ROCm HIP runtime. `ROCm/HIP` and `ROCm/clr` on GitHub.

## 5. HIP has no dma-buf external-memory handle type

**Blocks.** Importing any dma-buf into HIP, which is item 3's question for
AMD.

**Measured.** `hipExternalMemoryHandleType` ends at `NvSciBuf`; there is no
`DmaBuf` member, so there is nothing to ask with.

**Who.** As item 4.

## 6. No way to read a dma-buf's addresses from userspace

**Blocks.** Translating a GPU allocation without an out-of-tree module. uPCIe
carries one, `experimental/dmabuf_import`, purely to read the scatter list of
an imported dma-buf, and a deployment that cannot load an out-of-tree module
cannot use the GPU paths at all.

**Measured.** The module exists because there is no UAPI for it; that is its
stated reason.

**Who.** dma-buf: Christian König, Sumit Semwal, `linux-media`. This is the
hardest of the list to argue, since exposing physical addresses to userspace
is what the interface deliberately avoids, and the honest framing is what
problem needs solving rather than what interface we want.

## Not blocking, but worth knowing

**`IOMMU_IOAS_CHANGE_PROCESS` accepts only `MAP_FILE` mappings.** It is the
kernel's answer for handing a device to a restarted process, and taking it up
would constrain how the heap is mapped. Nothing depends on it today because a
homi restart is not yet a supported handover.

**There is no push counterpart to `pidfd_getfd`.** Moving a descriptor to an
unrelated process is `SCM_RIGHTS` and a socket, which is what the design does.
A `pidfd_setfd` would remove the socket from the attach path but nothing else,
so this is a convenience rather than a blocker. Gatekeeper: Christian Brauner.

**`RLIMIT_MEMLOCK` bounds no part of an iommufd mapping.** Measured with the
limit at 64 KiB against 2 MiB mappings, from both sides of a delegation. It is
recorded because a design decision was made on the assumption that it did.
