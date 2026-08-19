#!/usr/bin/env python
"""
Benchmark the uPCIe backends, single-process and multi-process
=============================================================

Runs `xnvmeperf` against the uPCIe backends in a set of shapes and records
IOPS, MiB/s and failed-IO counts for each. The shapes exist to answer two
questions that come up whenever the backend's memory handling changes:

* Did throughput move, for a given backend and attachment mode?
* Does routing I/O through a HOMI primary cost anything on the data path?

Shapes
------

flat
: One `xnvmeperf` process owning the devices exclusively. The baseline.

mproc
: A `homi` primary holding the devices, with `--secondaries` concurrent
  `xnvmeperf` processes attaching to it. With one secondary this compares
  against `flat` directly; with more it shows the fan-out.

The `upcie-cuda` backend uses `xnvmeperf cuda-run`, so data buffers come from
GPU memory; `upcie` and `upcie-hip` use `xnvmeperf run`.

Config Arguments
----------------

upcie.bench.devices: list of device URIs, e.g. ["0000:01:00.0"]
upcie.bench.shm_id: shared-memory id used for the multi-process shapes
upcie.bench.host_heap_size: optional, passed to homi as --host_heap_size
upcie.bench.device_heap_size: optional, passed to homi as --device_heap_size
xnvme.repository.path: where the binaries are, defaults to the installed ones

Retargetable: True
------------------
"""

import logging as log
import re
from argparse import ArgumentParser
from pathlib import Path

HOMI_LOG = "/tmp/upcie_bench_homi.log"
HOMI_PID = "/tmp/upcie_bench_homi.pid"

# " Total:                    1234567.89   4567.12        0"
TOTALS = re.compile(r"^\s*Total:\s+([0-9.]+)\s+([0-9.]+)\s+([0-9]+)\s*$", re.MULTILINE)


def add_args(parser: ArgumentParser):
    parser.add_argument(
        "--backend", type=str, default="upcie", help="upcie, upcie-cuda or upcie-hip"
    )
    parser.add_argument(
        "--shapes",
        type=str,
        nargs="+",
        default=["flat", "mproc"],
        help="Which shapes to run",
    )
    parser.add_argument(
        "--secondaries",
        type=int,
        nargs="+",
        default=[1],
        help="Concurrent xnvmeperf processes for the mproc shape",
    )
    parser.add_argument("--iopattern", type=str, default="randread")
    parser.add_argument("--iosize", type=int, default=512)
    parser.add_argument("--qdepth", type=int, default=128)
    parser.add_argument("--nqueues", type=int, default=1)
    parser.add_argument("--runtime", type=int, default=10)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument(
        "--cpulists",
        type=str,
        nargs="+",
        default=["0,32"],
        help="CPU pinning to sweep, one run per entry",
    )
    parser.add_argument(
        "--ndevices",
        type=int,
        default=0,
        help="Use only the first N devices; 0 uses every device in the config. "
        "Single-device runs measure the device, aggregates measure the host",
    )
    parser.add_argument(
        "--label",
        type=str,
        default="",
        help="Names the results file; defaults to the backend. Give each step "
        "its own label, otherwise two steps on the same backend overwrite "
        "each other's results",
    )
    parser.add_argument(
        "--vfio_modes",
        type=str,
        nargs="+",
        default=[""],
        help="XNVME_UPCIE_VFIO_MODE values to sweep; empty string leaves it unset",
    )


def _binary(cijoe, name):
    """Path to a tool, preferring a build in the repository over the install"""

    repos = cijoe.getconf("xnvme.repository.path", "")
    if not repos:
        return name

    candidates = {
        "homi": f"{repos}/builddir/tools/homi",
        "xnvmeperf": f"{repos}/builddir/tools/xnvmeperf/xnvmeperf",
    }
    path = candidates.get(name, name)

    err, _ = cijoe.run(f"test -x {path}")

    return path if not err else name


def _totals(output):
    """Returns (iops, mibps, failed) summed over the run, or None"""

    matches = TOTALS.findall(output)
    if not matches:
        return None

    iops = sum(float(match[0]) for match in matches)
    mibps = sum(float(match[1]) for match in matches)
    failed = sum(int(match[2]) for match in matches)

    return iops, mibps, failed


def _homi_start(args, cijoe, devices):
    """Start a HOMI primary in the background; returns 0 when it is serving"""

    heap_args = ""
    host_heap = cijoe.getconf("upcie.bench.host_heap_size", 0)
    device_heap = cijoe.getconf("upcie.bench.device_heap_size", 0)
    if host_heap:
        heap_args += f" --host_heap_size {host_heap}"
    if device_heap:
        heap_args += f" --device_heap_size {device_heap}"

    shm_id = cijoe.getconf("upcie.bench.shm_id", 1)
    homi = _binary(cijoe, "homi")

    cijoe.run(f"rm -f {HOMI_LOG} {HOMI_PID}")
    cijoe.run(
        f"nohup {homi} start {' '.join(devices)} --be {args.backend}"
        f" --shm_id {shm_id}{heap_args} > {HOMI_LOG} 2>&1 &"
        f" echo $! > {HOMI_PID}"
    )

    # homi says so itself once the devices are open; until then a secondary
    # attaching would race the primary and fail with -ENOENT
    for _ in range(30):
        err, _ = cijoe.run(f"grep -q 'HOMI started successfully' {HOMI_LOG}")
        if not err:
            return 0
        cijoe.run("sleep 1")

    log.error("HOMI did not come up")
    cijoe.run(f"cat {HOMI_LOG}")

    return 1


