// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

/**
 * Serving consumers of a runtime this process owns
 *
 * The loop is small because the pieces are elsewhere: uPCIe defines what
 * passes between the two, and the backend knows how to describe a runtime and
 * how to create a queue. What is left here is the bookkeeping nobody else can
 * do, which is remembering what each consumer holds so that a disconnect can
 * release it.
 *
 * A disconnect is the only reliable signal there is. A consumer that exits
 * cleanly hands its queues back; one that is killed mid-command does not, and
 * the socket closing is what says so.
 */
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <libxnvme.h>
#include <libxnvme_mproc.h>
#include <xnvme_be.h>
#include <xnvme_dev.h>

#ifdef XNVME_BE_UPCIE_ENABLED
#include <xnvme_be_upcie.h>

#define SERVE_CLIENTS_MAX 32
#define SERVE_QUEUES_PER_CLIENT 8

#define SERVE_LOANS_PER_CLIENT 32

struct serve_client {
	int sock;
	uint32_t qids[SERVE_QUEUES_PER_CLIENT];
	int nqids;
	uint64_t loans[SERVE_LOANS_PER_CLIENT]; ///< Heap offsets handed out
	int nloans;
};

/* What a status request reports. Counted rather than derived, because the
 * answer is wanted while the loop is between polls. */
static int serve_nclients;
static int serve_nqueues;

/* What the controller allocated, asked once: it cannot change while the
 * controller is up, and a status probe should not cost an admin command. */
static uint32_t serve_nsq_total;
static uint32_t serve_ncq_total;

/* The counters above are process-wide, so two loops would report each other's
 * numbers and decrement each other's totals. One at a time. */
static int serve_running;

/**
 * Release everything a consumer held, queues before the memory behind them
 */
static void
serve_client_release(struct xnvme_dev *dev, struct serve_client *client)
{
	/* Queues first: the controller has to stop being able to reach an
	 * address before that address stops meaning anything. */
	for (int i = 0; i < client->nqids; ++i) {
		int err = xnvme_be_upcie_ungrant(dev, client->qids[i]);

		if (err) {
			XNVME_DEBUG("FAILED: ungrant(qid(%u)); err(%d)", client->qids[i], err);
		}
	}

	for (int i = 0; i < client->nloans; ++i) {
		int err = xnvme_be_upcie_reclaim(client->loans[i]);

		if (err) {
			XNVME_DEBUG("FAILED: reclaim(0x%" PRIx64 "); err(%d)", client->loans[i],
				    err);
		}
	}

	serve_nqueues -= client->nqids;

	if (client->sock >= 0) {
		close(client->sock);
		serve_nclients--;
	}

	memset(client, 0, sizeof(*client));
	client->sock = -1;
}

/**
 * Answer one message, and record what the answer handed out
 */
