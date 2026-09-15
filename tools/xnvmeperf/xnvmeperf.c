#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libxnvme.h>
#include <xnvme_vcs.h>

#include "xnvmeperf.h"

/**
 * Generous per-queue allowance for uPCIe control structures (PRP pools, SQ/CQ
 * rings, and a share of the admin/sync qpairs). Sized to comfortably overshoot
 * the real per-queue overhead so the heap math stays simple.
 */
#define XNVMEPERF_HEAP_QUEUE_OVERHEAD (16UL << 20)

struct xnvmeperf_job;
/** Per-slot completion argument for the host-bounce path (job plus slot index). */
struct xnvmeperf_bref {
	struct xnvmeperf_job *job;
	uint32_t slot;
};

struct xnvmeperf_job {
	struct xnvme_dev *dev;
	struct xnvme_queue *queue;
	void *buf;
	uint32_t nsid;
	int opcode;
	size_t nbytes;
	uint64_t nblocks;
	uint16_t nlb;
	uint64_t offset;
	unsigned int seed;
	uint64_t io_completed;
	uint64_t io_failed;
	uint64_t (*peek_slba)(struct xnvmeperf_job *);
	void (*advance_slba)(struct xnvmeperf_job *);
	struct xnvmeperf_args *args;
	/* --buf-host-bounce: a ring of host buffers, one per queue slot, each copied
	 * to the GPU on completion; NULL fields when the flag is off. */
	struct xnvmeperf_gpu *gpu;
	void **bbufs;
	uint8_t *bslot_inuse;
	struct xnvmeperf_bref *brefs;
	uint32_t nslots;
	/* p2p-verify: the ring lives in GPU memory and a resident checker reads each
	 * slot at completion; bslot_inuse is then 0 free, 1 in flight, 2 checking. */
	struct xnvmeperf_p2pcheck *chk;
	uint32_t qidx;
	uint64_t *slot_slba;
	uint64_t matches;
	uint64_t mismatches;
	uint64_t first_bad_off;
	uint64_t first_bad_slba;
};

struct xnvmeperf_thread {
	uint16_t cpu;
	int njobs;
	int job_start;
	struct xnvmeperf_job *jobs;
	struct xnvmeperf_args *args;
	double elapsed;
	struct xnvme_dev **devs;
	int ndevs;
};

#ifndef XNVME_RAND_R_ENABLED
/**
 * Minimal thread-safe PRNG for platforms without rand_r() (e.g. Windows).
 *
 * Uses the glibc LCG parameters: multiplier 1103515245, increment 12345,
 * as specified in the C standard example and used by many libc implementations.
 * Returns bits [30:16] of the updated state to avoid the low-bit cycling
 * typical of LCGs, matching the behaviour of POSIX rand_r().
 *
 * @param seed  Per-caller state; must not be shared across threads
 * @return      Pseudo-random value in [0, RAND_MAX]
 */
static int
rand_r(unsigned int *seed)
{
	*seed = *seed * 1103515245 + 12345;
	return (int)((*seed >> 16) & 0x7fff);
}
#endif

static int
pin_to_cpu(int cpu)
{
#ifdef XNVME_PTHREAD_SETAFFINITY_NP_ENABLED
	cpu_set_t cpuset;

	CPU_ZERO(&cpuset);
	CPU_SET(cpu, &cpuset);
	return pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
#else
	(void)cpu;
	fprintf(stderr, "Warning: CPU pinning not supported on this platform\n");
	return 0;
#endif
}

/**
 * Returns the current sequential starting LBA without advancing.
 *
 * @param job  Job whose offset is read
 * @return     The next sequential starting LBA
 */
static uint64_t
peek_slba_seq(struct xnvmeperf_job *job)
{
	return job->offset;
}

/**
 * Advances the sequential cursor by nlb, wrapping at the end of the device.
 *
 * @param job  Job whose offset is advanced
 */
static void
advance_slba_seq(struct xnvmeperf_job *job)
{
	job->offset += job->nlb;
	if (job->offset >= job->nblocks * job->nlb) {
		job->offset = 0;
	}
}

/**
 * Returns a random starting LBA aligned to nlb within the device address space.
 *
 * Uses rand_r() for thread-safe random number generation seeded per-job.
 *
 * @param job  Job whose seed is used for random generation
 * @return     Random nlb-aligned starting LBA
 */
static uint64_t
peek_slba_rand(struct xnvmeperf_job *job)
{
	uint64_t slba = (rand_r(&job->seed) % job->nblocks) * job->nlb;
	return slba;
}

/**
 * No-op cursor advance for the random pattern, which holds no position.
 */
static void
advance_slba_noop(struct xnvmeperf_job *job)
{
	(void)job;
}

/**
 * Submits a single async IO using the job's slba selector and buffer.
 *
 * Advances the slba cursor only on a successful submit, so a failed submission
 * (e.g. -EBUSY) is retried on the same LBA.
 *
 * @param job  Job providing opcode, nsid, nlb, buffer, and slba selector
 * @param ctx  Command context obtained from the job's queue
 * @return     0 on success, negative errno on error
 */
/* A payload buffer where the run asked for it: host memory under a GPU backend
 * for the reverse split, else the backend's own heap. */
static void *
payload_alloc(const struct xnvmeperf_args *args, const struct xnvme_dev *dev)
{
	return args->buf_hostmem ? xnvme_buf_host_alloc(dev, args->iosize)
				 : xnvme_buf_alloc(dev, args->iosize);
}

static void
payload_free(const struct xnvmeperf_args *args, const struct xnvme_dev *dev, void *buf)
{
	if (args->buf_hostmem) {
		xnvme_buf_host_free(dev, buf);
	} else {
		xnvme_buf_free(dev, buf);
	}
}

static int
submit_io(struct xnvmeperf_job *job, struct xnvme_cmd_ctx *ctx)
{
	uint64_t slba = job->peek_slba(job);
	void *buf = job->buf;
	int slot = -1;
	int err;

	if (job->args->buf_host_bounce) {
		/* Take a slot whose buffer is free and whose last copy to the GPU has
		 * drained; when none is, the copy is the bottleneck, so decline the
		 * submit and let the caller poke completions before trying again. */
		for (uint32_t sl = 0; sl < job->nslots; sl++) {
			if (!job->bslot_inuse[sl] && xnvmeperf_gpu_bounce_ready(job->gpu, sl)) {
				slot = (int)sl;
				break;
			}
		}
		if (slot < 0) {
			return -EBUSY;
		}
		job->bslot_inuse[slot] = 1;
		buf = job->bbufs[slot];
		ctx->async.cb_arg = &job->brefs[slot];
	} else if (job->chk) {
		/* A free slot of the GPU ring; none free means every slot is in flight
		 * or still being checked, so decline and let the caller poke. */
		for (uint32_t sl = 0; sl < job->nslots; sl++) {
			if (!job->bslot_inuse[sl]) {
				slot = (int)sl;
				break;
			}
		}
		if (slot < 0) {
			return -EBUSY;
		}
		job->bslot_inuse[slot] = 1;
		buf = job->bbufs[slot];
		ctx->async.cb_arg = &job->brefs[slot];
	}

	ctx->cmd.common.opcode = job->opcode;
	ctx->cmd.common.nsid = job->nsid;
	ctx->cmd.nvm.nlb = job->nlb - 1;
	ctx->cmd.nvm.slba = slba;

	err = xnvme_cmd_pass(ctx, buf, (job->nbytes * job->nlb), NULL, 0);
	if (!err) {
		job->advance_slba(job);
	} else if (slot >= 0) {
		job->bslot_inuse[slot] = 0;
	}

	return err;
}

/**
 * Async IO completion callback; updates job counters and returns ctx to the queue.
 *
 * @param ctx     Completed command context
 * @param cb_arg  Pointer to the xnvmeperf_job that owns the queue
 */
static void
cb_fn(struct xnvme_cmd_ctx *ctx, void *cb_arg)
{
	struct xnvmeperf_job *job = cb_arg;

	if (xnvme_cmd_ctx_cpl_status(ctx)) {
		job->io_failed++;
	} else {
		job->io_completed++;
	}

	xnvme_queue_put_cmd_ctx(ctx->async.queue, ctx);
}

/**
 * Completion for --buf-host-bounce: on success enqueue the host-to-device copy
 * of the slot just read, then release the slot; its buffer is reusable once the
 * copy drains, which submit_io() checks via xnvmeperf_gpu_bounce_ready().
 */
static void
cb_fn_bounce(struct xnvme_cmd_ctx *ctx, void *cb_arg)
{
	struct xnvmeperf_bref *ref = cb_arg;
	struct xnvmeperf_job *job = ref->job;

	if (xnvme_cmd_ctx_cpl_status(ctx)) {
		job->io_failed++;
	} else {
		job->io_completed++;
		xnvmeperf_gpu_bounce_copy(job->gpu, ref->slot, job->bbufs[ref->slot]);
	}

	job->bslot_inuse[ref->slot] = 0;
	xnvme_queue_put_cmd_ctx(ctx->async.queue, ctx);
}

static void
cb_fn_p2pcheck(struct xnvme_cmd_ctx *ctx, void *cb_arg)
{
	struct xnvmeperf_bref *ref = cb_arg;
	struct xnvmeperf_job *job = ref->job;

	if (xnvme_cmd_ctx_cpl_status(ctx)) {
		job->io_failed++;
		job->bslot_inuse[ref->slot] = 0;
	} else {
		job->io_completed++;
		/* The completion is visible to the host now; ask the GPU to read the
		 * payload out of VRAM at this very moment and say whether it landed. */
		job->slot_slba[ref->slot] = ctx->cmd.nvm.slba;
		xnvmeperf_p2pcheck_post(job->chk, job->qidx, ref->slot, ctx->cmd.nvm.slba);
		job->bslot_inuse[ref->slot] = 2;
	}
	xnvme_queue_put_cmd_ctx(ctx->async.queue, ctx);
}