def _homi_stop(cijoe):
    """Stop the HOMI primary and check that it left no shared state behind"""

    cijoe.run(f"kill $(cat {HOMI_PID}) 2>/dev/null; sleep 2")
    cijoe.run(f"kill -9 $(cat {HOMI_PID}) 2>/dev/null; true")
    cijoe.run("ls -l /dev/shm/xnvme-upcie-* /tmp/xnvme-upcie-* 2>/dev/null; true")


def _cpu_count(cpulist):
    """Number of CPUs a cpulist such as '0,32' or '0-8' names"""

    count = 0
    for part in cpulist.split(","):
        if "-" in part:
            first, last = part.split("-", 1)
            count += int(last) - int(first) + 1
        else:
            count += 1

    return count


def _perf_cmd(args, cijoe, devices, cpulist, shm_id=None):
    """The xnvmeperf invocation for one process"""

    # GPU queues are driven from the device, so cuda-run has no CPU pinning
    # to give; passing --cpulist to it is an error rather than a no-op.
    subcmd = "cuda-run" if args.backend == "upcie-cuda" else "run"

    # Every thread needs a queue of its own, and a host with fewer devices
    # than the workflow assumes would otherwise fail with a usage error
    nqueues = args.nqueues
    if subcmd == "run":
        threads = _cpu_count(cpulist)
        nqueues = max(nqueues, -(-threads // len(devices)))

    cmd = (
        f"{_binary(cijoe, 'xnvmeperf')} {subcmd} {' '.join(devices)}"
        f" --be {args.backend}"
        f" --iopattern {args.iopattern}"
        f" --iosize {args.iosize}"
        f" --qdepth {args.qdepth}"
        f" --nqueues {nqueues}"
        f" --runtime {args.runtime}"
    )
    if subcmd == "run":
        cmd += f" --cpulist {cpulist}"
    if shm_id is not None:
        cmd += f" --shm_id {shm_id}"

    return cmd


def _run_shape(args, cijoe, shape, nprocs, devices, cpulist, env):
    """Run one shape once; returns the parsed totals or None"""

    shm_id = cijoe.getconf("upcie.bench.shm_id", 1)

    if shape == "mproc" and _homi_start(args, cijoe, devices):
        return None

    cmd = _perf_cmd(args, cijoe, devices, cpulist, shm_id if shape == "mproc" else None)

    if nprocs == 1:
        err, state = cijoe.run(cmd, env=env)
    else:
        # Concurrent secondaries have to overlap in time for the fan-out to
        # mean anything, so start them all and wait for the last one. They
        # are backgrounded in this shell rather than in subshells, otherwise
        # 'wait' would have no children to wait for and return at once.
        parts = " ".join(f"{cmd} &" for _ in range(nprocs))
        err, state = cijoe.run(f"{parts} wait", env=env)

    if shape == "mproc":
        _homi_stop(cijoe)

    if err:
        log.error(f"xnvmeperf failed; err({err})")
        return None

    # xnvmeperf exits 0 on a usage error, so a missing results table is the
    # only signal that the run did not happen
    totals = _totals(Path(state.output_fpath).read_text())
    if totals is None:
        log.error(f"no results from: {cmd}")

    return totals


def main(args, cijoe):
    """Run the benchmark matrix"""

    devices = cijoe.getconf("upcie.bench.devices", [])
    if not devices:
        log.error("no upcie.bench.devices in the configuration")
        return 1

    if args.ndevices:
        devices = devices[: args.ndevices]
    log.info(f"devices({len(devices)}): {' '.join(devices)}")

    results = []
    first_err = 0

    for vfio_mode in args.vfio_modes:
        env = {"XNVME_UPCIE_VFIO_MODE": vfio_mode} if vfio_mode else {}

        for cpulist in args.cpulists:
            for shape in args.shapes:
                counts = args.secondaries if shape == "mproc" else [1]

                for nprocs in counts:
                    for rep in range(args.repetitions):
                        totals = _run_shape(
                            args, cijoe, shape, nprocs, devices, cpulist, env
                        )
                        if totals is None:
                            first_err = first_err if first_err else 1
                            continue

                        iops, mibps, failed = totals
                        results.append(
                            {
                                "backend": args.backend,
                                "ndevices": len(devices),
                                "vfio_mode": vfio_mode or "auto",
                                "shape": shape,
                                "nprocs": nprocs,
                                "cpulist": cpulist,
                                "rep": rep,
                                "iops": iops,
                                "mibps": mibps,
                                "failed": failed,
                            }
                        )
                        if failed:
                            log.error(f"failed IOs({failed}) in {shape}/{nprocs}")
                            first_err = first_err if first_err else 1

    artifacts = Path(args.output) / "artifacts"
    artifacts.mkdir(parents=True, exist_ok=True)

    label = args.label or args.backend
    with (artifacts / f"upcie-bench-{label}.csv").open("w") as csv:
        csv.write(
            "backend,ndevices,vfio_mode,shape,nprocs,cpulist,rep,iops,mibps,failed\n"
        )
        for res in results:
            csv.write(
                f"{res['backend']},{res['ndevices']},{res['vfio_mode']},{res['shape']},"
                f"{res['nprocs']},\"{res['cpulist']}\",{res['rep']},"
                f"{res['iops']:.2f},{res['mibps']:.2f},{res['failed']}\n"
            )

    for res in results:
        log.info(
            f"{res['backend']} {res['shape']}({res['nprocs']})"
            f" cpus({res['cpulist']}) mode({res['vfio_mode']}):"
            f" {res['iops']:.0f} IOPS, {res['mibps']:.0f} MiB/s"
        )

    return first_err
