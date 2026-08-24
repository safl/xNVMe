/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file libxnvme_mem.h
 */

/**
 * Map a buffer for IO with the given device
 *
 * The buffer will be aligned to device geometry and DMA allocated if required by the backend for
 * command payloads
 *
 * @note nbytes must be greater than zero and a multiple of minimal granularity
 * @note Unmap the buffer using xnvme_mem_unmap(), where the backend implements it.
 * The uPCIe backend does not: it maps into the address space the device is
 * attached to and has nothing to unmap by, so a mapping there lasts as long as
 * the device is open and repeated mapping consumes address space.
 *
 * @param dev Device handle obtained with xnvme_dev_open()
 * @param vaddr Pointer to start of virtual memory to use as mapped memory
 * @param nbytes The number of bytes to map, starting at vaddr
 *
 * @return On success, 0 is returned. On error, negative `errno` is returned
 */
int
xnvme_mem_map(const struct xnvme_dev *dev, void *vaddr, size_t nbytes);

/**
 * Unmap the given IO buffer mapped with xnvme_mem_map()
 *
 * @note Not every backend implements this; -ENOSYS says the mapping cannot be
 * taken back, not that the call was malformed.
 *
 * @param dev Device handle obtained with xnvme_dev_open()
 * @param buf Pointer to a buffer allocated with xnvme_buf_alloc()
 *
 * @return On sucess, 0 is returned. On error, negative 'errno' is returned
 */
int
xnvme_mem_unmap(const struct xnvme_dev *dev, void *buf);