/**
 * Initializes a job for the given device and benchmark arguments.
 *
 * Derives geometry fields (nlb, nblocks, nbytes), sets the opcode and slba
 * selector based on the IO pattern, and creates the async queue. The caller
 * is responsible for allocating job->buf and calling xnvme_queue_term() on
 * cleanup.
 *
 * @param job   Job struct to initialize (must be zeroed by caller)
 * @param dev   Open xNVMe device handle
 * @param args  Benchmark arguments
 * @param seed  Initial seed for rand_r() used in random IO patterns
 * @return      0 on success, negative errno on error
 */
static int
setup_job(struct xnvmeperf_job *job, struct xnvme_dev *dev, struct xnvmeperf_args *args,
	  unsigned int seed)
{
	const struct xnvme_geo *geo = xnvme_dev_get_geo(dev);
	int err;

	job->args = args;
	job->seed = seed;
	job->dev = dev;

	job->nsid = xnvme_dev_get_nsid(job->dev);
	job->nbytes = geo->lba_nbytes;
	job->nblocks = geo->tbytes / args->iosize;
	job->nlb = args->iosize / job->nbytes;
	if (job->nlb == 0) {
		fprintf(stderr, "Error: iosize (%u) smaller than lba size (%zu)\n", args->iosize,
			job->nbytes);
		return -EINVAL;
	}

	switch (job->args->pattern) {
	case IOPATTERN_READ:
	case IOPATTERN_RANDREAD:
		job->opcode = XNVME_SPEC_NVM_OPC_READ;
		break;
	case IOPATTERN_WRITE:
	case IOPATTERN_RANDWRITE:
		job->opcode = XNVME_SPEC_NVM_OPC_WRITE;
		break;
	case IOPATTERN_VERIFY:
		// skip as opcode is set manually
		break;
	default:
		fprintf(stderr, "Error: Unsupported pattern(%d)", job->args->pattern);
		return -ENOTSUP;
	}

	switch (job->args->pattern) {
	case IOPATTERN_READ:
	case IOPATTERN_WRITE:
	case IOPATTERN_VERIFY:
		job->peek_slba = peek_slba_seq;
		job->advance_slba = advance_slba_seq;
		break;
	case IOPATTERN_RANDREAD:
	case IOPATTERN_RANDWRITE:
		job->peek_slba = peek_slba_rand;
		job->advance_slba = advance_slba_noop;
		break;
	default:
		fprintf(stderr, "Error: Unsupported pattern(%d)", job->args->pattern);
		return -ENOTSUP;
	}

	err = xnvme_queue_init(job->dev, args->qdepth, args->queue_opts, &job->queue);
	if (err) {
		xnvme_cli_perr("Failed: xnvme_queue_init()", err);
		return err;
	}
	xnvme_queue_set_cb(job->queue, args->buf_host_bounce ? cb_fn_bounce : cb_fn, job);

	return 0;
}

static void
thread_term(struct xnvmeperf_thread *thread)
{
	for (int i = 0; i < thread->ndevs; i++) {
		struct xnvmeperf_job *job = &thread->jobs[i];

		if (job->gpu) {
			xnvmeperf_gpu_bounce_close(job->gpu);
			job->gpu = NULL;
		}
		if (job->bbufs) {
			for (uint32_t sl = 0; sl < job->nslots; sl++) {
				if (job->bbufs[sl]) {
					xnvme_buf_free(job->dev, job->bbufs[sl]);
				}
			}
			free(job->bbufs);
		}
		free(job->bslot_inuse);
		free(job->brefs);
		if (job->buf) {
			payload_free(job->args, job->dev, job->buf);
		}
		if (job->queue) {
			xnvme_queue_term(job->queue);
		}
	}
	free(thread->jobs);
}

/**
 * Set up the host-bounce ring for one job: a host buffer per queue slot, a GPU
 * handle with the device buffer and copy stream, and each host buffer page-locked
 * for the copy. Freed by thread_term().
 */
static int
job_bounce_init(struct xnvmeperf_job *job, struct xnvmeperf_args *args)
{
	uint32_t n = args->qdepth;

	/* Cap the ring's in-flight staging by a byte budget: large blocks use
	 * fewer buffers (they saturate at low depth) so the heap stays under the
	 * GPU's IOMMU aperture, while small blocks keep the full queue depth. */
	{
		uint64_t cap = (128UL << 20) / args->iosize;
		if (cap < 1) {
			cap = 1;
		}
		if (n > cap) {
			n = (uint32_t)cap;
		}
	}

	job->nslots = n;
	job->bbufs = calloc(n, sizeof(*job->bbufs));
	job->bslot_inuse = calloc(n, sizeof(*job->bslot_inuse));
	job->brefs = calloc(n, sizeof(*job->brefs));
	if (!job->bbufs || !job->bslot_inuse || !job->brefs) {
		return -ENOMEM;
	}

	job->gpu = xnvmeperf_gpu_bounce_open(args->opts.gpu_id, args->iosize, n);
	if (!job->gpu) {
		return -errno;
	}

	for (uint32_t sl = 0; sl < n; sl++) {
		job->brefs[sl].job = job;
		job->brefs[sl].slot = sl;
		job->bbufs[sl] = xnvme_buf_alloc(job->dev, args->iosize);
		if (!job->bbufs[sl]) {
			return -errno;
		}
		xnvmeperf_gpu_bounce_register(job->gpu, sl, job->bbufs[sl]);
	}

	return 0;
}

static int
thread_init(struct xnvmeperf_thread *thread, struct xnvmeperf_args *args)
{
	int err;

	thread->njobs = thread->ndevs;
	thread->jobs = calloc(thread->ndevs, sizeof(struct xnvmeperf_job));
	if (!thread->jobs) {
		err = -errno;
		xnvme_cli_perr("Failed: calloc() for jobs", err);
		return err;
	}

	for (int i = 0; i < thread->ndevs; i++) {
		struct xnvmeperf_job *job = &thread->jobs[i];

		err = setup_job(job, thread->devs[(thread->job_start + i) / (int)args->nqueues],
				args, (unsigned int)(thread->cpu * 1000 + i));
		if (err) {
			xnvme_cli_perr("Failed: setup_job()", err);
			return err;
		}

		if (args->buf_host_bounce) {
			err = job_bounce_init(job, args);
			if (err) {
				xnvme_cli_perr("Failed: job_bounce_init()", err);
				return err;
			}
		} else {
			job->buf = payload_alloc(args, job->dev);
			if (!job->buf) {
				err = -errno;
				xnvme_cli_perr("Failed: xnvme_buf_alloc()", err);
				return err;
			}
		}
	}

	return err;
}

/**
 * Per-CPU benchmark thread; runs IO against the devices assigned to this thread.
 *
 * Sets up one job per device, fills queues to depth, then drives a time-bounded
 * IO loop. Elapsed time and per-job completion counters are written back into
 * the thread struct for aggregation by the main thread.
 *
 * @param arg  Pointer to xnvmeperf_thread describing this thread's assignment
 * @return     Always NULL
 */
static void *
thread_fn(void *arg)
{
	struct xnvmeperf_thread *thread = arg;
	struct xnvmeperf_args *args = thread->args;
	struct xnvme_timer timer = {0};
	uint64_t runtime_ns = (uint64_t)args->time * 1000000000ULL;
	int err;

	err = pin_to_cpu(thread->cpu);
	if (err) {
		fprintf(stderr, "Warning: failed to pin thread to CPU %" PRIu16 "\n", thread->cpu);
	}

	if (args->buf_host_bounce && xnvmeperf_gpu_set_device(args->opts.gpu_id)) {
		fprintf(stderr, "Error: could not select GPU %u on this thread\n",
			args->opts.gpu_id);
		return NULL;
	}

	// Fill each job buffer with a known pattern. xnvme_buf_fill() routes
	// transparently to device memory (CUDA/HIP) when the buffer is GPU VRAM,
	// so the GPU-backend P2P path needs no special-casing here. The bounce path
	// owns a ring of read buffers instead of one, and reads overwrite them, so
	// there is nothing to pre-fill.
	if (!args->buf_host_bounce) {
		for (int i = 0; i < thread->ndevs; i++) {
			struct xnvmeperf_job *job = &thread->jobs[i];

			err = xnvme_buf_fill(job->buf, args->iosize, "anum");
			if (err) {
				xnvme_cli_perr("Failed: xnvme_buf_fill()", err);
				return NULL;
			}
		}
	}

	xnvme_timer_start(&timer);

	// Fill all queues to depth
	for (int i = 0; i < thread->ndevs; i++) {
		struct xnvmeperf_job *job = &thread->jobs[i];

		for (uint32_t d = 0; d < args->qdepth; d++) {
			struct xnvme_cmd_ctx *ctx = xnvme_queue_get_cmd_ctx(job->queue);
			if (!ctx) {
				break;
			}

			err = submit_io(job, ctx);
			if (err) {
				xnvme_queue_put_cmd_ctx(job->queue, ctx);
				break;
			}
		}
	}

	{
		unsigned int poke_count = 0;

		while (1) {
			for (int i = 0; i < thread->ndevs; i++) {
				struct xnvmeperf_job *job = &thread->jobs[i];
				struct xnvme_cmd_ctx *ctx;

				xnvme_queue_poke(job->queue, 0);

				while ((ctx = xnvme_queue_get_cmd_ctx(job->queue))) {
					if (submit_io(job, ctx)) {
						xnvme_queue_put_cmd_ctx(job->queue, ctx);
						break;
					}
				}
			}

			if ((++poke_count & 63) == 0) {
				xnvme_timer_stop(&timer);
				if (xnvme_timer_elapsed_nsecs(&timer) >= runtime_ns) {
					break;
				}
			}
		}
	}

	for (int i = 0; i < thread->ndevs; i++) {
		xnvme_queue_drain(thread->jobs[i].queue);
		if (args->buf_host_bounce) {
			xnvmeperf_gpu_bounce_drain(thread->jobs[i].gpu);
		}
	}

	xnvme_timer_stop(&timer);
	thread->elapsed = xnvme_timer_elapsed_secs(&timer);

	return NULL;
}

