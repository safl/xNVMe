// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

/**
 * Queue setup for `upcie-cuda`: the P2P flush, XNVME_QUEUE_P2P_CQ_MIRROR and
 * XNVME_QUEUE_P2P_UNORDERED
 *
 * A payload in GPU memory and a completion in host memory are two PCIe
 * completers, so the completion can be visible before the payload has landed.
 * By default the queue is `upcie`'s with one addition: the poke does a host
 * read of the GPU before handing out the completions it found, which pushes
 * the payload writes queued ahead of it into GPU memory. With P2P_CQ_MIRROR
 * the controller completes into a CQ in the GPU heap, next to the data, and a
 * resident warp keeps the queue's dmamem CQ a copy of it, so there is one
 * completer and nothing to flush; on a served controller the server creates
 * the queue and is told where the CQ is, by offset into the heap this process
 * registered. P2P_UNORDERED declines both and is refused with the mirror. The
 * HIP backend has no flush, so its ordered default is the mirror.
 */
#include <libxnvme.h>
#include <errno.h>
#include <string.h>
#ifdef XNVME_BE_UPCIE_CUDA_ENABLED
#include <xnvme_dev.h>
#include <xnvme_queue.h>
#include <xnvme_be_upcie_cuda.h>
#include <xnvme_be_upcie_cuda_cqmirror.h>

