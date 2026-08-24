---
orphan: true
---
# HOMI under vfio: implementation plan

## What this is

An order of work for what `homi-design.md` decided, arranged so that every
step lands with something that fails when it is wrong, and so that no step
depends on a kernel capability that does not exist yet. Read the design first;
this document does not restate its reasoning and does not re-argue its
decisions.

Three constraints shape the order. The mechanism belongs in uPCIe and the
daemon in xNVMe, so the work runs bottom-up and the vendored headers move
before the backend does. Multi-process stops being a mode, so the
single-process path is rebuilt first and the shared case becomes a way of
arriving at the same structures. And CPU-submitted I/O into host memory
exercises everything except the last leg, so almost all of it is verifiable on
hardware today.

## Before anything starts

**An integration base exists**, since waiting for review is not the same as
knowing the design survives contact with the code. The `homi_vfio` branch is
xnvme/xnvme#743 with xnvme/xnvme#745 on top of it, which is where this work
happens; when either lands, the branch rebases and the duplicated commits fall
away. The risk taken is that a review changes one of them under us, and the
risk avoided is larger: that the plan rests on assumptions nobody has tried.

**Six decisions are taken.** Each is stated in the design; the recommendation
here is what I would implement absent an argument against.

1. Admin submission becomes a request to homi. This reverses the
   recommendation this plan first carried, and it is a trade rather than
   something the code forced: shared memory with a robust mutex works, and is
   what exists today. The round trip is on a cold path, and the cost worth
   naming is that admin then depends on homi being responsive. What buys it is
   an immutable record, read with no lock, and a single place to refuse the
   commands that are everybody's business rather than the caller's.
2. A secondary that outlives homi keeps serving I/O but cannot attach anything
   new, because measurement 6 shows the kernel permits the former and nothing
   permits the latter without an arbiter. homi's restart is then a fresh
   runtime, and handover through `IOMMU_IOAS_CHANGE_PROCESS` is deferred.
3. Queue identifiers are granted in the handshake and returned on disconnect,
   replacing the shared bitmap, since there is now an arbiter. The grant
   carries the queue's memory as heap offsets too, because the heap allocator
   is single-writer and stays with homi; a consumer never allocates from the
   shared heap, and registers its own buffers through its own iommufd.
4. The UIO path moves onto the socket too. One rendezvous, one lifetime model,
   and it is what lets CI cover the protocol without a GPU or an IOMMU.
5. `shm_id` becomes a socket address, one homi per controller, keyed by BDF.
   The old option keeps working for one release, resolving to the address.
6. Attachment is authorised by `SO_PEERCRED` against a uid or gid list homi
   carries, defaulting to the user homi runs as.

Decisions 4 and 5 are user-visible and reach the ctypes bindings, apigen and
the `-Dbe_upcie=false` stub build, none of which an ordinary Linux build
exercises.

## Phase 1: one construction path, no sockets

The single-process runtime is rebuilt so that a shared runtime is the same
thing arrived at differently. Nothing here needs two processes, so all of it
is covered by the existing suite plus one new test.

1. The inventory is done and it decided the shape. Every address reachable
   from `nvme_controller` sorts into data, heap offsets, BAR-derived values
   and things that must not be shared at all; the design records the
   classification. What follows is that the record carries data and offsets
   only, and that both sides construct a local controller from it, the primary
   included, which today runs directly on the shared struct.
2. Give the heap a descriptor and a header. Done: the heap opens with a header
   holding its size, the physical base of each hugepage backing it, and an
   offset the owner points consumers at; `hostmem_heap_attach()` maps a
   descriptor and translates from that table, so an importer needs neither a
   `/proc` path nor `CAP_SYS_ADMIN` to read pagemap. This retires a TODO the
   header already carried.
3. Place the record at a heap offset in every runtime. Done: the owner writes
   it into the heap and records the offset in the header, so a consumer finds
   it with nothing but the descriptor.
4. Add the record, the grant and their export and import beside the struct in
   uPCIe. Done: `nvme_runtime_record.h` carries
   `nvme_runtime_record_{export,import}` and
   `nvme_qpair_grant_{export,import,release}`. Implementing it added one thing
   the design had not foreseen: a consumer needs PRP scratch and has no
   allocator to get it from, so the grant names that too.

**Verified**, on warp against `uio_pci_generic`: the record is found through
the heap header, the heap is attached by descriptor with its physical
addresses taken from that header, the imported queue resolves to the same
doorbell and the same memory as the original, and a read of LBA 0 goes through
it. All in one process, so nothing here depends on the protocol that comes
next.

## Phase 2: the protocol, in uPCIe

What crosses is descriptors at attach and fixed-size messages after, none of
them on the I/O path.