static void *
dev_close_fn(void *arg)
{
	xnvme_dev_close(arg);
	return NULL;
}

static void
print_run_args(struct xnvmeperf_args *args, const char *pattern)
{
	printf("Running xnvmeperf with arguments:\n");
	printf("- Devices: [");
	for (int i = 0; i < args->ndevs; i++) {
		if (i) {
			printf(", ");
		}
		printf("%s", args->dev_uris[i]);
	}
	printf("] (total: %d)\n", args->ndevs);
	printf("- io pattern: %s\n", pattern);
	printf("- queues per device: %u\n", args->nqueues);
	printf("- queue depth: %u\n", args->qdepth);
	printf("- p2p completions: %s\n",
	       (args->queue_opts & XNVME_QUEUE_P2P_CQ_MIRROR)   ? "mirrored"
	       : (args->queue_opts & XNVME_QUEUE_P2P_UNORDERED) ? "unordered"
								: "flushed");
	printf("- sq in host memory: %s\n",
	       (args->queue_opts & XNVME_QUEUE_SQ_HOSTMEM) ? "yes" : "no");
	printf("- buf host bounce (read to host, copy to GPU): %s\n",
	       args->buf_host_bounce ? "yes" : "no");
	printf("- buf in host memory (reverse split): %s\n", args->buf_hostmem ? "yes" : "no");
	if (args->opts.homi_id) {
		printf("- served by homi: %u\n", args->opts.homi_id);
	}
	printf("- io size: %u\n", args->iosize);
	printf("- runtime: %u\n", args->time);

	if (args->ncpus) {
		printf("- CPUs: [");
		for (int i = 0; i < args->ncpus; i++) {
			if (i) {
				printf(", ");
			}
			printf("%" PRIu16, args->cpus[i]);
		}
		printf("] (total: %" PRIu16 ")\n", args->ncpus);
	}
}

/**
 * Print a performance results table.
 *
 * @param title    Header string (e.g. "xnvmeperf" or "xnvmeperf cuda-run")
 * @param elapsed  Elapsed time in seconds
 * @param uris     Device URI strings
 * @param ndevs    Number of devices
 * @param iops     Per-device IOPS
 * @param mibps    Per-device throughput in MiB/s
 * @param failed   Per-device failed I/O count
 * @param cpus     Per-device CPU strings, or NULL to omit the CPUs column
 */
static void
print_perf_results(const char *title, double elapsed, const char **uris, int ndevs,
		   const double *iops, const double *mibps, const uint64_t *failed,
		   const char **cpus)
{
	double total_iops = 0, total_mibps = 0;
	uint64_t total_failed = 0;
	const char *vcs = xnvme_ver_vcs();

	printf("\n");
	printf("====================================================================\n");
	printf(" %s (v%d.%d.%d%s%s) (elapsed: %.2fs)\n", title, xnvme_ver_major(),
	       xnvme_ver_minor(), xnvme_ver_patch(), *vcs ? " - " : "", vcs, elapsed);
	printf("====================================================================\n");
	if (cpus) {
		printf(" %-20s  %6s  %12s %10s %8s\n", "Device", "CPUs", "IOPS", "MiB/s",
		       "Failed");
	} else {
		printf(" %-20s  %12s %10s %8s\n", "Device", "IOPS", "MiB/s", "Failed");
	}

	for (int d = 0; d < ndevs; d++) {
		if (cpus) {
			printf(" %-20s  %6s  %12.2f %10.2f %8lu\n", uris[d], cpus[d], iops[d],
			       mibps[d], (unsigned long)failed[d]);
		} else {
			printf(" %-20s  %12.2f %10.2f %8lu\n", uris[d], iops[d], mibps[d],
			       (unsigned long)failed[d]);
		}

		total_iops += iops[d];
		total_mibps += mibps[d];
		total_failed += failed[d];
	}

	printf("--------------------------------------------------------------------\n");
	if (cpus) {
		printf(" %-29s %12.2f %10.2f %8lu\n", "Total:", total_iops, total_mibps,
		       (unsigned long)total_failed);
	} else {
		printf(" %-21s %12.2f %10.2f %8lu\n", "Total:", total_iops, total_mibps,
		       (unsigned long)total_failed);
	}
	printf("====================================================================\n");
}

void
print_intermediate_header(void)
{
	printf("Time,Batches,IOPS,MiB/s\n");
}

void
print_intermediate_result(double elapsed, uint64_t completed, uint32_t iosize)
{
	double iops, mibs;

	if (elapsed <= 0.0) {
		return;
	}

	iops = (double)completed / elapsed;
	mibs = ((double)completed * (double)iosize) / (elapsed * 1024.0 * 1024.0);

	printf("%.2f,1,%.2f,%.2f\n", elapsed, iops, mibs);
}

static void
print_intermediate_thread_result(struct xnvmeperf_thread *threads, struct xnvmeperf_args *args,
				 double elapsed)
{
	uint64_t completed = 0;

	for (int t = 0; t < args->ncpus; t++) {
		struct xnvmeperf_thread *thread = &threads[t];

		for (int j = 0; j < thread->njobs; j++) {
			completed += thread->jobs[j].io_completed;
		}
	}

	print_intermediate_result(elapsed, completed, args->iosize);
}

static void
print_results(struct xnvmeperf_thread *threads, struct xnvmeperf_args *args)
{
	double *iops, *mibps, elapsed = 0;
	uint64_t *failed;
	char **cpus_bufs;
	const char **cpus;

	for (int t = 0; t < args->ncpus; t++) {
		if (threads[t].elapsed > elapsed) {
			elapsed = threads[t].elapsed;
		}
	}

	iops = calloc(args->ndevs, sizeof(*iops));
	mibps = calloc(args->ndevs, sizeof(*mibps));
	failed = calloc(args->ndevs, sizeof(*failed));
	cpus_bufs = calloc(args->ndevs, sizeof(*cpus_bufs));
	cpus = calloc(args->ndevs, sizeof(*cpus));
	if (!iops || !mibps || !failed || !cpus_bufs || !cpus) {
		goto done;
	}

	for (int d = 0; d < args->ndevs; d++) {
		uint64_t completed = 0;
		int cpus_len = 0;

		cpus_bufs[d] = calloc(256, 1);
		if (!cpus_bufs[d]) {
			goto done;
		}

		for (int t = 0; t < args->ncpus; t++) {
			struct xnvmeperf_thread *thread = &threads[t];

			for (int j = 0; j < thread->njobs; j++) {
				if (strcmp(args->dev_uris[(thread->job_start + j) /
							  (int)args->nqueues],
					   args->dev_uris[d]) != 0) {
					continue;
				}

				completed += thread->jobs[j].io_completed;
				failed[d] += thread->jobs[j].io_failed;

				if (cpus_len > 0) {
					cpus_len += snprintf(cpus_bufs[d] + cpus_len,
							     256 - cpus_len, ",");
				}
				cpus_len += snprintf(cpus_bufs[d] + cpus_len, 256 - cpus_len,
						     "%" PRIu16, thread->cpu);
			}
		}

		iops[d] = (double)completed / elapsed;
		mibps[d] =
			((double)completed * (double)args->iosize) / (elapsed * 1024.0 * 1024.0);
		cpus[d] = cpus_bufs[d];
	}

	print_perf_results("xnvmeperf", elapsed, args->dev_uris, args->ndevs, iops, mibps, failed,
			   cpus);

done:
	if (cpus_bufs) {
		for (int i = 0; i < args->ndevs; i++) {
			free(cpus_bufs[i]);
		}
	}
	free(cpus_bufs);
	free(cpus);
	free(failed);
	free(mibps);
	free(iops);
}

/**
 * Open all devices in args and derive their geometry.
 *
 * devs must be a calloc'd array of args->ndevs null-initialised pointers.
 * On failure all successfully opened devices are closed and their entries
 * are set to NULL.
 *
 * @param args  Benchmark arguments with dev_uris, ndevs, and opts
 * @param devs  Caller-allocated array of ndevs device pointers (zero-initialised)
 * @return 0 on success, negative errno on error
 */
static int
xnvmeperf_open_devs(struct xnvmeperf_args *args, struct xnvme_dev **devs)
{
	int err;

	for (int i = 0; i < args->ndevs; i++) {
		devs[i] = xnvme_dev_open(args->dev_uris[i], &args->opts);
		if (!devs[i]) {
			err = -errno;
			fprintf(stderr, "Failed: xnvme_dev_open(%s): err(%d)\n", args->dev_uris[i],
				err);
			goto close;
		}
		err = xnvme_dev_derive_geo(devs[i]);
		if (err) {
			xnvme_cli_perr("Failed: xnvme_dev_derive_geo()", err);
			goto close;
		}
	}
	return 0;

close:
	for (int i = 0; i < args->ndevs; i++) {
		if (devs[i]) {
			xnvme_dev_close(devs[i]);
			devs[i] = NULL;
		}
	}
	return err;
}

/**
 * Runs a multi-threaded benchmark against all configured devices.
 *
 * Opens all devices, distributes them across CPU threads, and runs a
 * time-bounded async IO loop on each thread. Prints aggregated results
 * on completion.
 *
 * @param args  Benchmark arguments including devices, CPU mask, pattern, and timing
 * @return      0 on success, negative errno on error
 */