static int
_queue_init(struct xnvme_queue *queue, int opts)
{
	struct xnvme_queue_upcie *upcie_queue = (void *)queue;
	struct xnvme_be_upcie_state *state = (void *)queue->base.dev->be.state;
	struct dmamem *host = &g_upcie_rte.mem.dmem;
	uint16_t depth = queue->base.capacity + 1;
	size_t nbytes;
	uint64_t cq_iova;
	CUcontext prev;
	CUresult res;
	int err;

	if ((opts & XNVME_QUEUE_P2P_UNORDERED) && (opts & XNVME_QUEUE_P2P_CQ_MIRROR)) {
		XNVME_DEBUG(
			"FAILED: P2P_UNORDERED asks for no ordering, P2P_CQ_MIRROR for ordering");
		return -EINVAL;
	}
	/* The flush is the default: it costs one host read per poke that reaped
	 * and closes the window in which a completion is seen before its payload.
	 * The mirror needs none (one completer), UNORDERED declines it. */
	upcie_queue->p2p_flush = !(opts & (XNVME_QUEUE_P2P_CQ_MIRROR | XNVME_QUEUE_P2P_UNORDERED));
	opts &= ~XNVME_QUEUE_P2P_UNORDERED;
#if CUDA_VERSION < 11030
	if (upcie_queue->p2p_flush) {
		XNVME_DEBUG("FAILED: no cuFlushGPUDirectRDMAWrites() before CUDA 11.3; pass "
			    "XNVME_QUEUE_P2P_CQ_MIRROR or XNVME_QUEUE_P2P_UNORDERED");
		return -ENOTSUP;
	}
#endif
	if (!(opts & XNVME_QUEUE_P2P_CQ_MIRROR)) {
		return xnvme_be_upcie_queue_init_unlocked(queue, opts);
	}
	nbytes = ((size_t)depth * sizeof(struct nvme_completion) + 4095) & ~(size_t)4095;
	upcie_queue->cq_gpu = cudamem_heap_block_alloc_array_aligned(&g_upcie_cuda_rte.cuda_heap,
								     1, nbytes, 4096);
	if (!upcie_queue->cq_gpu) {
		XNVME_DEBUG("FAILED: allocating %zu bytes of GPU memory for the CQ; errno(%d)",
			    nbytes, errno);
		return -errno;
	}

	/* The controller may only ever see phase one where a completion was
	 * written, and the warp expects the same of the host copy, which
	 * nvme_qpair_dmamem_init() zeroes. */
	cuCtxPushCurrent(g_upcie_cuda_rte.cu_ctx);
	res = cuMemsetD8((CUdeviceptr)upcie_queue->cq_gpu, 0, nbytes);
	if (res == CUDA_SUCCESS) {
		res = cuStreamSynchronize(NULL);
	}
	cuCtxPopCurrent(&prev);
	if (res != CUDA_SUCCESS) {
		XNVME_DEBUG("FAILED: zeroing the CQ; res(%d)", res);
		err = -EIO;
		goto free_cq;
	}

	/* The address is the server's to know where the controller is served:
	 * it resolves the offset through the registration it holds. */
	cq_iova = g_upcie_rte.connection.alive
			  ? 0
			  : dmamem_va_to_iova(state->dmem, upcie_queue->cq_gpu);
	if (!cq_iova && !g_upcie_rte.connection.alive) {
		XNVME_DEBUG("FAILED: the CQ has no address the controller can reach");
		err = -EFAULT;
		goto free_cq;
	}

	if (g_upcie_rte.connection.alive) {
		/* The server creates the queue and takes the SQ and the PRP
		 * scratch from its heap; the CQ is named by offset into the
		 * region this process registered, which is the whole heap. */
		struct xnvme_be_upcie_cuda_ctrlr *slot = _cuda_ctrlr_slot_of(state->ctrlr);

		if (!slot || !slot->reg_offset) {
			XNVME_DEBUG("FAILED: device heap not registered with the server");
			err = -ENOTCONN;
			goto free_cq;
		}
		err = xnvme_be_upcie_cplane_alloc_qpair_cq_at(
			state->ctrlr, &upcie_queue->qpair, depth, slot->reg_offset,
			(uint64_t)(uintptr_t)upcie_queue->cq_gpu -
				g_upcie_cuda_rte.cuda_heap.vaddr);
	} else {
		err = nvme_controller_create_io_qpair_dmamem_cq_iova(
			state->ctrlr->ctrl, &upcie_queue->qpair, depth, &g_upcie_rte.mem.heap,
			&upcie_queue->offsets.sq, &upcie_queue->offsets.cq,
			&upcie_queue->offsets.prp, cq_iova);
	}
	if (err) {
		XNVME_DEBUG("FAILED: creating a queue of %u; err(%d)", depth, err);
		goto free_cq;
	}

	err = xnvme_be_upcie_cuda_cqmirror_attach(g_upcie_cuda_rte.cu_ctx, host->cpu_va,
						  host->size, upcie_queue->cq_gpu,
						  upcie_queue->qpair.cq, depth);
	if (err < 0) {
		XNVME_DEBUG("FAILED: xnvme_be_upcie_cuda_cqmirror_attach(); err(%d)", err);
		goto delete_qpair;
	}
	upcie_queue->cqmirror_slot = err;

	return 0;

delete_qpair:
	if (g_upcie_rte.connection.alive) {
		xnvme_be_upcie_cplane_free_qpair(state->ctrlr, &upcie_queue->qpair);
	} else {
		nvme_controller_delete_io_qpair_dmamem(
			state->ctrlr->ctrl, &upcie_queue->qpair, &g_upcie_rte.mem.heap,
			upcie_queue->offsets.sq, upcie_queue->offsets.cq,
			upcie_queue->offsets.prp);
	}
free_cq:
	cudamem_heap_block_free(&g_upcie_cuda_rte.cuda_heap, upcie_queue->cq_gpu);
	upcie_queue->cq_gpu = NULL;

	return err;
}

int
xnvme_be_upcie_cuda_queue_init(struct xnvme_queue *queue, int opts)
{
	int err;

	xnvme_be_upcie_heap_lock();
	err = _queue_init(queue, opts);
	xnvme_be_upcie_heap_unlock();
	return err;
}

