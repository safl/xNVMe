/**
 * NVMe/TCP Transport Implementation
 * =================================
 *
 * Preppers
 * --------
 *
 * Prepper-functions for setting up Capsules and PDUs
 *
 * TODO:
 *
 * - Support for digests
 *
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file xnvme_fabrics_tcp.c
 */
#include <arpa/inet.h>
#include <assert.h>     ///< For static_assert(..)
#include <errno.h>      ///< For EINVAL etc.
#include <netinet/in.h> // For sockaddr_in
#include <stdint.h>     ///< For uintX_t
#include <stdio.h>      // For printf()
#include <string.h>     // For memset()
#include <sys/socket.h> // For socket(), connect()
#include <unistd.h>     // For close()
#include <libxnvme.h>

void
xnvme_fabrics_prep_tcp_ic_req(void *buf, uint8_t hpda, bool hdgst_enable, bool ddgst_enable,
			      uint32_t maxr2t)
{
	struct xnvme_fabrics_tcp_ic_req *cmd = buf;

	cmd->ch.type = XNVME_FABRICS_TCP_OPC_ICREQ;
	cmd->ch.hlen = 0x80;
	cmd->ch.plen = 0x80;

	cmd->pfv = 0;
	cmd->hpda = hpda;
	cmd->dgst = hdgst_enable & (ddgst_enable << 1);
	cmd->maxr2t = maxr2t;
}

void
xnvme_fabrics_prep_tcp_ic_resp(void *buf, uint8_t cpda, bool hdgst_enable, bool ddgst_enable,
			       uint32_t maxh2cdata)
{
	struct xnvme_fabrics_tcp_ic_resp *cmd = buf;

	cmd->ch.type = XNVME_FABRICS_TCP_OPC_ICRESP;
	cmd->ch.hlen = 0x80;
	cmd->ch.plen = 0x80;

	cmd->pfv = 0;
	cmd->cpda = cpda;
	cmd->dgst = hdgst_enable & (ddgst_enable << 1);
	cmd->maxh2cdata = maxh2cdata;
}

void
xnvme_fabrics_prep_tcp_h2c_term_req(void *buf, uint16_t fes, uint16_t fei, uint32_t plen)
{
	struct xnvme_fabrics_tcp_h2c_term_req *cmd = buf;

	cmd->ch.type = XNVME_FABRICS_TCP_OPC_H2C_TERM_REQ;
	cmd->ch.hlen = 0x18;
	cmd->ch.plen = plen;

	cmd->fes = fes;
	cmd->fei = fei;
}

void
xnvme_fabrics_prep_tcp_c2h_term_req(void *buf, uint16_t fes, uint16_t fei, uint32_t plen)
{
	struct xnvme_fabrics_tcp_h2c_term_req *cmd = buf;

	cmd->ch.type = XNVME_FABRICS_TCP_OPC_C2H_TERM_REQ;
	cmd->ch.hlen = 0x18;
	cmd->ch.plen = plen;

	cmd->fes = fes;
	cmd->fei = fei;
}

/**
 * Command Capsule PDU Setup
 *
 * NOTE: Command setup is incomplete...
 */
void
xnvme_fabrics_prep_tcp_capsule_cmd(void *buf, bool hdgst_enable, bool ddgst_enable, uint8_t pdo,
				   uint32_t plen, void *ccsqe)
{
	struct xnvme_fabrics_tcp_capsule_cmd *cmd = buf;

	cmd->ch.type = XNVME_FABRICS_TCP_OPC_CAPSULE_CMD;
	cmd->ch.flags = hdgst_enable & (ddgst_enable << 1);
	cmd->ch.hlen = 0x48;
	cmd->ch.pdo = pdo;
	cmd->ch.plen = plen;

	memcpy(cmd->ccsqe, ccsqe, 64);
}

void
xnvme_fabrics_prep_tcp_capsule_resp(void *buf, bool hdgst_enable, uint32_t plen, void *rccqe)
{
	struct xnvme_fabrics_tcp_capsule_resp *cmd = buf;

	cmd->ch.type = XNVME_FABRICS_TCP_OPC_CAPSULE_RESP;
	cmd->ch.flags = hdgst_enable;
	cmd->ch.hlen = 0x18;
	cmd->ch.plen = plen;

	memcpy(cmd->rccqe, rccqe, 64);
}