static int
xnvmeperf_run(struct xnvmeperf_args *args)
{
	struct xnvmeperf_thread *threads;
	struct xnvme_dev **devs;
	pthread_t *tids, *close_tids;
	int total_jobs, err;

	// Pre-open all devices, as they can only be opened once.
	devs = calloc(args->ndevs, sizeof(*devs));
	if (!devs) {
		err = -errno;
		xnvme_cli_perr("Failed: calloc() for devs", err);
		return err;
	}

	err = xnvmeperf_open_devs(args, devs);
	if (err) {
		free(devs);
		return err;
	}

	total_jobs = args->ndevs * (int)args->nqueues;

	// Underprovisioning: more threads than queues means threads would share a
	// queue, which is not safe. Require at least one queue per thread.
	if (args->ncpus > total_jobs) {
		fprintf(stderr,
			"Error: --cpumask or --cpulist specifies %" PRIu16 " threads but only %d "
			"queue(s) (%d device(s) x %d queue(s) each); "
			"increase --nqueues so every thread has its own queue\n",
			args->ncpus, total_jobs, args->ndevs, args->nqueues);
		err = -EINVAL;
		goto close_devs;
	}

	threads = calloc(args->ncpus, sizeof(*threads));
	tids = calloc(args->ncpus, sizeof(*tids));
	if (!threads || !tids) {
		err = -ENOMEM;
		xnvme_cli_perr("Failed: calloc()", err);
		goto close_devs;
	}

	// Distribute job slots evenly across threads; overflow goes to the first
	// few threads one slot at a time.
	for (int i = 0; i < args->ncpus; i++) {
		int base = total_jobs / args->ncpus;
		int extra = total_jobs % args->ncpus;
		int start = i * base + (i < extra ? i : extra);
		int count = base + (i < extra ? 1 : 0);

		threads[i].cpu = args->cpus[i];
		threads[i].args = args;
		threads[i].ndevs = count;
		threads[i].job_start = start;
		threads[i].devs = devs;

		err = thread_init(&threads[i], args);
		if (err) {
			fprintf(stderr, "Failed: thread_init() for thread: %d, err: %d\n", i, err);
			goto close_devs;
		}
	}

	for (int i = 0; i < args->ncpus; i++) {
		err = pthread_create(&tids[i], NULL, thread_fn, &threads[i]);
		if (err) {
			xnvme_cli_perr("Failed: pthread_create()", err);
			args->ncpus = i;
			break;
		}
	}

	if (args->report_freq != 0.0) {
		uint64_t report_freq_ns = (uint64_t)(args->report_freq * 1000000000.0);
		uint64_t runtime_ns = (uint64_t)args->time * 1000000000ULL;
		uint64_t deadline = report_freq_ns;
		struct xnvme_timer timer = {0};

		xnvme_timer_start(&timer);
		print_intermediate_header();

		while (1) {
			struct timespec ts;
			uint64_t elapsed;

			xnvme_timer_stop(&timer);
			elapsed = xnvme_timer_elapsed_nsecs(&timer);
			if (elapsed >= runtime_ns) {
				break;
			}
			if (elapsed >= deadline) {
				print_intermediate_thread_result(threads, args,
								 (double)elapsed / 1000000000.0);
				deadline += report_freq_ns;
				continue;
			}

			ts.tv_sec = (time_t)((deadline - elapsed) / 1000000000ULL);
			ts.tv_nsec = (long)((deadline - elapsed) % 1000000000ULL);
			nanosleep(&ts, NULL);
		}
	}

	for (int i = 0; i < args->ncpus; i++) {
		pthread_join(tids[i], NULL);
	}

	print_results(threads, args);

	for (int i = 0; i < args->ncpus; i++) {
		thread_term(&threads[i]);
	}

close_devs:
	close_tids = calloc(args->ndevs, sizeof(*close_tids));

	// We use threads to close the devices, because it took a long time
	// to close each, so this is to speed up the process.
	if (close_tids) {
		for (int i = 0; i < args->ndevs; i++) {
			err = pthread_create(&close_tids[i], NULL, dev_close_fn, devs[i]);
			if (err) {
				// Failed creating thread, wait for all existing threads to finish
				// and close manually
				xnvme_cli_perr("Failed: pthread_create()", err);
				for (int j = 0; j < i; j++) {
					pthread_join(close_tids[j], NULL);
				}
				free(close_tids);
				goto failed_pthread_close;
			}
		}
		for (int i = 0; i < args->ndevs; i++) {
			pthread_join(close_tids[i], NULL);
		}
		free(close_tids);
	} else {
failed_pthread_close:
		for (int i = 0; i < args->ndevs; i++) {
			xnvme_dev_close(devs[i]);
		}
	}
	free(devs);
	free(threads);
	free(tids);

	return err;
}

/**
 * Fill `buf` with a verifiable per-LBA pattern.
 *
 * Each sector is filled with an "anum" background and then stamped with its
 * absolute LBA number in the first 8 bytes. This allows readback verification
 * to confirm both data integrity and that the correct sector was returned.
 *
 * @param buf     Buffer to fill; must be at least `nbytes` bytes
 * @param nbytes  Total buffer size in bytes (must equal nlb * lba_size)
 * @param slba    Starting LBA of the first sector in the buffer
 * @param nlb     Number of logical blocks in the buffer
 */
int
fill_pattern(void *buf, size_t nbytes, uint64_t slba, uint16_t nlb)
{
	size_t lba_size = nbytes / nlb;
	int err;

	for (uint16_t i = 0; i < nlb; i++) {
		uint8_t *p = (uint8_t *)buf + i * lba_size;
		uint64_t lba = slba + i;

		err = xnvme_buf_fill(p, lba_size, "anum");
		if (err) {
			xnvme_cli_perr("xnvme_buf_fill()", err);
			return err;
		}

		err = xnvme_buf_memcpy(p, &lba, sizeof(lba));
		if (err) {
			xnvme_cli_perr("xnvme_buf_memcpy()", err);
			return err;
		}
	}
	return 0;
}

/**
 * Verifies data integrity by writing and reading back a known pattern on each device.
 *
 * For each device: writes args->count sequential IOs with a per-LBA pattern,
 * then reads them back and compares against the expected pattern. Both phases
 * run with effective queue depth 1 to ensure buffer ordering correctness.
 *
 * @param args  Benchmark arguments
 * @return      0 on success, negative errno on the first device error
 */
static int
xnvmeperf_verify(struct xnvmeperf_args *args)
{
	struct xnvme_dev **devs;
	uint32_t nios = args->count;
	int err;

	printf("\nxnvmeperf verify: iosize=%u, nios=%d\n", args->iosize, nios);
	printf("====================================================================\n");

	devs = calloc(args->ndevs, sizeof(*devs));
	if (!devs) {
		return -ENOMEM;
	}

	err = xnvmeperf_open_devs(args, devs);
	if (err) {
		free(devs);
		return err;
	}

	err = 0;
	for (int d = 0; d < args->ndevs; d++) {
		struct xnvme_dev *dev = devs[d];
		struct xnvmeperf_job job = {0};
		void *write_buf = NULL, *read_buf = NULL, *expect_buf = NULL;
		uint64_t mismatches = 0;

		err = setup_job(&job, dev, args, 1);
		if (err) {
			xnvme_cli_perr("Failed: setup_job()", err);
			goto next_dev;
		}

		write_buf = payload_alloc(args, dev);
		read_buf = payload_alloc(args, dev);
		expect_buf = malloc(args->iosize);
		if (!write_buf || !read_buf || !expect_buf) {
			err = -errno;
			xnvme_cli_perr("Failed: buffer allocation", err);
			goto next_dev;
		}

		// Phase 1: Write known patterns
		job.opcode = XNVME_SPEC_NVM_OPC_WRITE;
		job.buf = write_buf;
		job.offset = 0;
		job.io_completed = 0;
		job.io_failed = 0;

		for (int i = 0; i < nios; i++) {
			uint64_t slba = job.offset;

			err = fill_pattern(write_buf, args->iosize, slba, job.nlb);
			if (err) {
				fprintf(stderr, "Failed: fill_pattern() at IO %d, err: %d\n", i,
					err);
				break;
			}

			struct xnvme_cmd_ctx *ctx = xnvme_queue_get_cmd_ctx(job.queue);
			while (!ctx) {
				xnvme_queue_poke(job.queue, 0);
				ctx = xnvme_queue_get_cmd_ctx(job.queue);
			}

			err = submit_io(&job, ctx);
			if (err) {
				xnvme_queue_put_cmd_ctx(job.queue, ctx);
				fprintf(stderr, "Failed: write submit at IO %d\n", i);
				break;
			}

			// Wait for this write to complete before overwriting write_buf
			// with the next LBA's pattern; without this, all in-flight DMA
			// transfers read from whatever write_buf contains at completion time
			while (job.io_completed + job.io_failed < (uint64_t)(i + 1)) {
				xnvme_queue_poke(job.queue, 0);
			}
		}
		xnvme_queue_drain(job.queue);

		if (job.io_failed > 0) {
			fprintf(stderr, " %-20s  FAIL: %lu write errors\n", args->dev_uris[d],
				(unsigned long)job.io_failed);
			goto next_dev;
		}

		// Phase 2: Read back and verify
		job.opcode = XNVME_SPEC_NVM_OPC_READ;
		job.buf = read_buf;
		job.offset = 0;
		job.io_completed = 0;
		job.io_failed = 0;

		for (int i = 0; i < nios; i++) {
			uint64_t slba;
			size_t diff = 0;

			err = xnvme_buf_clear(read_buf, args->iosize);
			if (err) {
				fprintf(stderr, "Failed: xnvme_buf_clear() at IO %d, err: %d\n", i,
					err);
				break;
			}

			struct xnvme_cmd_ctx *ctx = xnvme_queue_get_cmd_ctx(job.queue);
			while (!ctx) {
				xnvme_queue_poke(job.queue, 0);
				ctx = xnvme_queue_get_cmd_ctx(job.queue);
			}

			slba = job.offset;

			err = submit_io(&job, ctx);
			if (err) {
				xnvme_queue_put_cmd_ctx(job.queue, ctx);
				fprintf(stderr, "Failed: read submit at IO %d\n", i);
				break;
			}

			while (job.io_completed + job.io_failed < (uint64_t)(i + 1)) {
				xnvme_queue_poke(job.queue, 0);
			}

			err = fill_pattern(expect_buf, args->iosize, slba, job.nlb);
			if (err) {
				fprintf(stderr, "Failed: fill_pattern() at IO %d, err: %d\n", i,
					err);
				break;
			}

			err = xnvme_buf_diff(expect_buf, read_buf, args->iosize, &diff);
			if (err) {
				fprintf(stderr, "Failed: xnvme_buf_diff() at IO %d, err: %d\n", i,
					err);
				break;
			}
			if (diff) {
				fprintf(stderr, "  MISMATCH at slba=%lu (IO %d)\n",
					(unsigned long)slba, i);
				mismatches++;
			}
		}
		xnvme_queue_drain(job.queue);

		printf(" %-20s  verified %d IOs, %lu mismatches, %lu failed\n", args->dev_uris[d],
		       nios, (unsigned long)mismatches, (unsigned long)job.io_failed);

next_dev:
		if (write_buf) {
			payload_free(args, dev, write_buf);
		}
		if (read_buf) {
			payload_free(args, dev, read_buf);
		}
		free(expect_buf);
		if (job.queue) {
			xnvme_queue_term(job.queue);
		}
	}

	printf("====================================================================\n");

	for (int i = 0; i < args->ndevs; i++) {
		xnvme_dev_close(devs[i]);
	}
	free(devs);
	return err;
}