static int
_queue_term(struct xnvme_queue *queue)
{
	struct xnvme_queue_upcie *upcie_queue = (void *)queue;
	struct xnvme_be_upcie_state *state = (void *)queue->base.dev->be.state;
	int err;

	if (!upcie_queue->cq_gpu) {
		return xnvme_be_upcie_queue_term_unlocked(queue);
	}

	/* The warp first, so nothing reads the CQ once the controller is told
	 * to let go of it; the memory last, since a failed delete keeps it. A
	 * served queue is handed back instead, and the server keeps or frees
	 * its half by the same rule. */
	xnvme_be_upcie_cuda_cqmirror_detach(upcie_queue->cqmirror_slot);
	if (g_upcie_rte.connection.alive) {
		xnvme_be_upcie_cplane_free_qpair(state->ctrlr, &upcie_queue->qpair);
		err = 0;
	} else {
		err = nvme_controller_delete_io_qpair_dmamem(
			state->ctrlr->ctrl, &upcie_queue->qpair, &g_upcie_rte.mem.heap,
			upcie_queue->offsets.sq, upcie_queue->offsets.cq,
			upcie_queue->offsets.prp);
	}
	if (err) {
		XNVME_DEBUG("FAILED: deleting qid %u; err(%d), keeping its GPU memory too",
			    upcie_queue->qpair.qid, err);
		return 0;
	}
	cudamem_heap_block_free(&g_upcie_cuda_rte.cuda_heap, upcie_queue->cq_gpu);
	upcie_queue->cq_gpu = NULL;

	return 0;
}

int
xnvme_be_upcie_cuda_queue_term(struct xnvme_queue *queue)
{
	int err;

	xnvme_be_upcie_heap_lock();
	err = _queue_term(queue);
	xnvme_be_upcie_heap_unlock();
	return err;
}

int
xnvme_be_upcie_cuda_queue_poke(struct xnvme_queue *queue, uint32_t max)
{
	struct xnvme_queue_upcie *upcie_queue = (struct xnvme_queue_upcie *)queue;
	struct nvme_qpair *qp = &upcie_queue->qpair;
	struct nvme_completion *cq = qp->cq;
	uint16_t head = qp->head, phase = qp->phase;
	unsigned int visible = 0, reaped = 0;

	if (!upcie_queue->p2p_flush) {
		return xnvme_be_upcie_queue_poke(queue, max);
	}
	if (!max) {
		max = queue->base.outstanding;
	}

	wmb();
	nvme_qpair_sqdb_update(qp);

	/* Count what is visible now, flush once, then hand out exactly that many:
	 * a completion arriving after the read has not had its payload pushed. */
	while (visible < max && ((*(const volatile uint16_t *)&cq[head].status) & 0x1) == phase) {
		visible++;
		if (++head == qp->depth) {
			head = 0;
			phase ^= 1;
		}
	}
	if (visible) {
#if CUDA_VERSION >= 11030
		CUcontext prev;

		cuCtxPushCurrent(g_upcie_cuda_rte.cu_ctx);
		cuFlushGPUDirectRDMAWrites(CU_FLUSH_GPU_DIRECT_RDMA_WRITES_TARGET_CURRENT_CTX,
					   CU_FLUSH_GPU_DIRECT_RDMA_WRITES_TO_OWNER);
		cuCtxPopCurrent(&prev);
#endif
	}

	while (reaped < visible) {
		struct nvme_completion *cqe = &cq[qp->head];
		struct xnvme_cmd_ctx *ctx;
		struct nvme_request *req;

		dma_rmb();
		if (++qp->head == qp->depth) {
			qp->head = 0;
			qp->phase ^= 1;
		}
		reaped++;

		req = nvme_request_get(qp->rpool, cqe->cid);
		if (!req) {
			XNVME_DEBUG("FAILED: nvme_request_get()");
			return -EIO;
		}
		ctx = req->user;
		memcpy(&ctx->cpl, cqe, sizeof(ctx->cpl));
		nvme_request_free(qp->rpool, req->cid);
		queue->base.outstanding -= 1;
		ctx->async.cb(ctx, ctx->async.cb_arg);
	}

	if (reaped) {
		mmio_write32(qp->cqdb, 0, qp->head);
		upcie_queue->pokes_idle = 0;
		upcie_queue->served_gone_ns = 0;
		return reaped;
	}
	if (!g_upcie_rte.connection.alive) {
		return 0;
	}
	return xnvme_be_upcie_queue_poke_idle(upcie_queue);
}
#endif
