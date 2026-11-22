// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

#include <xnvme_be.h>

#ifdef XNVME_BE_LIBURING_DMABUF_ENABLED
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <xnvme_buf.h>
#include <xnvme_be_posix.h>
#include <xnvme_be_liburing_dmabuf.h>

static int
buf_register(const struct xnvme_dev *dev, void *vaddr, size_t nbytes)
{
	enum xnvme_buf_xfer xfer;
	size_t alloc_nbytes;
	void *base;
	int fd, err;

	if ((!vaddr) || (!nbytes)) {
		return -EINVAL;
	}

	// Counted on a registered dma-buf it lies in, host memory too, as from udmabuf
	err = xnvme_be_liburing_dmabuf_ref(dev, vaddr, nbytes);
	if (err != -ENOENT) {
		return err;
	}
	xfer = xnvme_buf_xfer_probe(vaddr);
	if (xfer == XNVME_BUF_XFER_CPU) {
		return xnvme_buf_noreg_register(dev, vaddr, nbytes);
	}

	err = xnvme_buf_export_dmabuf(vaddr, xfer, &base, &alloc_nbytes, &fd);
	if (err) {
		return err;
	}

	err = xnvme_be_liburing_dmabuf_add(dev, base, alloc_nbytes, fd, xfer);
	if (err) {
		close(fd);
		// Added by another thread meanwhile, which also took the reference
		return (err == -EEXIST) ? 0 : err;
	}

	return 0;
}

static int
buf_register_dmabuf(const struct xnvme_dev *dev, int fd, void *vaddr)
{
	off_t size;
	size_t nbytes;
	int own, err;

	size = lseek(fd, 0, SEEK_END);
	if (size <= 0) {
		XNVME_DEBUG("FAILED: lseek(dma-buf), errno: %d", errno);
		return size ? -errno : -EINVAL;
	}
	nbytes = (size_t)size;

	err = xnvme_be_liburing_dmabuf_ref(dev, vaddr, nbytes);
	if (err != -ENOENT) {
		return err;
	}

	own = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	if (own < 0) {
		return -errno;
	}

	// vaddr may name the memory of a runtime, which then moves data in it
	err = xnvme_be_liburing_dmabuf_add(dev, vaddr, nbytes, own, xnvme_buf_xfer_probe(vaddr));
	if (err) {
		close(own);
		// Added by another thread meanwhile, which also took the reference
		return (err == -EEXIST) ? 0 : err;
	}

	return 0;
}

static int
buf_unregister(const struct xnvme_dev *dev, void *vaddr)
{
	int err;

	err = xnvme_be_liburing_dmabuf_unref(dev, vaddr);
	if ((err == -ENOENT) && (xnvme_buf_xfer_probe(vaddr) == XNVME_BUF_XFER_CPU)) {
		return xnvme_buf_noreg_unregister(dev, vaddr);
	}

	return err;
}

static int
buf_get_alignment(const struct xnvme_dev *XNVME_UNUSED(dev), size_t *align)
{
	long pagesize;

	pagesize = sysconf(_SC_PAGESIZE);
	if (pagesize == -1) {
		XNVME_DEBUG("FAILED: sysconf(), errno: %d", errno);
		return -errno;
	}

	// A dma-buf is exported and registered in whole pages
	*align = (size_t)pagesize;

	return 0;
}

struct xnvme_be_mem g_xnvme_be_liburing_mem_dmabuf = {
	.id = "io_uring_dmabuf",
	.buf_alloc = xnvme_be_posix_mem_buf_alloc,
	.buf_realloc = xnvme_be_posix_mem_buf_realloc,
	.buf_free = xnvme_be_posix_mem_buf_free,
	.buf_vtophys = xnvme_be_posix_mem_buf_vtophys,
	.buf_register = buf_register,
	.buf_register_dmabuf = buf_register_dmabuf,
	.buf_unregister = buf_unregister,
	.buf_get_alignment = buf_get_alignment,
};
#endif