static int
xnvmeperf_cuda_run(struct xnvmeperf_args *args)
{
	struct xnvme_dev **devs;
	uint64_t *rounds_per_dev, *failed_per_dev;
	double elapsed_s;
	float elapsed_ms = 0;
	int err;

	devs = calloc(args->ndevs, sizeof(*devs));
	if (!devs) {
		err = -errno;
		xnvme_cli_perr("Failed: calloc() for devs", err);
		return err;
	}

	err = xnvmeperf_open_devs(args, devs);
	if (err) {
		free(devs);
		return err;
	}

	rounds_per_dev = calloc(args->ndevs, sizeof(*rounds_per_dev));
	failed_per_dev = calloc(args->ndevs, sizeof(*failed_per_dev));

	if (!rounds_per_dev || !failed_per_dev) {
		err = -ENOMEM;
		xnvme_cli_perr("Failed: calloc()", err);
		free(rounds_per_dev);
		free(failed_per_dev);
		goto close_devs;
	}

	err = xnvmeperf_cuda_run_io(devs, args, rounds_per_dev, failed_per_dev, &elapsed_ms);
	if (err) {
		xnvme_cli_perr("Failed: xnvmeperf_cuda_run_io()", err);
		free(rounds_per_dev);
		free(failed_per_dev);
		goto close_devs;
	}

	elapsed_s = elapsed_ms / 1000.0;
	{
		double iops[args->ndevs], mibps[args->ndevs];

		for (int i = 0; i < args->ndevs; i++) {
			double total_ios = (double)rounds_per_dev[i] * args->qdepth;
			iops[i] = total_ios / elapsed_s;
			mibps[i] = (total_ios * args->iosize) / (elapsed_s * 1024.0 * 1024.0);
		}
		print_perf_results("xnvmeperf cuda-run", elapsed_s, args->dev_uris, args->ndevs,
				   iops, mibps, failed_per_dev, NULL);
	}

	free(rounds_per_dev);
	free(failed_per_dev);

close_devs:
	for (int i = 0; i < args->ndevs; i++) {
		xnvme_dev_close(devs[i]);
	}
	free(devs);
	return err;
}

static int
xnvmeperf_cuda_verify(struct xnvmeperf_args *args)
{
	struct xnvme_dev **devs;
	int err;

	devs = calloc(args->ndevs, sizeof(*devs));
	if (!devs) {
		err = -errno;
		xnvme_cli_perr("Failed: calloc() for devs", err);
		return err;
	}

	err = xnvmeperf_open_devs(args, devs);
	if (err) {
		free(devs);
		return err;
	}

	printf("\nxnvmeperf cuda-verify: iosize: %u, qdepth: %u, nqueues: %u\n", args->iosize,
	       args->qdepth, args->nqueues);
	printf("====================================================================\n");

	err = xnvmeperf_cuda_verify_io(devs, args);
	if (err) {
		xnvme_cli_perr("Failed: xnvmeperf_cuda_verify_io()", err);
	}

	printf("====================================================================\n");

	for (int i = 0; i < args->ndevs; i++) {
		xnvme_dev_close(devs[i]);
	}
	free(devs);
	return err;
}

static enum iopattern
str_to_iopattern(const char *name)
{
	static const struct {
		const char *name;
		enum iopattern pat;
	} patterns[] = {
		{"read", IOPATTERN_READ},
		{"write", IOPATTERN_WRITE},
		{"randread", IOPATTERN_RANDREAD},
		{"randwrite", IOPATTERN_RANDWRITE},
	};

	for (size_t i = 0; i < sizeof(patterns) / sizeof(*patterns); i++) {
		if (strcmp(name, patterns[i].name) == 0) {
			return patterns[i].pat;
		}
	}
	return 0;
}

/**
 * Parse the subset of CLI arguments common to all sub-commands: devices, iosize, and opts.
 *
 * @return 0 on success, negative errno on validation failure
 */
static int
parse_common_args(struct xnvme_cli *cli, struct xnvmeperf_args *args)
{
	int err = 0;

	args->ndevs = cli->args.posn_count;
	if (args->ndevs <= 0) {
		err = -EINVAL;
		xnvme_cli_perr("Error: at least one device URI is required", err);
		return err;
	}
	args->dev_uris = cli->args.posn;

	args->iosize = cli->args.iosize;
	if (!args->iosize || !xnvme_is_pow2(args->iosize)) {
		err = -EINVAL;
		xnvme_cli_perr("Error: --iosize must be a power of 2", err);
		return err;
	}

	args->opts = xnvme_opts_default();
	xnvme_cli_to_opts(cli, &args->opts);
	args->queue_opts = cli->args.p2p_cq_mirror ? XNVME_QUEUE_P2P_CQ_MIRROR : 0;
	args->queue_opts |= cli->args.sq_hostmem ? XNVME_QUEUE_SQ_HOSTMEM : 0;
	args->queue_opts |= cli->args.p2p_unordered ? XNVME_QUEUE_P2P_UNORDERED : 0;
	if (cli->args.p2p_unordered && cli->args.p2p_cq_mirror) {
		err = -EINVAL;
		xnvme_cli_perr("Error: --p2p-unordered declines the ordering --p2p-cq-mirror "
			       "provides; pick one",
			       err);
		return err;
	}
	/* HIP has no flush; its ordered default is the mirror, so say so and size
	 * the GPU heap for it (the banner and derive_heap_sizes() read queue_opts). */
	if (args->opts.be && strstr(args->opts.be, "hip") && !cli->args.p2p_unordered) {
		args->queue_opts |= XNVME_QUEUE_P2P_CQ_MIRROR;
	}
	args->buf_hostmem = cli->args.buf_hostmem;
	return err;
}

/**
 * Parse the full set of run sub-command arguments into @p args.
 * Calls parse_common_args() then adds iopattern, qdepth, and runtime.
 *
 * @return 0 on success, negative errno on validation failure
 */
static int
parse_run_args(struct xnvme_cli *cli, struct xnvmeperf_args *args)
{
	int err = 0;

	err = parse_common_args(cli, args);
	if (err) {
		return err;
	}

	if (!cli->args.iopattern) {
		err = -EINVAL;
		xnvme_cli_perr("Error: --iopattern is required", err);
		return err;
	}
	args->pattern = str_to_iopattern(cli->args.iopattern);
	if (!args->pattern) {
		err = -EINVAL;
		fprintf(stderr, "Error: unknown iopattern '%s': err(%d)\n", cli->args.iopattern,
			err);
		return err;
	}

	args->qdepth = cli->args.qdepth;
	if (!args->qdepth || !xnvme_is_pow2(args->qdepth)) {
		err = -EINVAL;
		xnvme_cli_perr("Error: --qdepth must be a power of 2", err);
		return err;
	}

	args->time = cli->args.runtime;
	if (!args->time) {
		err = -EINVAL;
		xnvme_cli_perr("Error: --time must be a positive integer", err);
		return err;
	}

	args->report_freq = cli->args.report_freq;
	if (args->report_freq != 0.0 && args->report_freq < 0.001) {
		err = -EINVAL;
		xnvme_cli_perr("Error: --report-freq must be 0 or at least 0.001", err);
		return err;
	}

	args->nqueues = cli->args.nqueues ? cli->args.nqueues : 1;

	args->ncpus = cli->args.ncpus;
	args->cpus = cli->args.cpus;

	args->buf_host_bounce = cli->args.buf_host_bounce;
	if (args->buf_host_bounce && args->opts.be &&
	    (strstr(args->opts.be, "cuda") || strstr(args->opts.be, "hip"))) {
		err = -EINVAL;
		fprintf(stderr,
			"Error: --buf-host-bounce reads into host memory and copies to the GPU;"
			" use a host backend (e.g. --be upcie), not '%s': err(%d)\n",
			args->opts.be, err);
		return err;
	}
	if (args->buf_hostmem && args->buf_host_bounce) {
		err = -EINVAL;
		fprintf(stderr,
			"Error: --buf-hostmem and --buf-host-bounce are exclusive: err(%d)\n",
			err);
		return err;
	}
	if (args->buf_hostmem &&
	    !(args->opts.be && (strstr(args->opts.be, "cuda") || strstr(args->opts.be, "hip")))) {
		err = -EINVAL;
		fprintf(stderr,
			"Error: --buf-hostmem keeps the payloads in host memory under a GPU "
			"backend;"
			" use --be upcie-cuda or upcie-hip, not '%s': err(%d)\n",
			args->opts.be ? args->opts.be : "(default)", err);
		return err;
	}

	return err;
}

