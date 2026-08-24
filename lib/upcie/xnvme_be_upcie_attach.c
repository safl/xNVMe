// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

/**
 * Attaching to a runtime another process owns
 *
 * The counterpart to serving. Where the owner allocated memory and opened a
 * controller, this receives descriptors for both and builds a view of them: a
 * mapping of the same memory, a translation table for this process's
 * addresses, and a mapping of the BAR so that queues granted later can be rung
 * from here.
 *
 * Nothing is allocated and nothing is owned. What this process wants from the
 * heap it asks for, and what it is granted it hands back.
 */
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <libxnvme.h>
#include <xnvme_be.h>
#include <xnvme_dev.h>

#ifdef XNVME_BE_UPCIE_ENABLED
#include <xnvme_be_upcie.h>

/**
 * Where consumers of a given shm_id look for the primary
 *
 * The tool that serves derives the same path from the same identifier; see the
 * design note on why it is a filesystem path.
 */
void
xnvme_be_upcie_socket_path(uint32_t shm_id, char *path, size_t nbytes)
{
	snprintf(path, nbytes, "/tmp/xnvme-homi-%u.sock", shm_id);
}

/**
 * Ask the owner for something and wait for the answer
 *
 * @param msg The request, replaced by the reply
 * @param fds Pre-allocated array of NVME_DELEGATE_FDS_MAX, or NULL
 * @param nfds Set to how many descriptors arrived
 *
 * @return 0 on success, the owner's status when it refused, negative errno on error
 */
int
xnvme_be_upcie_ask(struct nvme_delegate_msg *msg, int *fds, uint32_t *nfds)
{
	if (g_upcie_rte.attached.sock < 0) {
		return -ENOTCONN;
	}

	return nvme_delegate_request(g_upcie_rte.attached.sock, msg, fds, nfds);
}

/**
 * Build this process's view of a runtime somebody else owns
 *
 * @param shm_id Identifies the runtime, and with it the socket
 *
 * @return 0 on success, -ENOENT when nobody is serving, negative errno on error
 */
int
xnvme_be_upcie_attach(uint32_t shm_id)
{
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	struct nvme_delegate_msg msg = {0};
	const struct hostmem_shared_desc *desc;
	const struct nvme_runtime_record *record;
	int fds[NVME_DELEGATE_FDS_MAX];
	uint32_t nfds = 0;
	char path[256] = {0};
	void *heap_base = MAP_FAILED;
	void *bar0 = MAP_FAILED;
	int sock, err;

	xnvme_be_upcie_socket_path(shm_id, path, sizeof(path));

	sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock < 0) {
		return -errno;
	}
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr))) {
		/* Nobody is serving this identifier. That is not an error
		 * here: the caller decides whether to become the owner. */
		err = -errno;
		close(sock);
		return (err == -ENOENT) || (err == -ECONNREFUSED) ? -ENOENT : err;
	}

	g_upcie_rte.attached.sock = sock;

	msg.op = NVME_DELEGATE_OP_ATTACH;
	err = xnvme_be_upcie_ask(&msg, fds, &nfds);
	if (err || (nfds != 2)) {
		XNVME_DEBUG("FAILED: attach; err(%d) nfds(%u)", err, nfds);
		/* Whatever arrived is installed in this process already, so it is
		 * this process's to close, even when the reply was not the one
		 * asked for. */
		for (uint32_t i = 0; i < nfds; ++i) {
			close(fds[i]);
		}
		err = err ? err : -EPROTO;
		goto failed;
	}

	heap_base = mmap(NULL, msg.u.attach.heap_nbytes, PROT_READ | PROT_WRITE, MAP_SHARED,
			 fds[0], 0);
	close(fds[0]);
	if (heap_base == MAP_FAILED) {
		XNVME_DEBUG("FAILED: mmap(heap); errno(%d)", errno);
		err = -errno;
		close(fds[1]);
		goto failed;
	}

	bar0 = mmap(NULL, msg.u.attach.bar0_nbytes, PROT_READ | PROT_WRITE, MAP_SHARED, fds[1], 0);
	close(fds[1]);
	if (bar0 == MAP_FAILED) {
		XNVME_DEBUG("FAILED: mmap(BAR0); errno(%d)", errno);
		err = -errno;
		goto failed;
	}

	record = (const struct nvme_runtime_record *)((char *)heap_base +
						      msg.u.attach.record_offset);
	if (record->version != NVME_RUNTIME_RECORD_VERSION) {
		XNVME_DEBUG("FAILED: record version(%u), expected(%u)", record->version,
			    NVME_RUNTIME_RECORD_VERSION);
		err = -EPROTO;
		goto failed;
	}

	/* The owner read the physical addresses once, when it allocated; this
	 * process could not, so it takes them from where they were left. */
	desc = (const struct hostmem_shared_desc *)((char *)heap_base + record->desc_offset);

	err = dmamem_from_shared_hostmem(&g_upcie_rte.mem.dmem, heap_base, desc,
					 xnvme_be_upcie_va_bits());
	if (err) {
		XNVME_DEBUG("FAILED: dmamem_from_shared_hostmem(); err(%d)", err);
		goto failed;
	}
	g_upcie_rte.mem.dmem_alive = 1;

	g_upcie_rte.attached.heap_base = heap_base;
	g_upcie_rte.attached.heap_nbytes = msg.u.attach.heap_nbytes;
	g_upcie_rte.attached.bar0 = bar0;
	g_upcie_rte.attached.bar0_nbytes = msg.u.attach.bar0_nbytes;
	g_upcie_rte.attached.record = record;
	g_upcie_rte.attached.alive = 1;

	return 0;

failed:
	if (bar0 != MAP_FAILED) {
		munmap(bar0, msg.u.attach.bar0_nbytes);
	}
	if (heap_base != MAP_FAILED) {
		munmap(heap_base, msg.u.attach.heap_nbytes);
	}
	close(g_upcie_rte.attached.sock);
	g_upcie_rte.attached.sock = -1;

	return err;
}

/**
 * Let go of a runtime this process attached to
 *
 * Closing the socket is what tells the owner to reclaim whatever is still
 * held, so it goes last and it always goes.
 */
void
xnvme_be_upcie_detach(void)
{
	if (!g_upcie_rte.attached.alive) {
		return;
	}

	if (g_upcie_rte.mem.dmem_alive) {
		dmamem_destroy(&g_upcie_rte.mem.dmem);
		g_upcie_rte.mem.dmem_alive = 0;
	}
	if (g_upcie_rte.attached.bar0) {
		munmap(g_upcie_rte.attached.bar0, g_upcie_rte.attached.bar0_nbytes);
	}
	if (g_upcie_rte.attached.heap_base) {
		munmap(g_upcie_rte.attached.heap_base, g_upcie_rte.attached.heap_nbytes);
	}
	if (g_upcie_rte.attached.sock >= 0) {
		close(g_upcie_rte.attached.sock);
	}

	memset(&g_upcie_rte.attached, 0, sizeof(g_upcie_rte.attached));
	g_upcie_rte.attached.sock = -1;
}
#endif
