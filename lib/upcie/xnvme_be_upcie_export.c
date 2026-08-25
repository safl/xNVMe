// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

/**
 * Describing a runtime this process owns, so another can attach to it
 *
 * The backend knows things homi would otherwise have to reach in and take: the
 * heap's descriptor, the controller's BAR, and where in the heap a consumer's
 * description of both can live. This puts them behind one call, so the serving
 * loop stays about serving.
 *
 * Everything published here is written once, when the runtime comes up. What a
 * consumer does with it afterwards, and what it is granted, is homi's business.
 */
#include <errno.h>
#include <string.h>

#include <libxnvme.h>
#include <xnvme_be.h>
#include <xnvme_dev.h>

#ifdef XNVME_BE_UPCIE_ENABLED
#include <xnvme_be_upcie.h>

/**
 * Place the record and the heap description in the heap, and report the rest
 *
 * @param dev A device this process opened
 * @param out Pre-allocated export to fill
 *
 * @return 0 on success, negative errno on error
 */
int
xnvme_be_upcie_export(struct xnvme_dev *dev, struct xnvme_be_upcie_export *out)
{
	struct xnvme_be_upcie_state *state;
	struct nvme_runtime_record *record;
	struct hostmem_shared_desc *desc;
	struct nvme_controller *ctrl;
	size_t record_offset, desc_offset, desc_nbytes;
	char *heap_base;
	int err;

	if (!dev || !out) {
		return -EINVAL;
	}

	state = (void *)dev->be.state;
	ctrl = state->ctrlr->ctrl;
	heap_base = g_upcie_rte.mem.dmem.base_va;

	if (!g_upcie_rte.mem.heap_alive || !heap_base) {
		XNVME_DEBUG("FAILED: no heap to describe");
		return -ENOTCONN;
	}

	/* The description is sized by the granules the region spans, so it has
	 * to be allocated before it can be filled. */
	desc_nbytes = hostmem_shared_desc_nbytes(g_upcie_rte.mem.hp.nphys);

	err = dmamem_heap_alloc(&g_upcie_rte.mem.heap, desc_nbytes, &desc_offset);
	if (err) {
		XNVME_DEBUG("FAILED: dmamem_heap_alloc(desc); err(%d)", err);
		return err;
	}

	err = dmamem_heap_alloc(&g_upcie_rte.mem.heap, sizeof(*record), &record_offset);
	if (err) {
		XNVME_DEBUG("FAILED: dmamem_heap_alloc(record); err(%d)", err);
		dmamem_heap_free(&g_upcie_rte.mem.heap, desc_offset);
		return err;
	}

	desc = (struct hostmem_shared_desc *)(heap_base + desc_offset);
	record = (struct nvme_runtime_record *)(heap_base + record_offset);

	err = hostmem_shared_desc_fill(desc, &g_upcie_rte.mem.hp);
	if (err) {
		XNVME_DEBUG("FAILED: hostmem_shared_desc_fill(); err(%d)", err);
		goto failed;
	}

	err = nvme_runtime_record_export(ctrl, g_upcie_rte.mem.dmem.size, record);
	if (err) {
		XNVME_DEBUG("FAILED: nvme_runtime_record_export(); err(%d)", err);
		goto failed;
	}
	record->desc_offset = desc_offset;

	memset(out, 0, sizeof(*out));
	out->heap_fd = g_upcie_rte.mem.hp.fd;
	out->bar0_fd = ctrl->func.bars[0].fd;
	out->bar0_nbytes = ctrl->func.bars[0].size;
	out->heap_nbytes = g_upcie_rte.mem.dmem.size;
	out->record_offset = record_offset;

	return 0;

failed:
	dmamem_heap_free(&g_upcie_rte.mem.heap, record_offset);
	dmamem_heap_free(&g_upcie_rte.mem.heap, desc_offset);

	return err;
}

/**
 * Queues created for consumers, so that a tool never handles a uPCIe structure
 *
 * One table per process, since a process holds one runtime. The identifier is
 * the key because that is what a consumer hands back, and what a disconnect
 * leaves behind for whoever is reaping.
 */
static struct {
	struct nvme_qpair qpair;
	size_t sq_offset;
	size_t cq_offset;
	size_t prp_offset;
	int live;
} g_grants[XNVME_BE_UPCIE_GRANTS_MAX];

/**
 * Create a queue for a consumer and describe it in offsets
 *
 * @param dev A device this process opened
 * @param depth Entries the consumer asked for
 * @param out Pre-allocated grant to fill
 *
 * @return 0 on success, negative errno on error
 */