/**
 * Derive uPCIe host/device heap sizes from the workload and write them into
 * args->opts. Deliberately overshoots to keep the estimate simple: every queue
 * gets a fixed control-structure slab, and data buffers land on the host heap
 * for the host backends or on the device heap for the CUDA backend (qdepth
 * buffers per queue vs one reused host buffer).
 */
static void
derive_heap_sizes(struct xnvmeperf_args *args)
{
	int is_gpu =
		args->opts.be && (strstr(args->opts.be, "cuda") || strstr(args->opts.be, "hip"));
	size_t nq = args->nqueues ? args->nqueues : 1;
	size_t qd = args->qdepth ? args->qdepth : 1;
	size_t queues = (size_t)args->ndevs * nq;
	size_t control = queues * XNVMEPERF_HEAP_QUEUE_OVERHEAD;
	size_t iosize = args->iosize;

	size_t bounce_cap = (128UL << 20) / iosize;
	if (bounce_cap < 1) {
		bounce_cap = 1;
	}
	size_t data_bufs = args->buf_host_bounce ? (qd < bounce_cap ? qd : bounce_cap) : 1;
	args->opts.host_heap_size =
		control + ((is_gpu && !args->buf_hostmem) ? 0 : queues * data_bufs * iosize);
	if (args->buf_host_bounce) {
		/* The bounce ring's real footprint runs well above the nominal byte
		 * sum (per-buffer heap overhead), and the served homi shares the
		 * hugepages, so size the heap with headroom: a fixed control slab
		 * plus three times the ring. Kept under the ~4 GiB point where the
		 * DMA mapping collides in the GPU's IOMMU aperture, which is what
		 * caps the usable block size. */
		args->opts.host_heap_size =
			(size_t)queues * (64UL << 20) + 3UL * queues * data_bufs * iosize;
	}
	printf("- host_heap_size: %zu bytes\n", args->opts.host_heap_size);

	if (is_gpu) {
		args->opts.device_heap_size = control + queues * qd * iosize;
		printf("- device_heap_size: %zu bytes\n", args->opts.device_heap_size);
	}
}

static int
sub_run(struct xnvme_cli *cli)
{
	struct xnvmeperf_args args = {0};
	int err;

	err = parse_run_args(cli, &args);
	if (err) {
		return err;
	}

	if (!args.ncpus) {
		err = -EINVAL;
		xnvme_cli_perr("Error: Either --cpumask or --cpulist must be given", err);
		return err;
	}

	print_run_args(&args, cli->args.iopattern);
	derive_heap_sizes(&args);

	err = xnvmeperf_run(&args);
	return err;
}

static int
sub_verify(struct xnvme_cli *cli)
{
	struct xnvmeperf_args args = {0};
	int err;

	err = parse_common_args(cli, &args);
	if (err) {
		return err;
	}

	if (!cli->args.count) {
		err = -EINVAL;
		xnvme_cli_perr("Error: --count must be a positive integer", err);
		return err;
	}
	args.count = (uint32_t)cli->args.count;

	args.qdepth = 1;
	args.pattern = IOPATTERN_VERIFY;

	derive_heap_sizes(&args);

	return xnvmeperf_verify(&args);
}

static int
sub_cuda_run(struct xnvme_cli *cli)
{
	struct xnvmeperf_args args = {0};
	int err;

	err = parse_run_args(cli, &args);
	if (err) {
		return err;
	}

	if (!args.opts.be) {
		args.opts.be = "upcie-cuda";
	} else if (strcmp(args.opts.be, "upcie-cuda") != 0) {
		err = -EINVAL;
		fprintf(stderr, "Error: cuda-run requires --be upcie-cuda, got '%s': err(%d)\n",
			args.opts.be, err);
		return err;
	}

	print_run_args(&args, cli->args.iopattern);
	derive_heap_sizes(&args);

	return xnvmeperf_cuda_run(&args);
}

static uint64_t
peek_slba_window(struct xnvmeperf_job *job)
{
	return job->offset;
}

/* A sequential cursor wrapping at the pre-written window, job->nblocks LBAs */
static void
advance_slba_window(struct xnvmeperf_job *job)
{
	job->offset += job->nlb;
	if (job->offset >= job->nblocks) {
		job->offset = 0;
	}
}

static int
job_p2pcheck_init(struct xnvmeperf_job *job, struct xnvmeperf_args *args)
{
	uint32_t n = args->qdepth;

	job->nslots = n;
	job->bbufs = calloc(n, sizeof(*job->bbufs));
	job->bslot_inuse = calloc(n, sizeof(*job->bslot_inuse));
	job->brefs = calloc(n, sizeof(*job->brefs));
	job->slot_slba = calloc(n, sizeof(*job->slot_slba));
	if (!job->bbufs || !job->bslot_inuse || !job->brefs || !job->slot_slba) {
		return -ENOMEM;
	}
	for (uint32_t sl = 0; sl < n; sl++) {
		job->brefs[sl].job = job;
		job->brefs[sl].slot = sl;
		job->bbufs[sl] = xnvme_buf_alloc(job->dev, args->iosize);
		if (!job->bbufs[sl]) {
			return -errno;
		}
	}
	return 0;
}

static void
job_p2pcheck_term(struct xnvmeperf_job *job)
{
	if (job->bbufs) {
		for (uint32_t sl = 0; sl < job->nslots; sl++) {
			if (job->bbufs[sl]) {
				xnvme_buf_free(job->dev, job->bbufs[sl]);
			}
		}
		free(job->bbufs);
	}
	free(job->bslot_inuse);
	free(job->brefs);
	free(job->slot_slba);
	if (job->queue) {
		xnvme_queue_term(job->queue);
	}
}

static double
p2p_now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void
print_p2pverify_results(const struct xnvmeperf_args *args, const struct xnvmeperf_job *jobs,
			double elapsed)
{
	uint64_t t_done = 0, t_failed = 0, t_match = 0, t_mis = 0;
	const char *vcs = xnvme_ver_vcs();
	double t_iops = 0;

	printf("\n");
	printf("====================================================================\n");
	printf(" xnvmeperf p2p-verify (v%d.%d.%d%s%s) (elapsed: %.2fs)\n", xnvme_ver_major(),
	       xnvme_ver_minor(), xnvme_ver_patch(), *vcs ? " - " : "", vcs, elapsed);
	printf("====================================================================\n");
	printf(" %-20s %10s %8s %10s %10s %9s %13s\n", "Device", "Completed", "Failed", "Matches",
	       "Mismatches", "FirstBad", "IOPS");
	for (int d = 0; d < args->ndevs; d++) {
		const struct xnvmeperf_job *j = &jobs[d];
		double iops = elapsed > 0 ? (double)j->io_completed / elapsed : 0;
		char fb[24] = "-";

		if (j->mismatches) {
			snprintf(fb, sizeof(fb), "%lu", (unsigned long)j->first_bad_off);
		}
		printf(" %-20s %10lu %8lu %10lu %10lu %9s %13.2f\n", args->dev_uris[d],
		       (unsigned long)j->io_completed, (unsigned long)j->io_failed,
		       (unsigned long)j->matches, (unsigned long)j->mismatches, fb, iops);
		t_done += j->io_completed;
		t_failed += j->io_failed;
		t_match += j->matches;
		t_mis += j->mismatches;
		t_iops += iops;
	}
	printf("--------------------------------------------------------------------\n");
	printf(" %-20s %10lu %8lu %10lu %10lu %9s %13.2f\n", "Total:", (unsigned long)t_done,
	       (unsigned long)t_failed, (unsigned long)t_match, (unsigned long)t_mis, "-", t_iops);
	printf("====================================================================\n");
}

/* Tally the verdicts that are in; returns 1 while any slot is still in flight or
 * being checked. */
static int
p2p_collect(struct xnvmeperf_job *job, struct xnvmeperf_p2pcheck *chk, const char *uri,
	    uint64_t *reported)
{
	int busy = 0;

	for (uint32_t sl = 0; sl < job->nslots; sl++) {
		uint32_t verdict;

		if (job->bslot_inuse[sl] == 2 &&
		    xnvmeperf_p2pcheck_poll(chk, job->qidx, sl, &verdict)) {
			if (!verdict) {
				job->matches++;
			} else {
				if (!job->mismatches) {
					job->first_bad_off = verdict - 1;
					job->first_bad_slba = job->slot_slba[sl];
				}
				job->mismatches++;
				if ((*reported)++ < 32) {
					fprintf(stderr,
						"MISMATCH dev=%s slot=%u slba=%lu off=%u\n", uri,
						sl, (unsigned long)job->slot_slba[sl],
						verdict - 1);
				}
			}
			job->bslot_inuse[sl] = 0;
		}
		if (job->bslot_inuse[sl]) {
			busy = 1;
		}
	}
	return busy;
}

