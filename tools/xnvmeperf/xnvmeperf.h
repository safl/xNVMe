// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

#ifndef __XNVMEPERF_H
#define __XNVMEPERF_H

#include <stddef.h>
#include <stdint.h>
#include <errno.h>

#include <libxnvme.h>

enum iopattern {
	IOPATTERN_READ      = 1,
	IOPATTERN_WRITE     = 2,
	IOPATTERN_RANDREAD  = 3,
	IOPATTERN_RANDWRITE = 4,
	IOPATTERN_VERIFY    = 5, ///< Used for verify subcommand
};

struct xnvmeperf_args {
	int ndevs;
	const char **dev_uris;
	uint16_t ncpus;
	uint16_t *cpus;
	uint32_t qdepth;
	uint32_t iosize;
	uint32_t time;
	uint32_t count;
	uint32_t nqueues;
	double report_freq;
	enum iopattern pattern;
	int queue_opts;      ///< Passed to xnvme_queue_init() or xnvme_cuda_queue_create()
	int buf_host_bounce; ///< Read into host memory and copy each payload to the GPU
	int buf_hostmem;     ///< Payloads in host memory under a GPU backend, the reverse split
	struct xnvme_opts opts;
};

int
fill_pattern(void *buf, size_t nbytes, uint64_t slba, uint16_t nlb);

void
print_intermediate_header(void);

void
print_intermediate_result(double elapsed, uint64_t completed, uint32_t iosize);

#ifdef XNVME_BE_UPCIE_CUDA_ENABLED
int
xnvmeperf_cuda_run_io(struct xnvme_dev **devs, const struct xnvmeperf_args *args,
		      uint64_t *rounds_per_dev, uint64_t *failed_per_dev, float *elapsed_ms);
int
xnvmeperf_cuda_verify_io(struct xnvme_dev **devs, const struct xnvmeperf_args *args);
#else
static inline int
xnvmeperf_cuda_run_io(struct xnvme_dev **XNVME_UNUSED(devs),
		      const struct xnvmeperf_args *XNVME_UNUSED(args),
		      uint64_t *XNVME_UNUSED(rounds_per_dev),
		      uint64_t *XNVME_UNUSED(failed_per_dev), float *XNVME_UNUSED(elapsed_ms))
{
	return -ENOSYS;
}

static inline int
xnvmeperf_cuda_verify_io(struct xnvme_dev **XNVME_UNUSED(devs),
			 const struct xnvmeperf_args *XNVME_UNUSED(args))
{
	return -ENOSYS;
}
#endif

/*
 * Host-bounce staging (``run --buf-host-bounce``) and the host-to-device copy
 * roofline (``htod-roofline``). The NVMe I/O stays on a host backend; these
 * move the payload on to the GPU with the vendor runtime's async copy. The
 * generic loop in xnvmeperf.c owns the per-slot host buffers and the free-list;
 * the vendor file (CUDA .cu, HIP .hip) owns the device buffer, the copy stream
 * and the per-slot completion events. One opaque handle per queue.
 */
struct xnvmeperf_gpu;

#if defined(XNVME_BE_UPCIE_CUDA_ENABLED) || defined(XNVME_BE_UPCIE_HIP_ENABLED)

/** Make @p gpu_id the calling thread's device before any copy on it. */
int
xnvmeperf_gpu_set_device(uint32_t gpu_id);

/** A device buffer of @p nslots * @p iosize, a copy stream and @p nslots events. */
struct xnvmeperf_gpu *
xnvmeperf_gpu_bounce_open(uint32_t gpu_id, uint32_t iosize, uint32_t nslots);

/** Page-lock a host buffer so its copy runs at link speed; kept for teardown. */
int
xnvmeperf_gpu_bounce_register(struct xnvmeperf_gpu *gpu, uint32_t slot, void *hbuf);

/** 1 when @p slot's previous copy has finished (or it never copied), else 0. */
int
xnvmeperf_gpu_bounce_ready(struct xnvmeperf_gpu *gpu, uint32_t slot);

/** Enqueue the host-to-device copy of @p slot and record its completion event. */
int
xnvmeperf_gpu_bounce_copy(struct xnvmeperf_gpu *gpu, uint32_t slot, void *hbuf);

/** Wait for every enqueued copy to finish. */
void
xnvmeperf_gpu_bounce_drain(struct xnvmeperf_gpu *gpu);

/** Unregister the host buffers and free the device buffer, stream and events. */
void
xnvmeperf_gpu_bounce_close(struct xnvmeperf_gpu *gpu);

/**
 * Host-to-device copy roofline: keep @p nslots pinned copies of @p iosize in
 * flight for @p seconds and report the delivered GB/s in @p gbps.
 */
int
xnvmeperf_htod_roofline(uint32_t gpu_id, uint32_t iosize, uint32_t nslots, uint32_t seconds,
			double *gbps);