int
xnvme_be_upcie_grant(struct xnvme_dev *dev, uint16_t depth, struct xnvme_be_upcie_qgrant *out)
{
	struct xnvme_be_upcie_state *state;
	struct nvme_controller *ctrl;
	size_t slot;
	int err;

	if (!dev || !out || !depth) {
		return -EINVAL;
	}

	state = (void *)dev->be.state;
	ctrl = state->ctrlr->ctrl;

	for (slot = 0; slot < XNVME_BE_UPCIE_GRANTS_MAX; ++slot) {
		if (!g_grants[slot].live) {
			break;
		}
	}
	if (slot == XNVME_BE_UPCIE_GRANTS_MAX) {
		XNVME_DEBUG("FAILED: no room for another grant");
		return -ENOSPC;
	}

	/* The dmamem variant, because that is what this runtime's heap is. It
	 * allocates the queue and the request pool's scratch and reports where
	 * it put them, which is exactly what a consumer needs to be told. */
	err = nvme_controller_create_io_qpair_dmamem(
		ctrl, &g_grants[slot].qpair, depth, &g_upcie_rte.mem.heap,
		&g_grants[slot].sq_offset, &g_grants[slot].cq_offset, &g_grants[slot].prp_offset);
	if (err) {
		XNVME_DEBUG("FAILED: nvme_controller_create_io_qpair_dmamem(); err(%d)", err);
		return err;
	}

	g_grants[slot].live = 1;

	memset(out, 0, sizeof(*out));
	out->sq_offset = g_grants[slot].sq_offset;
	out->cq_offset = g_grants[slot].cq_offset;
	out->prp_offset = g_grants[slot].prp_offset;
	out->qid = g_grants[slot].qpair.qid;
	out->depth = g_grants[slot].qpair.depth;

	return 0;
}

/**
 * Delete a queue granted earlier and release what went with it
 *
 * @param dev A device this process opened
 * @param qid The identifier from a grant
 *
 * @return 0 on success, negative errno on error
 */
int
xnvme_be_upcie_ungrant(struct xnvme_dev *dev, uint32_t qid)
{
	struct xnvme_be_upcie_state *state;
	struct nvme_controller *ctrl;

	if (!dev || !qid) {
		return -EINVAL;
	}

	state = (void *)dev->be.state;
	ctrl = state->ctrlr->ctrl;

	for (size_t slot = 0; slot < XNVME_BE_UPCIE_GRANTS_MAX; ++slot) {
		if (!g_grants[slot].live || (g_grants[slot].qpair.qid != qid)) {
			continue;
		}

		/* The queue goes before its memory does: the controller has to
		 * stop being able to reach an address before it stops
		 * resolving, which this does in one call. */
		nvme_controller_delete_io_qpair_dmamem(
			ctrl, &g_grants[slot].qpair, &g_upcie_rte.mem.heap,
			g_grants[slot].sq_offset, g_grants[slot].cq_offset,
			g_grants[slot].prp_offset);
		memset(&g_grants[slot], 0, sizeof(g_grants[slot]));

		return 0;
	}

	return -ENOENT;
}

/**
 * Submit an admin command on a consumer's behalf
 *
 * The payload does not come through here: the command names an address the
 * device can already reach, from memory the consumer registered or was
 * granted, so what lands where is the consumer's arrangement.
 *
 * @param dev A device this process opened
 * @param cmd A struct nvme_command to submit
 * @param cpl A struct nvme_completion to fill
 *
 * @return 0 on success, negative errno on error
 */
int
xnvme_be_upcie_admin(struct xnvme_dev *dev, void *cmd, void *cpl)
{
	struct xnvme_be_upcie_state *state;
	struct nvme_controller *ctrl;

	if (!dev || !cmd || !cpl) {
		return -EINVAL;
	}

	state = (void *)dev->be.state;
	ctrl = state->ctrlr->ctrl;

	return nvme_qpair_submit_sync(&ctrl->aq, cmd, ctrl->timeout_ms, cpl);
}

/**
 * Allocate from the heap on a consumer's behalf
 *
 * A consumer cannot allocate here itself: the free list is a chain of this
 * process's addresses and has no lock. It asks and receives an offset, which
 * is what its own mapping resolves against.
 *
 * @param nbytes How much the consumer asked for
 * @param offset Set to where the memory begins in the heap
 *
 * @return 0 on success, negative errno on error
 */
int
xnvme_be_upcie_lend(size_t nbytes, uint64_t *offset)
{
	size_t at;
	int err;

	if (!nbytes || !offset) {
		return -EINVAL;
	}
	if (!g_upcie_rte.mem.heap_alive) {
		return -ENOTCONN;
	}

	err = dmamem_heap_alloc(&g_upcie_rte.mem.heap, nbytes, &at);
	if (err) {
		XNVME_DEBUG("FAILED: dmamem_heap_alloc(%zu); err(%d)", nbytes, err);
		return err;
	}

	*offset = at;

	return 0;
}

/**
 * Take back memory lent to a consumer
 *
 * @param offset An offset from xnvme_be_upcie_lend()
 *
 * @return 0 on success, negative errno on error
 */
int
xnvme_be_upcie_reclaim(uint64_t offset)
{
	if (!g_upcie_rte.mem.heap_alive) {
		return -ENOTCONN;
	}

	dmamem_heap_free(&g_upcie_rte.mem.heap, offset);

	return 0;
}
#endif
