// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

/**
 * I/O through a dma-buf that udmabuf exports of a memfd
 *
 * The dma-buf is registered with xnvme_buf_register_dmabuf() at the address the memfd is mapped
 * at, and its content is checked through that mapping. iommufd's IOMMU_IOAS_MAP_FILE takes the
 * dma-bufs vfio-pci exports; whether it takes this one decides whether the registration needs the
 * dmabuf_import and iommu_map_pa modules. With --alt-uri, the dma-buf is registered through that
 * second controller too, and the first bytes of its namespace are read into the dma-buf and
 * compared with a read into a buffer of xnvme_buf_alloc(); that namespace is only read. Needs
 * /dev/udmabuf. It WRITES the first bytes of the namespace.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE // memfd_create() and the file seals
#endif
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/udmabuf.h>
#include <libxnvme.h>

#define NBYTES (1UL << 20)

static uint64_t
min_u64(uint64_t a, uint64_t b)
{
	return a < b ? a : b;
}

/** A dma-buf of `nbytes` of a sealed memfd, and the memfd mapped at `*vaddr` */
static int
udmabuf_create(uint64_t nbytes, int *memfd, void **vaddr)
{
	struct udmabuf_create create = {0};
	int dev, fd, err;

	*memfd = memfd_create("xnvme-test-udmabuf", MFD_ALLOW_SEALING);
	if (*memfd < 0) {
		return -errno;
	}
	if (ftruncate(*memfd, nbytes) || fcntl(*memfd, F_ADD_SEALS, F_SEAL_SHRINK)) {
		return -errno;
	}
	*vaddr = mmap(NULL, nbytes, PROT_READ | PROT_WRITE, MAP_SHARED, *memfd, 0);
	if (*vaddr == MAP_FAILED) {
		return -errno;
	}

	dev = open("/dev/udmabuf", O_RDWR);
	if (dev < 0) {
		err = -errno;
		xnvme_cli_perr("open(/dev/udmabuf)", err);
		return err;
	}
	create.memfd = *memfd;
	create.offset = 0;
	create.size = nbytes;
	fd = ioctl(dev, UDMABUF_CREATE, &create);
	err = fd < 0 ? -errno : 0;
	close(dev);
	if (err) {
		xnvme_cli_perr("UDMABUF_CREATE", err);
		return err;
	}

	return fd;
}

/** Commands of at most the device's transfer size, over the first nbytes of the namespace */
static int
nvm_rw(struct xnvme_dev *dev, int wr, char *buf, uint64_t nbytes)
{
	const struct xnvme_geo *geo = xnvme_dev_get_geo(dev);
	uint64_t step = geo->mdts_nbytes ? min_u64(geo->mdts_nbytes, 128 * 1024) : 4096;

	for (uint64_t off = 0; off < nbytes; off += step) {
		struct xnvme_cmd_ctx ctx = {0};
		uint64_t len = min_u64(step, nbytes - off);
		uint16_t nlb = (uint16_t)(len / geo->lba_nbytes - 1);
		uint64_t slba = off / geo->lba_nbytes;
		uint32_t nsid = xnvme_dev_get_nsid(dev);
		int err;

		xnvme_dev_get_cmd_ctx(dev, &ctx);
		if (wr) {
			struct xnvme_cmd_prep_nvm_write_args args = {
				.nsid = nsid, .slba = slba, .nlb = nlb};

			err = xnvme_cmd_prep_nvm_write(&ctx.cmd, &args);
		} else {
			struct xnvme_cmd_prep_nvm_read_args args = {
				.nsid = nsid, .slba = slba, .nlb = nlb};

			err = xnvme_cmd_prep_nvm_read(&ctx.cmd, &args);
		}
		if (!err) {
			err = xnvme_cmd_pass(&ctx, buf + off, len, NULL, 0);
		}
		if (err || xnvme_cmd_ctx_cpl_status(&ctx)) {
			xnvme_cli_pinf("FAILED: %s at offset %" PRIu64 "; err(%d)",
				       wr ? "write" : "read", off, err);
			xnvme_cmd_ctx_pr(&ctx, XNVME_PR_DEF);
			return err ? err : -EIO;
		}
	}

	return 0;
}

