/**
 * NVMe/Fabrics Command-Set Implementation
 * =======================================
 *
 * Preppers
 * --------
 *
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file xnvme_fabrics.c
 */
#include <libxnvme.h>
#include <errno.h>

void
xnvme_fabrics_prep_authentification_receive(void *buf)
{
	struct xnvme_fabrics_command_capsule_authentification_receive_sqe *cmd = buf;

	cmd->common.opc = XNVME_FABRICS_OPCODE;
	cmd->common.fctype = XNVME_FABRICS_COMMAND_TYPE_AUTHENTIFICATION_RECEIVE;
}

void
xnvme_fabrics_prep_authentification_send(void *buf)
{
	struct xnvme_fabrics_command_capsule_authentification_send_sqe *cmd = buf;

	cmd->common.opc = XNVME_FABRICS_OPCODE;
	cmd->common.fctype = XNVME_FABRICS_COMMAND_TYPE_AUTHENTIFICATION_SEND;
}

void
xnvme_fabrics_prep_connect_sqe(void *buf, uint16_t qid, uint16_t sqsize)
{
	struct xnvme_fabrics_command_capsule_connect_sqe *cmd = buf;

	cmd->common.opc = XNVME_FABRICS_OPCODE;
	cmd->common.fctype = XNVME_FABRICS_COMMAND_TYPE_CONNECT;

	cmd->qid = qid;
	cmd->sqsize = sqsize;
}

void
xnvme_fabrics_prep_connect_data(void *buf, void *hostid, uint16_t cntlid, void *subnqn,
				void *hostnqn)
{
	struct xnvme_fabrics_command_capsule_connect_data *data = buf;

	data->cntlid = cntlid;

	memcpy(data->hostid, hostid, sizeof(data->hostid));
	memcpy(data->subnqn, subnqn, sizeof(data->subnqn));
	memcpy(data->hostnqn, hostnqn, sizeof(data->hostnqn));
}

void
xnvme_fabrics_prep_disconnect(void *buf)
{
	struct xnvme_fabrics_command_capsule_disconnect_sqe *cmd = buf;

	cmd->common.opc = XNVME_FABRICS_OPCODE;
	cmd->common.fctype = XNVME_FABRICS_COMMAND_TYPE_DISCONNECT;
}

void
xnvme_fabrics_prep_property_get(void *buf)
{
	struct xnvme_fabrics_command_capsule_property_get_sqe *cmd = buf;

	cmd->common.opc = XNVME_FABRICS_OPCODE;
	cmd->common.fctype = XNVME_FABRICS_COMMAND_TYPE_PROPERTY_GET;
}

void
xnvme_fabrics_prep_property_set(void *buf)
{
	struct xnvme_fabrics_command_capsule_property_set_sqe *cmd = buf;

	cmd->common.opc = XNVME_FABRICS_OPCODE;
	cmd->common.fctype = XNVME_FABRICS_COMMAND_TYPE_PROPERTY_SET;
}

void
xnvme_fabrics_connection_term(struct xnvme_fabrics_connection *con)
{
	xnvme_buf_virt_free(con->response_buffer);
	xnvme_buf_virt_free(con->request_buffer);
	free(con);
}

int
xnvme_fabrics_connection_init(struct xnvme_fabrics_connection **con,
			      struct xnvme_fabrics_connection_parameters *params)
{
	*con = calloc(1, sizeof(**con));
	if (!(*con)) {
		return -errno;
	}

	(*con)->request_buffer = xnvme_buf_virt_alloc(4096, 4096);
	if (!(*con)->request_buffer) {
		return errno;
	}

	(*con)->response_buffer = xnvme_buf_virt_alloc(4096, 4096);
	if (!(*con)->response_buffer) {
		xnvme_buf_virt_free((*con)->response_buffer);
		return errno;
	}

	(*con)->params = *params; ///< Copy the given parameters to release ownership

	return 0;
};

void
xnvme_fabrics_disconnect(struct xnvme_fabrics_connection *con)
{
	xnvme_fabrics_tcp_disconnect(con);

	xnvme_fabrics_connection_term(con);
}

struct xnvme_fabrics_connection *
xnvme_fabrics_connect(struct xnvme_fabrics_connection_parameters *params)
{
	struct xnvme_fabrics_connection *con;
	int err;

	err = xnvme_fabrics_connection_init(&con, params);
	if (err) {
		errno = err;
		return NULL;
	}

	// NOTE: this part could dispatch to different transport implementations
	err = xnvme_fabrics_tcp_connect(con);
	if (err) {
		xnvme_fabrics_connection_term(con);
		errno = err;
		return NULL;
	}

	return con;
}