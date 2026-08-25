(sec-backends-upcie-mproc)=
# uPCIe (multi-process)

The **upcie** backend supports a **multi-process** mode in which several
independent processes share NVMe controllers. One process **owns** a
controller: it opened the device, allocated the DMA memory, and holds the
admin queue. Any number of **consumers** attach to what it owns and drive
their own I/O without re-initializing anything.

Ownership is not something a library user falls into. The owner is
{ref}`sec-tools-homi`, which exists to be one, and a consumer that finds
nobody serving simply runs on its own.

```bash
# The owner holds the controller for as long as it runs
homi start --shm_id 1 --be upcie 0000:03:00.0

# Consumers name the same id and attach to it
xnvme info 0000:03:00.0 --be upcie --shm_id 1
```

(sec-backends-upcie-mproc-model)=
## Process model

### Rendezvous

An owner listens on a unix socket named for the identifier consumers pass,
`/tmp/xnvme-homi-<shm_id>.sock`. A consumer derives the same name from
`--shm_id` and connects; finding nothing there means nobody is serving that
identifier, and the process builds its own runtime instead.

A socket rather than a shared segment because a segment cannot carry a file
descriptor, and under `vfio-pci` a descriptor is the only thing that grants
access to a device. It is a filesystem path rather than an abstract name
because an abstract address cannot be reached from another network namespace,
which a containerised consumer would need.

### What crosses at attach

Descriptors, and offsets into what they describe:

- the DMA heap, which the consumer maps to see the same memory;
- BAR0, so that the consumer rings its own doorbells;
- the offset of a **runtime record**, written once by the owner, naming the
  controller and where to find a description of the heap.

The heap description carries the physical address of each granule. The owner
read those when it allocated, which needs `CAP_SYS_ADMIN`; leaving them where
the consumer will map them anyway is what lets an unprivileged consumer
translate at all.

### What a consumer does for itself, and what it asks for

A consumer submits I/O on queues it was granted, ringing its own doorbell.
Nothing is on the socket during I/O.

Everything else is a request, because the resources belong to the owner:

- **Queues.** Creating one means an admin command and memory from the heap, so
  the consumer asks and receives an identifier plus the offsets of its
  submission queue, completion queue and PRP scratch.
- **Memory.** The heap's allocator is the owner's and its free list has no
  lock, so `xnvme_buf_alloc()` asks for an offset.
- **Admin commands.** There is one admin queue and it belongs to the owner.
  The payload does not travel with the request: the command names an address
  the device can already reach, so an identify lands in the consumer's own
  buffer and only the command and its completion cross.

### Liveness

The connection is the liveness signal. A consumer that exits cleanly hands its
queues and memory back; one that is killed mid-command does not, and the
socket closing is what tells the owner to reclaim them, queues first so that
the controller loses its reach on an address before the address stops
resolving.

Nothing is left behind for anyone to clean up, and nothing has to be told
apart from debris: a socket that answers has a process behind it.

(sec-backends-upcie-mproc-vulns)=
## Limitations

### No isolation between processes

A consumer maps BAR0 and can therefore ring any doorbell and write `CC`, which
is to say it can reset the controller. Under `vfio-pci` it also holds the
device descriptor. Consumers are inside one trust domain by construction;
where that is unacceptable, the answer is the kernel driver, which arbitrates
because it owns the device.

### One controller per owner

The protocol names no controller, so an owner serves the one it holds. Serving
several from one process is not implemented and is refused rather than guessed
at.

### The owner has to be answering

Attaching, allocating and admin all depend on the owner being responsive.
Under the arrangement this replaced, a primary stuck in a poll loop blocked
nobody, because those were reads of shared memory. That is the price of having
one place where a policy could be applied and of a record that needs no lock.

### GPU consumers

A controller behind an IOMMU cannot yet DMA into VRAM: `IOMMU_IOAS_MAP_FILE`
does not accept dma-bufs exported by CUDA or HIP. GPU workloads therefore stay
on `uio_pci_generic` until that changes, and the vfio path serves
CPU-submitted I/O into host memory.
