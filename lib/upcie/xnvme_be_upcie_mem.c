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

	dmamem_heap_free(&g_upcie_rte.mem.heap, offset);
}

int
xnvme_be_upcie_buf_vtophys(const struct xnvme_dev *XNVME_UNUSED(dev), void *buf, uint64_t *phys)
{
	*phys = dmamem_va_to_iova(&g_upcie_rte.mem.dmem, buf);

	return 0;
}

/**
 * Register caller memory with a registry-backed dmamem
 *
 * Shared by the GPU backends, which differ only in which dmamem the memory
 * belongs to.
 *
 * @return 0 on success, negative errno on failure.
 */
int
xnvme_be_upcie_dmamem_map(struct dmamem *dmem, void *vaddr, size_t nbytes, uint64_t *phys)
{
	int err;

	err = dmamem_register(dmem, vaddr, nbytes);
	if (err) {
		XNVME_DEBUG("FAILED: dmamem_register(); err(%d)", err);
		return err;
	}

	if (phys) {
		*phys = dmamem_va_to_iova(dmem, vaddr);
		if (!*phys) {
			XNVME_DEBUG("FAILED: registered but unresolvable; vaddr(%p)", vaddr);
			err = dmamem_unregister(dmem, vaddr);
			if (err) {
				XNVME_DEBUG("FAILED: dmamem_unregister(); err(%d)", err);
			}
			return -EINVAL;
		}
	}

	return 0;
}

int
xnvme_be_upcie_dmamem_unmap(struct dmamem *dmem, void *vaddr)
{
	return dmamem_unregister(dmem, vaddr);
}

/**
 * Register caller-allocated host memory for DMA
 *
 * Under vfio the device translates through an address space, so the range is
 * mapped into it. Under uio_pci_generic the device consumes physical addresses,
 * which uPCIe does not read for memory it did not allocate, so registration is
 * refused rather than resolving to something that is not the caller's.
 *
 * @return 0 on success, negative errno on failure.
 */
static int
xnvme_be_upcie_mem_map(const struct xnvme_dev *XNVME_UNUSED(dev), void *vaddr, size_t nbytes,
		       uint64_t *phys)
{
	uint64_t iova = 0;
	int err;

	if (g_upcie_rte.mode != XNVME_BE_UPCIE_MODE_VFIO_CDEV) {
		XNVME_DEBUG("FAILED: mapping caller memory needs an address space to map into");
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

/**
 * Unregister what xnvme_be_upcie_mem_map() registered
 *
 * @return 0 on success, negative errno on failure.
 */
static int
xnvme_be_upcie_mem_unmap(const struct xnvme_dev *XNVME_UNUSED(dev), void *XNVME_UNUSED(vaddr))
{
	/* iommufd unmaps by IOVA and length, neither of which a caller handing
	 * back an address has. */
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