static int
serve_one(struct xnvme_dev *dev, struct serve_client *client,
	  const struct xnvme_be_upcie_export *exported)
{
	struct nvme_delegate_msg msg = {0};
	struct nvme_delegate_msg reply = {0};
	int fds[NVME_DELEGATE_FDS_MAX];
	uint32_t nfds = 0;
	int err;

	err = nvme_delegate_msg_recv(client->sock, &msg, NULL, NULL);
	if (err) {
		return err; ///< -ENOTCONN when the consumer is gone
	}

	reply.op = msg.op;
	reply.version = NVME_DELEGATE_VERSION;

	if (msg.version != NVME_DELEGATE_VERSION) {
		XNVME_DEBUG("FAILED: consumer speaks version(%u)", msg.version);
		reply.status = -EPROTO;
		return nvme_delegate_msg_send(client->sock, &reply, NULL, 0);
	}

	switch (msg.op) {
	case NVME_DELEGATE_OP_ATTACH:
		reply.u.attach.record_offset = exported->record_offset;
		reply.u.attach.heap_nbytes = exported->heap_nbytes;
		reply.u.attach.bar0_nbytes = exported->bar0_nbytes;
		fds[nfds++] = exported->heap_fd;
		fds[nfds++] = exported->bar0_fd;
		break;

	case NVME_DELEGATE_OP_GRANT: {
		struct xnvme_be_upcie_qgrant grant = {0};

		if (client->nqids == SERVE_QUEUES_PER_CLIENT) {
			reply.status = -ENOSPC;
			break;
		}

		reply.status = xnvme_be_upcie_grant(dev, msg.u.queue.depth, &grant);
		if (reply.status) {
			break;
		}

		client->qids[client->nqids++] = grant.qid;
		serve_nqueues++;

		reply.u.queue.grant.sq_offset = grant.sq_offset;
		reply.u.queue.grant.cq_offset = grant.cq_offset;
		reply.u.queue.grant.prp_offset = grant.prp_offset;
		reply.u.queue.grant.qid = grant.qid;
		reply.u.queue.grant.depth = grant.depth;
	} break;

	case NVME_DELEGATE_OP_RELEASE:
		reply.status = -ENOENT;
		for (int i = 0; i < client->nqids; ++i) {
			if (client->qids[i] != msg.u.release.qid) {
				continue;
			}

			reply.status = xnvme_be_upcie_ungrant(dev, msg.u.release.qid);
			client->qids[i] = client->qids[--client->nqids];
			serve_nqueues--;
			break;
		}
		break;

	case NVME_DELEGATE_OP_ALLOC: {
		uint64_t offset = 0;

		if (client->nloans == SERVE_LOANS_PER_CLIENT) {
			reply.status = -ENOSPC;
			break;
		}

		reply.status = xnvme_be_upcie_lend(msg.u.mem.nbytes, &offset);
		if (reply.status) {
			break;
		}

		client->loans[client->nloans++] = offset;
		reply.u.mem.offset = offset;
	} break;

	case NVME_DELEGATE_OP_FREE:
		reply.status = -ENOENT;
		for (int i = 0; i < client->nloans; ++i) {
			if (client->loans[i] != msg.u.mem.offset) {
				continue;
			}

			reply.status = xnvme_be_upcie_reclaim(msg.u.mem.offset);
			client->loans[i] = client->loans[--client->nloans];
			break;
		}
		break;

	case NVME_DELEGATE_OP_STATUS:
		/* Asking is not attaching, so whoever is asking does not count
		 * itself among the consumers. */
		reply.u.status.nconsumers = (uint32_t)(serve_nclients - 1);
		reply.u.status.nqueues = (uint32_t)serve_nqueues;
		reply.u.status.nsq_total = serve_nsq_total;
		reply.u.status.ncq_total = serve_ncq_total;
		snprintf(reply.u.status.bdf, sizeof(reply.u.status.bdf), "%s", exported->uri);
		break;

	case NVME_DELEGATE_OP_ADMIN:
		if (!nvme_delegate_admin_permitted(&msg.u.admin.cmd)) {
			reply.status = -EPERM;
			break;
		}
		reply.status = xnvme_be_upcie_admin(dev, &msg.u.admin.cmd, &reply.u.admin.cpl);
		break;

	default:
		reply.status = -ENOSYS;
		break;
	}

	reply.nfds = nfds;

	return nvme_delegate_msg_send(client->sock, &reply, nfds ? fds : NULL, nfds);
}