static int
test_rw(struct xnvme_cli *cli)
{
	struct xnvme_dev *dev = cli->args.dev;
	char *pattern = NULL, *hbuf = NULL;
	void *vaddr = MAP_FAILED;
	int memfd = -1, fd = -1, registered = 0, err;

	fd = udmabuf_create(NBYTES, &memfd, &vaddr);
	if (fd < 0) {
		err = fd;
		goto exit;
	}

	err = xnvme_buf_register_dmabuf(dev, fd, vaddr);
	if (err) {
		xnvme_cli_perr("xnvme_buf_register_dmabuf()", err);
		goto exit;
	}
	registered = 1;

	pattern = malloc(NBYTES);
	hbuf = xnvme_buf_alloc(dev, NBYTES);
	if (!pattern || !hbuf) {
		err = -ENOMEM;
		goto exit;
	}

	// The drive into the dma-buf
	for (uint64_t i = 0; i < NBYTES; ++i) {
		pattern[i] = (char)((i * 131) ^ (i >> 9));
	}
	memcpy(hbuf, pattern, NBYTES);
	memset(vaddr, 0, NBYTES);
	err = nvm_rw(dev, 1, hbuf, NBYTES);
	err = err ? err : nvm_rw(dev, 0, vaddr, NBYTES);
	if (err) {
		goto exit;
	}
	if (memcmp(vaddr, pattern, NBYTES)) {
		xnvme_cli_pinf("FAILED: the dma-buf differs from the namespace read into it");
		err = -EIO;
		goto exit;
	}
	xnvme_cli_pinf("LGTM: read from the namespace into the dma-buf");

	// The dma-buf onto the drive
	for (uint64_t i = 0; i < NBYTES; ++i) {
		pattern[i] = (char)~((i * 7) ^ (i >> 11));
	}
	memcpy(vaddr, pattern, NBYTES);
	memset(hbuf, 0, NBYTES);
	err = nvm_rw(dev, 1, vaddr, NBYTES);
	err = err ? err : nvm_rw(dev, 0, hbuf, NBYTES);
	if (err) {
		goto exit;
	}
	if (memcmp(hbuf, pattern, NBYTES)) {
		xnvme_cli_pinf("FAILED: the namespace differs from the dma-buf written to it");
		err = -EIO;
		goto exit;
	}
	xnvme_cli_pinf("LGTM: wrote from the dma-buf onto the namespace");

	// Registered through a second controller as well, and read into through that one
	if (cli->args.alt_uri) {
		struct xnvme_opts opts;
		struct xnvme_dev *dev2;
		char *hbuf2;

		xnvme_opts_set_defaults(&opts);
		err = xnvme_cli_to_opts(cli, &opts);
		err = err ? err : xnvme_dev_open_uri(cli->args.alt_uri, &opts, &dev2);
		if (err) {
			xnvme_cli_perr("xnvme_dev_open_uri(--alt-uri)", err);
			goto exit;
		}
		// The namespace behind --alt-uri is only read, so it is compared with itself
		hbuf2 = xnvme_buf_alloc(dev2, NBYTES);
		err = hbuf2 ? nvm_rw(dev2, 0, hbuf2, NBYTES) : -ENOMEM;
		err = err ? err : xnvme_buf_register_dmabuf(dev2, fd, vaddr);
		if (!err) {
			memset(vaddr, 0, NBYTES);
			err = nvm_rw(dev2, 0, vaddr, NBYTES);
			err = err ? err : (memcmp(vaddr, hbuf2, NBYTES) ? -EIO : 0);
			err = err ? err : xnvme_buf_unregister(dev2, vaddr);
		}
		xnvme_buf_free(dev2, hbuf2);
		xnvme_dev_close(dev2);
		if (err) {
			xnvme_cli_perr("the dma-buf through --alt-uri", err);
			goto exit;
		}
		xnvme_cli_pinf("LGTM: read into the dma-buf through %s", cli->args.alt_uri);
	}

	err = xnvme_buf_unregister(dev, vaddr);
	registered = 0;
	if (err) {
		xnvme_cli_perr("xnvme_buf_unregister()", err);
		goto exit;
	}
	xnvme_cli_pinf("LGTM: unregistered");

exit:
	if (registered) {
		xnvme_buf_unregister(dev, vaddr);
	}
	if (fd >= 0) {
		close(fd);
	}
	if (vaddr != MAP_FAILED) {
		munmap(vaddr, NBYTES);
	}
	if (memfd >= 0) {
		close(memfd);
	}
	xnvme_buf_free(dev, hbuf);
	free(pattern);

	return err;
}