static int
xnvmeperf_p2p_verify(struct xnvmeperf_args *args)
{
	struct xnvme_dev **devs = NULL;
	struct xnvmeperf_job *jobs = NULL;
	struct xnvmeperf_p2pcheck *chk = NULL;
	void **bufs = NULL;
	const uint32_t nslots = args->qdepth;
	uint32_t lba_nbytes = 0;
	uint64_t reported = 0, last_progress = 0;
	double t0 = 0, elapsed = 0, last_change = 0;
	int err;

	devs = calloc(args->ndevs, sizeof(*devs));
	jobs = calloc(args->ndevs, sizeof(*jobs));
	bufs = calloc((size_t)args->ndevs * nslots, sizeof(*bufs));
	if (!devs || !jobs || !bufs) {
		err = -ENOMEM;
		goto out;
	}
	err = xnvmeperf_open_devs(args, devs);
	if (err) {
		goto out;
	}
	xnvmeperf_p2pcheck_print_attrs(args->opts.gpu_id);
	err = xnvmeperf_p2pcheck_prepare();
	if (err) {
		xnvme_cli_perr("Failed: xnvmeperf_p2pcheck_prepare()", err);
		goto out;
	}

	for (int d = 0; d < args->ndevs; d++) {
		struct xnvmeperf_job *job = &jobs[d];
		uint64_t window;

		err = setup_job(job, devs[d], args, 1);
		if (err) {
			xnvme_cli_perr("Failed: setup_job()", err);
			goto out;
		}
		job->qidx = (uint32_t)d;
		if (!lba_nbytes) {
			lba_nbytes = (uint32_t)job->nbytes;
		} else if (lba_nbytes != job->nbytes) {
			err = -EINVAL;
			fprintf(stderr, "Error: devices differ in LBA size (%u vs %zu)\n",
				lba_nbytes, job->nbytes);
			goto out;
		}
		/* The pre-written window: 64 reads per slot, so a slot never re-reads
		 * the LBA it last held and a stale payload cannot pass by coincidence.
		 * job->nblocks becomes the window's span in LBAs, for the cursor. */
		window = 64ULL * nslots;
		if (window > job->nblocks) {
			window = job->nblocks;
		}
		if (window < 2ULL * nslots) {
			err = -EINVAL;
			fprintf(stderr, "Error: %s is too small for a %u-deep ring\n",
				args->dev_uris[d], nslots);
			goto out;
		}
		job->nblocks = window * job->nlb;
		job->peek_slba = peek_slba_window;
		job->advance_slba = advance_slba_window;
		err = job_p2pcheck_init(job, args);
		if (err) {
			xnvme_cli_perr("Failed: job_p2pcheck_init()", err);
			goto out;
		}
		for (uint32_t sl = 0; sl < nslots; sl++) {
			bufs[(size_t)d * nslots + sl] = job->bbufs[sl];
		}
	}

	/* Phase 1: the pattern, written from host memory at depth one exactly as
	 * verify does; nothing is observed yet, so flushing here is fine. */
	for (int d = 0; d < args->ndevs; d++) {
		struct xnvmeperf_job *job = &jobs[d];
		void *wbuf = xnvme_buf_host_alloc(devs[d], args->iosize);
		uint64_t nios = job->nblocks / job->nlb;

		if (!wbuf) {
			err = -errno;
			xnvme_cli_perr("Failed: xnvme_buf_host_alloc()", err);
			goto out;
		}
		xnvme_queue_set_cb(job->queue, cb_fn, job);
		job->opcode = XNVME_SPEC_NVM_OPC_WRITE;
		job->buf = wbuf;
		job->offset = 0;
		job->io_completed = 0;
		job->io_failed = 0;
		for (uint64_t i = 0; i < nios; i++) {
			struct xnvme_cmd_ctx *ctx;

			err = fill_pattern(wbuf, args->iosize, job->offset, job->nlb);
			if (err) {
				break;
			}
			ctx = xnvme_queue_get_cmd_ctx(job->queue);
			while (!ctx) {
				xnvme_queue_poke(job->queue, 0);
				ctx = xnvme_queue_get_cmd_ctx(job->queue);
			}
			err = submit_io(job, ctx);
			if (err) {
				xnvme_queue_put_cmd_ctx(job->queue, ctx);
				break;
			}
			while (job->io_completed + job->io_failed < i + 1) {
				xnvme_queue_poke(job->queue, 0);
			}
		}
		xnvme_queue_drain(job->queue);
		xnvme_buf_host_free(devs[d], wbuf);
		job->buf = NULL;
		if (err || job->io_failed) {
			if (!err) {
				err = -EIO;
			}
			fprintf(stderr,
				"Failed: pre-writing the pattern on %s (%lu failed): err(%d)\n",
				args->dev_uris[d], (unsigned long)job->io_failed, err);
			goto out;
		}
		/* Nothing stale in the ring before the first read lands in it */
		for (uint32_t sl = 0; sl < nslots; sl++) {
			xnvme_buf_clear(job->bbufs[sl], args->iosize);
		}
	}

	/* Phase 2: read back into GPU memory; the checker answers per completion */
	chk = xnvmeperf_p2pcheck_open((uint32_t)args->ndevs, nslots, args->iosize, lba_nbytes,
				      bufs);
	if (!chk) {
		err = -errno;
		xnvme_cli_perr("Failed: xnvmeperf_p2pcheck_open()", err);
		goto out;
	}
	for (int d = 0; d < args->ndevs; d++) {
		struct xnvmeperf_job *job = &jobs[d];

		job->chk = chk;
		job->opcode = XNVME_SPEC_NVM_OPC_READ;
		job->offset = 0;
		job->io_completed = 0;
		job->io_failed = 0;
		xnvme_queue_set_cb(job->queue, cb_fn_p2pcheck, job);
	}

	printf("\nxnvmeperf p2p-verify: reading back into GPU memory, checking on the GPU\n");
	t0 = p2p_now_s();
	last_change = t0;
	while (true) {
		int busy = 0, bounded = 1;
		double now = p2p_now_s();
		int timed_out = args->time && (now - t0) >= (double)args->time;
		uint64_t progress = 0;

		for (int d = 0; d < args->ndevs; d++) {
			struct xnvmeperf_job *job = &jobs[d];
			int done =
				timed_out ||
				(args->count && job->io_completed + job->io_failed >= args->count);

			xnvme_queue_poke(job->queue, 0);
			busy |= p2p_collect(job, chk, args->dev_uris[d], &reported);
			progress += job->io_completed + job->io_failed + job->matches +
				    job->mismatches;
			if (done) {
				continue;
			}
			bounded = 0;
			while (true) {
				struct xnvme_cmd_ctx *ctx = xnvme_queue_get_cmd_ctx(job->queue);

				if (!ctx) {
					break;
				}
				err = submit_io(job, ctx);
				if (err) {
					xnvme_queue_put_cmd_ctx(job->queue, ctx);
					if (err == -EBUSY) {
						err = 0;
					}
					break;
				}
			}
			if (err) {
				xnvme_cli_perr("Failed: submit_io()", err);
				goto drain;
			}
		}
		if (bounded && !busy) {
			break;
		}
		/* Neither a completion nor a verdict in 5 s: a lost command or a
		 * checker that is not answering; stop rather than spin forever. */
		if (progress != last_progress) {
			last_progress = progress;
			last_change = now;
		} else if (now - last_change >= 5.0) {
			fprintf(stderr, "Error: no completion or verdict for 5s; giving up\n");
			err = -ETIMEDOUT;
			goto drain;
		}
	}

drain:
	elapsed = p2p_now_s() - t0;
	{
		double deadline = p2p_now_s() + 5.0;
		int busy;

		for (int d = 0; d < args->ndevs; d++) {
			xnvme_queue_drain(jobs[d].queue);
		}
		do {
			busy = 0;
			for (int d = 0; d < args->ndevs; d++) {
				xnvme_queue_poke(jobs[d].queue, 0);
				busy |= p2p_collect(&jobs[d], chk, args->dev_uris[d], &reported);
			}
		} while (busy && p2p_now_s() < deadline);
		if (busy) {
			fprintf(stderr,
				"Error: the checker did not answer within 5s; is it resident?\n");
			if (!err) {
				err = -ETIMEDOUT;
			}
		}
	}
	print_p2pverify_results(args, jobs, elapsed);

out:
	/* Checker first, then the queues (and with them the CQ mirror), then the
	 * checker's memory: freeing GPU memory waits for every resident kernel. */
	xnvmeperf_p2pcheck_stop(chk);
	if (jobs) {
		for (int d = 0; d < args->ndevs; d++) {
			job_p2pcheck_term(&jobs[d]);
		}
	}
	xnvmeperf_p2pcheck_close(chk);
	free(bufs);
	free(jobs);
	if (devs) {
		for (int d = 0; d < args->ndevs; d++) {
			if (devs[d]) {
				xnvme_dev_close(devs[d]);
			}
		}
		free(devs);
	}
	return err;
}

static int
sub_p2p_verify(struct xnvme_cli *cli)
{
	struct xnvmeperf_args args = {0};
	int err;

	err = parse_common_args(cli, &args);
	if (err) {
		return err;
	}
	if (!args.opts.be) {
		args.opts.be = "upcie-cuda";
	} else if (strcmp(args.opts.be, "upcie-cuda") != 0) {
		err = -EINVAL;
		fprintf(stderr, "Error: p2p-verify requires --be upcie-cuda, got '%s': err(%d)\n",
			args.opts.be, err);
		return err;
	}
	args.qdepth = cli->args.qdepth;
	if (!args.qdepth || !xnvme_is_pow2(args.qdepth)) {
		err = -EINVAL;
		xnvme_cli_perr("Error: --qdepth must be a power of 2", err);
		return err;
	}
	args.count = cli->args.count;
	args.time = cli->args.runtime;
	if (!args.count && !args.time) {
		err = -EINVAL;
		xnvme_cli_perr("Error: give --count or --runtime", err);
		return err;
	}
	args.nqueues = 1;
	args.pattern = IOPATTERN_VERIFY;

	print_run_args(&args, "p2p-verify");
	derive_heap_sizes(&args);

	return xnvmeperf_p2p_verify(&args);
}

