#!/usr/bin/env python3
"""
Probe whether a HIP kernel reaches an NVMe controller's registers
=================================================================

GPU-initiated IO has a kernel ring the controller's doorbells, which are in its
BAR0. The CUDA backend makes them reachable with cuMemHostRegister() and
CU_MEMHOSTREGISTER_IOMEMORY; this tries the HIP counterpart,
hipHostRegister() with hipHostRegisterIoMemory, on BAR0 mapped from sysfs, and
has a kernel read CAP and VS through it. It passes when the kernel reads what
the CPU does. The controller is detached from the kernel first, since a
kernel holding BAR0 keeps sysfs from mapping it.

Only reads: a write to a doorbell is only checked by a command completing,
which needs a queue the probe does not own.

Retargetable: True
"""

import errno
import logging as log
import re
from argparse import ArgumentParser

PROBE = r"""
#include <hip/hip_runtime.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define NREGS 3 /* CAP (two dwords) and VS */
#define MAP_NBYTES 0x1000

__global__ void
read_regs(volatile uint32_t *bar0, uint32_t *out)
{
	for (int i = 0; i < NREGS; ++i) {
		out[i] = bar0[i];
	}
}

int
main(int argc, char **argv)
{
	uint32_t cpu[NREGS], gpu[NREGS], *out = NULL;
	char path[256];
	void *bar0, *dptr = NULL;
	hipError_t res;
	int fd;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <bdf>\n", argv[0]);
		return 2;
	}
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource0", argv[1]);
	fd = open(path, O_RDWR | O_SYNC);
	if (fd < 0) {
		perror(path);
		return 1;
	}
	bar0 = mmap(NULL, MAP_NBYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (bar0 == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	for (int i = 0; i < NREGS; ++i) {
		cpu[i] = ((volatile uint32_t *)bar0)[i];
	}

	res = hipHostRegister(bar0, MAP_NBYTES, hipHostRegisterIoMemory | hipHostRegisterMapped);
	if (res != hipSuccess) {
		fprintf(stderr, "hipHostRegister(IoMemory): %s\n", hipGetErrorString(res));
		return 1;
	}
	res = hipHostGetDevicePointer(&dptr, bar0, 0);
	if (res != hipSuccess) {
		fprintf(stderr, "hipHostGetDevicePointer(): %s\n", hipGetErrorString(res));
		return 1;
	}
	if (hipMalloc((void **)&out, sizeof(gpu)) != hipSuccess) {
		fprintf(stderr, "hipMalloc() failed\n");
		return 1;
	}

	hipLaunchKernelGGL(read_regs, dim3(1), dim3(1), 0, 0, (volatile uint32_t *)dptr, out);
	res = hipDeviceSynchronize();
	if (res != hipSuccess) {
		fprintf(stderr, "kernel: %s\n", hipGetErrorString(res));
		return 1;
	}
	if (hipMemcpy(gpu, out, sizeof(gpu), hipMemcpyDeviceToHost) != hipSuccess) {
		fprintf(stderr, "hipMemcpy() failed\n");
		return 1;
	}

	printf("cpu: cap=0x%08" PRIx32 "%08" PRIx32 " vs=0x%08" PRIx32 "\n", cpu[1], cpu[0],
	       cpu[2]);
	printf("gpu: cap=0x%08" PRIx32 "%08" PRIx32 " vs=0x%08" PRIx32 "\n", gpu[1], gpu[0],
	       gpu[2]);
	for (int i = 0; i < NREGS; ++i) {
		if (cpu[i] != gpu[i]) {
			fprintf(stderr, "MISMATCH: the kernel did not read the registers\n");
			return 1;
		}
	}

	return 0;
}
"""


def add_args(parser: ArgumentParser):
    parser.add_argument(
        "--label", default="hip", help="Probe the first device with this label"
    )
    parser.add_argument(
        "--offload-arch",
        default=None,
        help="The GPU's target; default: '-Dhip_offload_arch' of xnvme.build.setup_args",
    )


def offload_arch(args, cijoe):
    """The target the probe is built for, as xNVMe's build gets it"""

    if args.offload_arch:
        return args.offload_arch

    setup_args = cijoe.getconf("xnvme.build.setup_args", "") or ""
    match = re.search(r"-Dhip_offload_arch=(\S+)", setup_args)

    return match.group(1) if match else None


def main(args, cijoe):
    """Build the probe on the target and run it against the labelled controller"""

    device = next(
        (d for d in cijoe.getconf("devices", []) if args.label in d.get("labels", [])),
        None,
    )
    if not device:
        log.error(f"FAILED: no device labelled '{args.label}'")
        return errno.ENOENT

    err, _ = cijoe.run("xnvme-driver")
    if err:
        log.error("FAILED: detaching the controller from the kernel")
        return err

    err, state = cijoe.run("mktemp -d /tmp/hip_bar_probe.XXXXXX")
    if err:
        return err
    workdir = state.output().strip()

    try:
        err, _ = cijoe.run(f"cat > {workdir}/probe.hip <<'EOF'\n{PROBE}\nEOF")
        if err:
            return err
        arch = offload_arch(args, cijoe)
        arch_arg = f"--offload-arch={arch}" if arch else ""
        err, _ = cijoe.run(f"hipcc {arch_arg} -o {workdir}/probe {workdir}/probe.hip")
        if err:
            log.error("FAILED: building the probe")
            return err
        err, _ = cijoe.run(f"{workdir}/probe {device['uri']}")
        if err:
            log.error("FAILED: a HIP kernel did not read the controller's registers")
        return err
    finally:
        cijoe.run(f"rm -rf {workdir}")