/**
 * The dma-buf registered under a range of addresses reserved with PROT_NONE, which neither the
 * test nor the library may read or write, as a caller does whose memory the CPU cannot reach; the
 * bytes are checked through the memfd's own mapping
 */
static int
test_reserved(struct xnvme_cli *cli)
{
	struct xnvme_dev *dev = cli->args.dev;
	char *pattern = NULL, *hbuf = NULL;
	void *mapped = MAP_FAILED, *reserved = MAP_FAILED;
	int memfd = -1, fd = -1, registered = 0, err;

	fd = udmabuf_create(NBYTES, &memfd, &mapped);
	if (fd < 0) {
		err = fd;
		goto exit;
	}
	reserved =
		mmap(NULL, NBYTES, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (reserved == MAP_FAILED) {
		err = -errno;
		goto exit;
	}

	err = xnvme_buf_register_dmabuf(dev, fd, reserved);
	if (err) {
		xnvme_cli_perr("xnvme_buf_register_dmabuf(PROT_NONE)", err);
		goto exit;
	}
	registered = 1;

	pattern = malloc(NBYTES);
	hbuf = xnvme_buf_alloc(dev, NBYTES);
	if (!pattern || !hbuf) {
		err = -ENOMEM;
		goto exit;
	}

	// The dma-buf onto the drive, then the drive into the dma-buf
	for (uint64_t i = 0; i < NBYTES; ++i) {
		pattern[i] = (char)((i * 13) ^ (i >> 9));
	}
	memcpy(mapped, pattern, NBYTES);
	memset(hbuf, 0, NBYTES);
	err = nvm_rw(dev, 1, reserved, NBYTES);
	err = err ? err : nvm_rw(dev, 0, hbuf, NBYTES);
	if (err) {
		goto exit;
	}
	if (memcmp(hbuf, pattern, NBYTES)) {
		xnvme_cli_pinf("FAILED: the namespace differs from the dma-buf written to it");
		err = -EIO;
		goto exit;
	}
	memset(mapped, 0, NBYTES);
	err = nvm_rw(dev, 0, reserved, NBYTES);
	if (err) {
		goto exit;
	}
	if (memcmp(mapped, pattern, NBYTES)) {
		xnvme_cli_pinf("FAILED: the dma-buf differs from the namespace read into it");
		err = -EIO;
		goto exit;
	}

	err = xnvme_buf_unregister(dev, reserved);
	registered = 0;
	if (err) {
		xnvme_cli_perr("xnvme_buf_unregister()", err);
		goto exit;
	}
	xnvme_cli_pinf("LGTM: written from and read into a dma-buf registered under PROT_NONE");

exit:
	if (registered) {
		xnvme_buf_unregister(dev, reserved);
	}
	if (reserved != MAP_FAILED) {
		munmap(reserved, NBYTES);
	}
	if (fd >= 0) {
		close(fd);
	}
	if (mapped != MAP_FAILED) {
		munmap(mapped, NBYTES);
	}
	if (memfd >= 0) {
		close(memfd);
	}
	xnvme_buf_free(dev, hbuf);
	free(pattern);

	return err;
}

#define MANY 100

/** More dma-bufs than a registration table starts out with, each read into and compared */
static int
test_many(struct xnvme_cli *cli)
{
	struct xnvme_dev *dev = cli->args.dev;
	const struct xnvme_geo *geo = xnvme_dev_get_geo(dev);
	uint64_t nbytes = geo->lba_nbytes > 4096 ? geo->lba_nbytes : 4096;
	int memfd[MANY], fd[MANY], registered[MANY] = {0};
	void *vaddr[MANY];
	char *hbuf = NULL;
	int err = 0;

	for (int i = 0; i < MANY; ++i) {
		memfd[i] = -1;
		fd[i] = -1;
		vaddr[i] = MAP_FAILED;
	}

	hbuf = xnvme_buf_alloc(dev, nbytes);
	if (!hbuf) {
		err = -ENOMEM;
		goto exit;
	}
	err = nvm_rw(dev, 0, hbuf, nbytes);
	if (err) {
		goto exit;
	}

	for (int i = 0; i < MANY; ++i) {
		fd[i] = udmabuf_create(nbytes, &memfd[i], &vaddr[i]);
		if (fd[i] < 0) {
			err = fd[i];
			goto exit;
		}
		err = xnvme_buf_register_dmabuf(dev, fd[i], vaddr[i]);
		if (err) {
			xnvme_cli_perr("xnvme_buf_register_dmabuf()", err);
			goto exit;
		}
		registered[i] = 1;
	}
	for (int i = 0; i < MANY; ++i) {
		memset(vaddr[i], 0, nbytes);
		err = nvm_rw(dev, 0, vaddr[i], nbytes);
		if (err) {
			goto exit;
		}
		if (memcmp(vaddr[i], hbuf, nbytes)) {
			xnvme_cli_pinf(
				"FAILED: dma-buf %d differs from the namespace read into it", i);
			err = -EIO;
			goto exit;
		}
	}
	for (int i = 0; i < MANY; ++i) {
		err = xnvme_buf_unregister(dev, vaddr[i]);
		registered[i] = 0;
		if (err) {
			xnvme_cli_perr("xnvme_buf_unregister()", err);
			goto exit;
		}
	}
	xnvme_cli_pinf("LGTM: %d dma-bufs registered, read into and unregistered", MANY);

exit:
	for (int i = 0; i < MANY; ++i) {
		if (registered[i]) {
			xnvme_buf_unregister(dev, vaddr[i]);
		}
		if (fd[i] >= 0) {
			close(fd[i]);
		}
		if (vaddr[i] != MAP_FAILED) {
			munmap(vaddr[i], nbytes);
		}
		if (memfd[i] >= 0) {
			close(memfd[i]);
		}
	}
	xnvme_buf_free(dev, hbuf);

	return err;
}

//
// Command-Line Interface (CLI) definition
//
static struct xnvme_cli_sub g_subs[] = {
	{
		"rw",
		"Read into and write from a dma-buf that udmabuf exports of a memfd",
		"Read into and write from a dma-buf that udmabuf exports of a memfd",
		test_rw,
		{
			{XNVME_CLI_OPT_POSA_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_URI, XNVME_CLI_POSA},

			{XNVME_CLI_OPT_NON_POSA_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_ALT_URI, XNVME_CLI_LOPT},
			XNVME_CLI_SYNC_OPTS,
		},
	},
	{
		"reserved",
		"Write and read through a dma-buf registered under a PROT_NONE range",
		"Write and read through a dma-buf registered under a PROT_NONE range",
		test_reserved,
		{
			{XNVME_CLI_OPT_POSA_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_URI, XNVME_CLI_POSA},

			{XNVME_CLI_OPT_NON_POSA_TITLE, XNVME_CLI_SKIP},
			XNVME_CLI_SYNC_OPTS,
		},
	},
	{
		"many",
		"Register more dma-bufs than a registration table starts out with",
		"Register more dma-bufs than a registration table starts out with",
		test_many,
		{
			{XNVME_CLI_OPT_POSA_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_URI, XNVME_CLI_POSA},

			{XNVME_CLI_OPT_NON_POSA_TITLE, XNVME_CLI_SKIP},
			XNVME_CLI_SYNC_OPTS,
		},
	},
};

static struct xnvme_cli g_cli = {
	.title = "Test I/O through a dma-buf udmabuf exports",
	.descr_short = "Test I/O through a dma-buf udmabuf exports",
	.subs = g_subs,
	.nsubs = sizeof g_subs / sizeof(*g_subs),
};

int
main(int argc, char **argv)
{
	return xnvme_cli_run(&g_cli, argc, argv, XNVME_CLI_INIT_DEV_OPEN);
}