int
xnvme_mproc_serve(struct xnvme_dev **devs, int ndevs, const char *path,
		  volatile sig_atomic_t *stop)
{
	struct xnvme_be_upcie_export exported = {0};
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	struct serve_client clients[SERVE_CLIENTS_MAX];
	struct pollfd pfds[SERVE_CLIENTS_MAX + 1];
	int listener;
	int err;

	if (!devs || (ndevs < 1) || !path || !stop) {
		return -EINVAL;
	}
	if (serve_running) {
		XNVME_DEBUG("FAILED: this process is already serving");
		return -EBUSY;
	}
	if (ndevs > 1) {
		XNVME_DEBUG("FAILED: serving more than one device is not implemented");
		return -ENOSYS;
	}

	if (strcmp(devs[0]->be.attr.name, "upcie")) {
		XNVME_DEBUG("FAILED: serving is uPCIe's; be(%s) has its own arrangement",
			    devs[0]->be.attr.name);
		return -ENOSYS;
	}

	serve_running = 1;

	err = xnvme_be_upcie_export(devs[0], &exported);
	if (err) {
		XNVME_DEBUG("FAILED: xnvme_be_upcie_export(); err(%d)", err);
		serve_running = 0;
		return err;
	}

	/* Asking is the owner's to do, since a consumer has no controller to
	 * submit on. A refusal is not fatal: the totals stay zero, which the
	 * reply defines as "did not answer". */
	{
		struct xnvme_cmd_ctx ctx = xnvme_cmd_ctx_from_dev(devs[0]);

		if (xnvme_adm_gfeat_nqueues(&ctx, &serve_nsq_total, &serve_ncq_total)) {
			XNVME_DEBUG("INFO: controller did not report its queue counts");
			serve_nsq_total = 0;
			serve_ncq_total = 0;
		}
	}

	memset(clients, 0, sizeof(clients));
	for (int i = 0; i < SERVE_CLIENTS_MAX; ++i) {
		clients[i].sock = -1;
	}

	unlink(path);
	/* Non-blocking, because accept() is drained in a loop: on a blocking
	 * listener the call after the last waiting connection waits for one
	 * that is not coming, and the loop never gets back to poll(). */
	listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (listener < 0) {
		err = -errno;
		xnvme_be_upcie_unexport(&exported);
		serve_running = 0;
		return err;
	}
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

	/* Deep enough to absorb a burst of probes. Something asking whether a
	 * runtime is alive gets its answer from connecting, so a refused
	 * connection reads as a runtime that is not there. */
	if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) ||
	    listen(listener, SERVE_CLIENTS_MAX * 8)) {
		err = -errno;
		close(listener);
		xnvme_be_upcie_unexport(&exported);
		serve_running = 0;
		return err;
	}

	while (!*stop) {
		int nfds = 0;

		pfds[nfds].fd = listener;
		pfds[nfds].events = POLLIN;
		nfds++;

		for (int i = 0; i < SERVE_CLIENTS_MAX; ++i) {
			if (clients[i].sock < 0) {
				continue;
			}
			pfds[nfds].fd = clients[i].sock;
			pfds[nfds].events = POLLIN;
			nfds++;
		}

		/* A timeout rather than a signal mask: the caller's flag is
		 * what ends this, and a second of latency on the way out costs
		 * nobody anything. */
		if (poll(pfds, nfds, 1000) < 0) {
			if (errno == EINTR) {
				continue;
			}
			err = -errno;
			break;
		}

		while (pfds[0].revents & POLLIN) {
			/* Accepted blocking on purpose: reading a message waits
			 * for the rest of one, and a consumer that pauses
			 * before writing has not gone away. */
			int sock = accept4(listener, NULL, NULL, 0);

			if (sock < 0) {
				break; ///< Drained, or nothing was waiting after all
			}
			{
				int slot;

				for (slot = 0; slot < SERVE_CLIENTS_MAX; ++slot) {
					if (clients[slot].sock < 0) {
						break;
					}
				}
				if (slot == SERVE_CLIENTS_MAX) {
					close(sock);
				} else {
					clients[slot].sock = sock;
					serve_nclients++;
				}
			}
		}

		for (int i = 0; i < SERVE_CLIENTS_MAX; ++i) {
			if (clients[i].sock < 0) {
				continue;
			}

			for (int p = 1; p < nfds; ++p) {
				if ((pfds[p].fd != clients[i].sock) || !pfds[p].revents) {
					continue;
				}

				if (serve_one(devs[0], &clients[i], &exported)) {
					serve_client_release(devs[0], &clients[i]);
				}
				break;
			}
		}
	}

	for (int i = 0; i < SERVE_CLIENTS_MAX; ++i) {
		if (clients[i].sock >= 0) {
			serve_client_release(devs[0], &clients[i]);
		}
	}

	close(listener);
	unlink(path);
	xnvme_be_upcie_unexport(&exported);
	serve_running = 0;

	return err;
}
#else
int
xnvme_mproc_serve(struct xnvme_dev **XNVME_UNUSED(devs), int XNVME_UNUSED(ndevs),
		  const char *XNVME_UNUSED(path), volatile sig_atomic_t *XNVME_UNUSED(stop))
{
	return -ENOSYS;
}
#endif