static int
sub_cuda_verify(struct xnvme_cli *cli)
{
	struct xnvmeperf_args args = {0};
	int err;

	err = parse_common_args(cli, &args);
	if (err) {
		return err;
	}

	if (!args.opts.be) {
		args.opts.be = "upcie-cuda";
	} else if (strcmp(args.opts.be, "upcie-cuda") != 0) {
		err = -EINVAL;
		fprintf(stderr, "Error: cuda-verify requires --be upcie-cuda, got '%s': err(%d)\n",
			args.opts.be, err);
		return err;
	}

	args.qdepth = cli->args.qdepth;
	if (!args.qdepth || !xnvme_is_pow2(args.qdepth)) {
		err = -EINVAL;
		xnvme_cli_perr("Error: --qdepth must be a power of 2", err);
		return err;
	}

	args.nqueues = cli->args.nqueues ? cli->args.nqueues : 1;

	derive_heap_sizes(&args);

	return xnvmeperf_cuda_verify(&args);
}

static int
sub_htod_roofline(struct xnvme_cli *cli)
{
	struct xnvme_opts opts = xnvme_opts_default();
	uint32_t iosize = cli->args.iosize;
	uint32_t nslots = cli->args.qdepth ? cli->args.qdepth : 1;
	uint32_t seconds = cli->args.runtime ? cli->args.runtime : 5;
	double gbps = 0.0;
	int err;

	xnvme_cli_to_opts(cli, &opts);

	if (!iosize || !xnvme_is_pow2(iosize)) {
		err = -EINVAL;
		xnvme_cli_perr("Error: --iosize must be a power of 2", err);
		return err;
	}

	printf("xnvmeperf htod-roofline: iosize %u, in-flight %u, runtime %u s, gpu %u\n", iosize,
	       nslots, seconds, opts.gpu_id);

	err = xnvmeperf_htod_roofline(opts.gpu_id, iosize, nslots, seconds, &gbps);
	if (err) {
		xnvme_cli_perr("Failed: xnvmeperf_htod_roofline()", err);
		return err;
	}

	printf("\nhost-to-device copy: %.2f GB/s\n", gbps);
	return 0;
}

static struct xnvme_cli_sub g_subs[] = {
	{
		"run",
		"Run a benchmark against the given devices",
		"Run a time-bounded async IO benchmark against one or more NVMe devices.\n"
		"Devices are distributed across CPU threads pinned by --cpumask or --cpulist.",
		sub_run,
		{
			{XNVME_CLI_OPT_POSA_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_URI, XNVME_CLI_POSN},
			{XNVME_CLI_OPT_NON_POSA_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_IOPATTERN, XNVME_CLI_LREQ},
			{XNVME_CLI_OPT_NQUEUES, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_QDEPTH, XNVME_CLI_LREQ},
			{XNVME_CLI_OPT_IOSIZE, XNVME_CLI_LREQ},
			{XNVME_CLI_OPT_RUNTIME, XNVME_CLI_LREQ},
			{XNVME_CLI_OPT_CPUMASK, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_CPULIST, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_ORCH_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_BE, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_SUBNQN, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_DIRECT, XNVME_CLI_LFLG},
			{XNVME_CLI_OPT_POLL_IO, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_POLL_SQ, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_GPU_ID, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_HOMI_ID, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_REPORT_FREQ, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_P2P_CQ_MIRROR, XNVME_CLI_LFLG},
			{XNVME_CLI_OPT_P2P_UNORDERED, XNVME_CLI_LFLG},
			{XNVME_CLI_OPT_BUF_HOST_BOUNCE, XNVME_CLI_LFLG},
			{XNVME_CLI_OPT_BUF_HOSTMEM, XNVME_CLI_LFLG},
		},
	},
	{
		"verify",
		"Verify data integrity by writing and reading back a known pattern",
		"For each device: writes --count sequential IOs with a per-LBA pattern,\n"
		"then reads them back and compares against the expected data.",
		sub_verify,
		{
			{XNVME_CLI_OPT_POSA_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_URI, XNVME_CLI_POSN},
			{XNVME_CLI_OPT_NON_POSA_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_IOSIZE, XNVME_CLI_LREQ},
			{XNVME_CLI_OPT_COUNT, XNVME_CLI_LREQ},
			{XNVME_CLI_OPT_ORCH_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_BE, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_SUBNQN, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_DIRECT, XNVME_CLI_LFLG},
			{XNVME_CLI_OPT_POLL_IO, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_POLL_SQ, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_GPU_ID, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_HOMI_ID, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_P2P_CQ_MIRROR, XNVME_CLI_LFLG},
			{XNVME_CLI_OPT_P2P_UNORDERED, XNVME_CLI_LFLG},
			{XNVME_CLI_OPT_BUF_HOSTMEM, XNVME_CLI_LFLG},
		},
	},
	{
		"p2p-verify",
		"Check each P2P payload on the GPU the moment its completion is seen",
		"Pre-writes a per-LBA pattern, then reads it back into GPU memory with the\n"
		"CPU driving the queues; a resident GPU kernel reads every payload out of\n"
		"VRAM as soon as the host has seen its completion and compares it. Without\n"
		"--p2p-cq-mirror the completion can be visible before the payload has landed.",
		sub_p2p_verify,
		{
			{XNVME_CLI_OPT_POSA_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_URI, XNVME_CLI_POSN},
			{XNVME_CLI_OPT_NON_POSA_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_IOSIZE, XNVME_CLI_LREQ},
			{XNVME_CLI_OPT_QDEPTH, XNVME_CLI_LREQ},
			{XNVME_CLI_OPT_COUNT, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_RUNTIME, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_ORCH_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_BE, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_SUBNQN, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_DIRECT, XNVME_CLI_LFLG},
			{XNVME_CLI_OPT_POLL_IO, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_POLL_SQ, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_GPU_ID, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_HOMI_ID, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_P2P_CQ_MIRROR, XNVME_CLI_LFLG},
			{XNVME_CLI_OPT_P2P_UNORDERED, XNVME_CLI_LFLG},
		},
	},
	{
		"cuda-run",
		"Run a GPU benchmark against the given devices (requires upcie-cuda backend)",
		"Run a time-bounded GPU NVMe I/O benchmark against one or more devices.\n"
		"All devices run in parallel: one CUDA block per queue, all queues launched\n"
		"in a single kernel. All devices must use the same LBA size.",
		sub_cuda_run,
		{
			{XNVME_CLI_OPT_POSA_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_URI, XNVME_CLI_POSN},
			{XNVME_CLI_OPT_NON_POSA_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_IOPATTERN, XNVME_CLI_LREQ},
			{XNVME_CLI_OPT_NQUEUES, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_QDEPTH, XNVME_CLI_LREQ},
			{XNVME_CLI_OPT_IOSIZE, XNVME_CLI_LREQ},
			{XNVME_CLI_OPT_RUNTIME, XNVME_CLI_LREQ},
			{XNVME_CLI_OPT_ORCH_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_BE, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_GPU_ID, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_HOMI_ID, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_REPORT_FREQ, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_SQ_HOSTMEM, XNVME_CLI_LFLG},
		},
	},
	{
		"cuda-verify",
		"Verify GPU NVMe I/O data integrity (requires upcie-cuda backend)",
		"Write an LBA-stamped pattern to each device through GPU queues and read\n"
		"it back, verifying that the data matches. Uses the same queue topology\n"
		"as cuda-run so results are directly comparable.",
		sub_cuda_verify,
		{
			{XNVME_CLI_OPT_POSA_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_URI, XNVME_CLI_POSN},
			{XNVME_CLI_OPT_NON_POSA_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_IOSIZE, XNVME_CLI_LREQ},
			{XNVME_CLI_OPT_NQUEUES, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_QDEPTH, XNVME_CLI_LREQ},
			{XNVME_CLI_OPT_ORCH_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_BE, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_GPU_ID, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_HOMI_ID, XNVME_CLI_LOPT},
			{XNVME_CLI_OPT_SQ_HOSTMEM, XNVME_CLI_LFLG},
		},
	},
	{
		"htod-roofline",
		"Host-to-device copy bandwidth ceiling (requires a GPU backend build)",
		"Keep --qdepth pinned copies of --iosize in flight to one GPU for --runtime\n"
		"seconds and report the delivered host-to-device GB/s. Takes no NVMe devices.",
		sub_htod_roofline,
		{
			{XNVME_CLI_OPT_NON_POSA_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_IOSIZE, XNVME_CLI_LREQ},
			{XNVME_CLI_OPT_QDEPTH, XNVME_CLI_LREQ},
			{XNVME_CLI_OPT_RUNTIME, XNVME_CLI_LREQ},
			{XNVME_CLI_OPT_GPU_ID, XNVME_CLI_LOPT},
		},
	},
};

static struct xnvme_cli g_cli = {
	.title = "xnvmeperf - NVMe async IO benchmark",
	.vcs = XNVME_VCS_TAG,
	.descr_short = "Run async IO benchmarks against NVMe devices",
	.descr_long = "",
	.subs = g_subs,
	.nsubs = sizeof g_subs / sizeof(*g_subs),
};

int
main(int argc, char **argv)
{
	return xnvme_cli_run(&g_cli, argc, argv, XNVME_CLI_INIT_NONE);
}
