// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

#include <xnvme_be.h>
#include <xnvme_be_nosys.h>
#ifdef XNVME_BE_UPCIE_ENABLED
#include <errno.h>
#include <xnvme_be_upcie.h>
#include <xnvme_dev.h>

/**
 * Allocate a buffer from the RTE's dmamem_heap.
 *
 * Offsets into the heap resolve to device addresses through the dmamem
 * translator, so the same allocation works whichever way the target is
 * attached; the caller hands the returned VA to the device as a PRP later.
 */
void *
xnvme_be_upcie_buf_alloc(const struct xnvme_dev *XNVME_UNUSED(dev), size_t nbytes, uint64_t *phys)
{
	size_t offset = 0;
	void *buf;
	int err;

	if (g_upcie_rte.attached.alive) {
		/* The allocator belongs to whoever owns the heap, so this asks
		 * for an offset rather than taking one. */
		struct nvme_delegate_msg msg = {0};

		msg.op = NVME_DELEGATE_OP_ALLOC;
		msg.u.mem.nbytes = nbytes;

		err = xnvme_be_upcie_ask(&msg, NULL, NULL);
		if (err) {
			errno = -err;
			return NULL;
		}

		buf = (char *)g_upcie_rte.attached.heap_base + msg.u.mem.offset;
		if (phys) {
			*phys = dmamem_va_to_iova(&g_upcie_rte.mem.dmem, buf);
		}

		return buf;
	}

	err = dmamem_heap_alloc(&g_upcie_rte.mem.heap, nbytes, &offset);
	if (err) {
		errno = -err;
		return NULL;
	}

	buf = dmamem_heap_at_va(&g_upcie_rte.mem.heap, offset);
	if (!buf) {
		dmamem_heap_free(&g_upcie_rte.mem.heap, offset);
		errno = EFAULT;
		return NULL;
	}

	if (phys) {
		*phys = dmamem_heap_at_iova(&g_upcie_rte.mem.heap, offset);
	}

	return buf;
}

void
xnvme_be_upcie_buf_free(const struct xnvme_dev *XNVME_UNUSED(dev), void *buf)
{
	size_t offset;

	if (!buf) {
		return;
	}
	offset = (size_t)((char *)buf - (char *)g_upcie_rte.mem.dmem.cpu_va);

	if (g_upcie_rte.attached.alive) {
		struct nvme_delegate_msg msg = {0};

		msg.op = NVME_DELEGATE_OP_FREE;
		msg.u.mem.offset = offset;

		if (xnvme_be_upcie_ask(&msg, NULL, NULL)) {
			XNVME_DEBUG("FAILED: giving back offset(0x%zx)", offset);
		}

		return;
	}

	dmamem_heap_free(&g_upcie_rte.mem.heap, offset);
}

int
xnvme_be_upcie_buf_vtophys(const struct xnvme_dev *XNVME_UNUSED(dev), void *buf, uint64_t *phys)
{
	*phys = dmamem_va_to_iova(&g_upcie_rte.mem.dmem, buf);

	return 0;
}

/**
 * Register memory the caller owns, so the controller can reach it
 *
 * Only where the controller translates through an address space this process
 * can add to. Under uio_pci_generic it consumes physical addresses, which come
 * from pagemap for memory nobody described, and an attached process has neither
 * the privilege to read them nor an address space of its own to map into.
 */
static int
xnvme_be_upcie_mem_map(const struct xnvme_dev *XNVME_UNUSED(dev), void *vaddr, size_t nbytes,
		       uint64_t *phys)
{
	uint64_t iova = 0;
	int err;

	if (g_upcie_rte.mode != XNVME_BE_UPCIE_MODE_VFIO_CDEV) {
		XNVME_DEBUG("FAILED: mapping caller memory needs an IOAS to map it into");
		return -ENOTSUP;
	}

	err = iommufd_ioas_map(&g_upcie_rte.cdev.iommufd, (uint64_t)vaddr, nbytes,
			       IOMMU_IOAS_MAP_READABLE | IOMMU_IOAS_MAP_WRITEABLE, &iova);
	if (err) {
		XNVME_DEBUG("FAILED: iommufd_ioas_map(); err(%d)", err);
		return err;
	}

	if (phys) {
		*phys = iova;
	}

	return 0;
}

static int
xnvme_be_upcie_mem_unmap(const struct xnvme_dev *XNVME_UNUSED(dev), void *XNVME_UNUSED(vaddr))
{
	/* iommufd unmaps by IOVA and length, neither of which a caller handing
	 * back an address has. Left until the registry carries what a range was
	 * mapped as, rather than guessing at it here. */
	return -ENOSYS;
}

#endif

struct xnvme_be_mem g_xnvme_be_upcie_mem = {
	.id = "upcie",
#ifdef XNVME_BE_UPCIE_ENABLED
	.buf_alloc = xnvme_be_upcie_buf_alloc,
	.buf_realloc = xnvme_be_nosys_buf_realloc,
	.buf_free = xnvme_be_upcie_buf_free,
	.buf_vtophys = xnvme_be_upcie_buf_vtophys,
	.mem_map = xnvme_be_upcie_mem_map,
	.mem_unmap = xnvme_be_upcie_mem_unmap,
#else
	.buf_alloc = xnvme_be_nosys_buf_alloc,
	.buf_realloc = xnvme_be_nosys_buf_realloc,
	.buf_free = xnvme_be_nosys_buf_free,
	.buf_vtophys = xnvme_be_nosys_buf_vtophys,
	.mem_map = xnvme_be_nosys_mem_map,
	.mem_unmap = xnvme_be_nosys_mem_unmap,
#endif
};