#else
static inline int
xnvmeperf_gpu_set_device(uint32_t XNVME_UNUSED(gpu_id))
{
	return -ENOSYS;
}
static inline struct xnvmeperf_gpu *
xnvmeperf_gpu_bounce_open(uint32_t XNVME_UNUSED(gpu_id), uint32_t XNVME_UNUSED(iosize),
			  uint32_t XNVME_UNUSED(nslots))
{
	errno = ENOSYS;
	return NULL;
}
static inline int
xnvmeperf_gpu_bounce_register(struct xnvmeperf_gpu *XNVME_UNUSED(gpu), uint32_t XNVME_UNUSED(slot),
			      void *XNVME_UNUSED(hbuf))
{
	return -ENOSYS;
}
static inline int
xnvmeperf_gpu_bounce_ready(struct xnvmeperf_gpu *XNVME_UNUSED(gpu), uint32_t XNVME_UNUSED(slot))
{
	return 0;
}
static inline int
xnvmeperf_gpu_bounce_copy(struct xnvmeperf_gpu *XNVME_UNUSED(gpu), uint32_t XNVME_UNUSED(slot),
			  void *XNVME_UNUSED(hbuf))
{
	return -ENOSYS;
}
static inline void
xnvmeperf_gpu_bounce_drain(struct xnvmeperf_gpu *XNVME_UNUSED(gpu))
{
}
static inline void
xnvmeperf_gpu_bounce_close(struct xnvmeperf_gpu *XNVME_UNUSED(gpu))
{
}
static inline int
xnvmeperf_htod_roofline(uint32_t XNVME_UNUSED(gpu_id), uint32_t XNVME_UNUSED(iosize),
			uint32_t XNVME_UNUSED(nslots), uint32_t XNVME_UNUSED(seconds),
			double *XNVME_UNUSED(gbps))
{
	return -ENOSYS;
}
#endif

/**
 * A resident GPU checker for the p2p-verify sub-command: it reads a payload out
 * of GPU memory the moment the host has seen its completion and reports whether
 * the payload had landed. Opaque; implemented in xnvmeperf_cuda.cu.
 */
struct xnvmeperf_p2pcheck;

#ifdef XNVME_BE_UPCIE_CUDA_ENABLED
/** Print the platform's ordering guarantees for peer writes into @p gpu_id. */
void
xnvmeperf_p2pcheck_print_attrs(uint32_t gpu_id);

/**
 * Load the checker into the current CUDA context and confirm it can be resident.
 * Call it after the devices are open and before any queue exists: a resident
 * CQ-mirror kernel keeps the GPU busy, and the load waits for it to go idle.
 */
int
xnvmeperf_p2pcheck_prepare(void);

/**
 * Start the checker over @p nqueues rings of @p nslots device buffers, @p bufs laid
 * out as [queue * nslots + slot], each check covering @p iosize bytes of
 * @p lba_nbytes sectors. Call it on the thread that opened the devices, whose CUDA
 * context owns the buffers, after xnvmeperf_p2pcheck_prepare().
 */
struct xnvmeperf_p2pcheck *
xnvmeperf_p2pcheck_open(uint32_t nqueues, uint32_t nslots, uint32_t iosize, uint32_t lba_nbytes,
			void **bufs);

/** Ask for @p slot of @p queue to be checked against the pattern of @p slba. */
void
xnvmeperf_p2pcheck_post(struct xnvmeperf_p2pcheck *chk, uint32_t queue, uint32_t slot,
			uint64_t slba);

/** 1 with the verdict once it is in (0 match, else the first bad offset + 1), else 0. */
int
xnvmeperf_p2pcheck_poll(struct xnvmeperf_p2pcheck *chk, uint32_t queue, uint32_t slot,
			uint32_t *verdict);

/** Stop the checker kernel; the resources stay until close(). */
void
xnvmeperf_p2pcheck_stop(struct xnvmeperf_p2pcheck *chk);

/**
 * Release the checker's resources. Its frees wait for the GPU to go idle, so
 * call it only once every queue, and with it the CQ mirror, is gone.
 */
void
xnvmeperf_p2pcheck_close(struct xnvmeperf_p2pcheck *chk);
#else
static inline void
xnvmeperf_p2pcheck_print_attrs(uint32_t XNVME_UNUSED(gpu_id))
{
}
static inline int
xnvmeperf_p2pcheck_prepare(void)
{
	return -ENOSYS;
}
static inline struct xnvmeperf_p2pcheck *
xnvmeperf_p2pcheck_open(uint32_t XNVME_UNUSED(nqueues), uint32_t XNVME_UNUSED(nslots),
			uint32_t XNVME_UNUSED(iosize), uint32_t XNVME_UNUSED(lba_nbytes),
			void **XNVME_UNUSED(bufs))
{
	errno = ENOSYS;
	return NULL;
}
static inline void
xnvmeperf_p2pcheck_post(struct xnvmeperf_p2pcheck *XNVME_UNUSED(chk), uint32_t XNVME_UNUSED(queue),
			uint32_t XNVME_UNUSED(slot), uint64_t XNVME_UNUSED(slba))
{
}
static inline int
xnvmeperf_p2pcheck_poll(struct xnvmeperf_p2pcheck *XNVME_UNUSED(chk), uint32_t XNVME_UNUSED(queue),
			uint32_t XNVME_UNUSED(slot), uint32_t *XNVME_UNUSED(verdict))
{
	return 0;
}
static inline void
xnvmeperf_p2pcheck_stop(struct xnvmeperf_p2pcheck *XNVME_UNUSED(chk))
{
}
static inline void
xnvmeperf_p2pcheck_close(struct xnvmeperf_p2pcheck *XNVME_UNUSED(chk))
{
}
#endif

#endif /* __XNVMEPERF_H */