1. Define the wire format. Done: `nvme_delegate.h` carries attach, grant,
   release and admin in one fixed-size message, so there is no framing to get
   wrong and a short read means the peer is gone rather than that more is
   coming.
2. Carry a version that covers the record layout, not just the protocol. Done
   in part: both the message and the record carry one and both are checked. A
   test that builds a deliberate mismatch is still missing.
3. Implement the transport. Done: send and receive with `SCM_RIGHTS`, a
   request that waits for its reply, and a partial-read loop that takes the
   ancillary data once, since descriptors arrive with the first byte. The
   serve loop is not here, because the bookkeeping is the owner's.
4. Decide the socket address family. Open. An abstract-namespace address
   leaves no debris and is unreachable from another network namespace, which a
   containerised consumer would need. Measure before choosing.
5. Authorise with `SO_PEERCRED`. Not started.

**Verified**, on warp against `uio_pci_generic`: an owner serves a controller
and a consumer holding nothing receives the heap and the BAR as descriptors,
finds the record through the heap header, is granted a queue, reads LBA 0 on
it through its own doorbell, and has the owner submit an identify whose
payload lands in the consumer's own memory. Two real processes, and the only
thing to cross the socket for that identify was the command and its
completion.

## Phase 3: homi becomes the arbiter

1. homi gains a serve mode: bind the controller, build the runtime, listen,
   accept, grant queues, and reap on disconnect. The accept loop is a thread,
   so status and serving do not block each other.
2. Fix the teardown order and write it down where the code does it: delete the
   submission and completion queues, drain what is outstanding, unmap the
   secondary's IOAS ranges, release its queue identifiers. Unmapping before
   the queues are gone lets the controller DMA through addresses that no
   longer resolve.
3. Handle partial attachment: a secondary that dies between the grant and the
   first submission leaves a grant behind, and the disconnect path is the only
   thing that will notice.
4. Shrink `xnvme_be_upcie_mproc.c` to backend wiring: attach through the uPCIe
   import path, and delete the pointer surgery, both segments and both lock
   files.

**Verified by** cijoe tests on `uio_pci_generic` against emulated NVMe, which
CI already provisions: attach, a granted queue doing I/O, a secondary killed
mid-I/O and its queues reaped, a version mismatch refused, and a second
secondary attaching after the first has gone. None of this needs a GPU or an
IOMMU, which is the point of decision 4.

## Phase 4: the vfio path

1. Remove the up-front rejection of `vfio-pci` in the CUDA and HIP backends.
   An early guess about kernel support is worse than a precise failure at the
   point of use.
2. Add a capability probe next to `iommufd.h` that asks once whether GPU
   memory can enter an IOAS, and report its answer in one place with an error
   naming `IOMMU_IOAS_MAP_FILE`.
3. Make `xnvme_mem_map` work on the vfio path for host memory, registering
   through the secondary's own iommufd.
4. Mark the VRAM leg as capability-gated: the tests skip where the kernel
   lacks it rather than asserting that it does, so a kernel carrying the
   support turns them green.

**Verified by** the lab: a secondary on `vfio-pci` doing CPU-submitted I/O
into host memory it registered itself, on warp and wave. The GPU leg is
verified only on a kernel that has the capability, which today means an
out-of-tree one.

## What each phase risks

Phase 1 is the one that can break working software, since it rebuilds the
single-process path that everything already depends on. It is also the phase
with no new externally visible behaviour, so it can land quietly and be
reverted cheaply.

Phase 2 risks designing a protocol around today's needs and then finding the
record layout question was harder than the transport. Doing the pointer
inventory in phase 1 rather than phase 2 is what keeps that from surfacing
late.

Phase 3 is where a mistake costs data: teardown ordering under an IOMMU is the
difference between a clean reap and a controller writing into memory that has
been handed back. It deserves a test that kills a secondary at several points,
not one.

Phase 4 risks nothing except discovering that the gated leg stays gated.

## What this plan does not cover

`XNVME_BE_UPCIE_MODE_VFIO_TYPE1` and kernels without the vfio cdev interface.
Every measurement behind the design used the cdev with iommufd on Linux 7.0,
and what the socket path does below that floor is undecided.

Handover across a homi restart. Decision 2 defers it, and
`IOMMU_IOAS_CHANGE_PROCESS` supports only maps made with
`IOMMU_IOAS_MAP_FILE`, which constrains how the heap must be mapped if it is
ever taken up.

GPU-initiated submission on AMD, which does not exist to be planned for:
`hipHostRegisterIoMemory` is documented as unsupported, and there is no HIP
counterpart to `nvme_qpair_cuda.h`.