void
xnvme_fabrics_prep_tcp_h2c_data(void *buf, bool hdgst_enable, bool ddgst_enable, bool last_pdu,
				uint8_t pdo, uint32_t plen, uint16_t cccid, uint16_t ttag,
				uint32_t datao, uint32_t datal)
{
	struct xnvme_fabrics_tcp_h2c_data *cmd = buf;

	cmd->ch.type = XNVME_FABRICS_TCP_OPC_H2C_DATA;
	cmd->ch.flags = hdgst_enable & (ddgst_enable << 1) & (last_pdu << 2);
	cmd->ch.hlen = 0x18;
	cmd->ch.pdo = pdo;
	cmd->ch.plen = plen;

	cmd->cccid = cccid;
	cmd->ttag = ttag;
	cmd->datao = datao;
	cmd->datal = datal;
}

void
xnvme_fabrics_prep_tcp_c2h_data(void *buf, bool hdgst_enable, bool ddgst_enable, bool last_pdu,
				bool success, uint8_t pdo, uint32_t plen, uint16_t cccid,
				uint32_t datao, uint32_t datal)
{
	struct xnvme_fabrics_tcp_c2h_data *cmd = buf;

	cmd->ch.type = XNVME_FABRICS_TCP_OPC_C2H_DATA;
	cmd->ch.flags = hdgst_enable & (ddgst_enable << 1) & (last_pdu << 2) & (success << 3);
	cmd->ch.hlen = 0x18;
	cmd->ch.pdo = pdo;
	cmd->ch.plen = plen;

	cmd->cccid = cccid;
	cmd->datao = datao;
	cmd->datal = datal;
}

void
xnvme_fabrics_prep_tcp_r2t(void *buf, bool hdgst_enable, uint32_t plen, uint16_t cccid,
			   uint16_t ttag, uint32_t r2to, uint32_t r2tl)
{
	struct xnvme_fabrics_tcp_r2t *cmd = buf;

	cmd->ch.type = XNVME_FABRICS_TCP_OPC_R2T;
	cmd->ch.flags = hdgst_enable;
	cmd->ch.hlen = 0x18;
	cmd->ch.plen = plen;

	cmd->cccid = cccid;
	cmd->ttag = ttag;
	cmd->r2to = r2to;
	cmd->r2tl = r2tl;
}

int
xnvme_fabrics_tcp_connect(struct xnvme_fabrics_connection *con)
{
	struct sockaddr_in server_addr = {0};
	ssize_t nbytes;

	server_addr.sin_port = htons(con->params.port);
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = inet_addr(con->params.addr);

	con->socket = socket(AF_INET, SOCK_STREAM, 0);
	if (con->socket < 0) {
		return -errno;
	}

	if (connect(con->socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
		return -errno;
	}

	///< Send initate Connection request (ic_req)
	xnvme_fabrics_prep_tcp_ic_req(con->request_buffer, 0x0, con->params.hdgst_enable,
				      con->params.ddgst_enable, 0x0);
	nbytes = send(con->socket, con->request_buffer, sizeof(struct xnvme_fabrics_tcp_ic_req),
		      0x0);
	if (nbytes < 0) {
		perror("Failed sending: 'ic_req'");
		return -errno;
	}

	///< Receive initiate Connection Response (ic_resp)
	nbytes = recv(con->socket, con->response_buffer, 128, 0x0);
	if (nbytes < 0) {
		perror("Failed receiving: 'ic_resp'");
		return -errno;
	}

	///< Handle Initate Connection Response (ic_resp)
	{
		struct xnvme_fabrics_tcp_ic_resp *resp = con->response_buffer;

		if (resp->pfv) {
			printf("Unsupported pfv: %d", resp->pfv);
			return -EINVAL;
		}

		///< Verify digests
		// con->dgst = resp->dgst;
	}

	return 0;
}

void
xnvme_fabrics_tcp_disconnect(struct xnvme_fabrics_connection *con)
{
	///< TODO: send h2cterm here?
	close(con->socket);
}