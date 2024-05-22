/**
Simons NVMe Over TCP
====================

The NVM Express Base Specification Revision 2.0d January 11th, 2024 provides the definitions
represented here and a basis for figure-references in docstrings.

This is a implementation of NVMe-Over-TCP. The motivation for making this is two-fold.

1) To provide a user-space implementation with minimal dependencies as an alternative to the SPDK
NVMe drivers.

2) An exercise to determine how generated code, based on the Spex and Yace initiatives should be
able to emit.

Thus, this implementation aims for simplicity, interoperability, and portability not for
performance. Because, the SPDK NVMe-oF drivers already have you covered when it comes to
performance and I would like to contribute something to the software eco-system around NVMe instead
of competing and causing fragmentation.

This implementation is a rough draft done from reading the spec and translating that into the
data-structures and logic found below.

Use-cases include things like:

Fire up your pyTorch workload and have to do data-load over the network using using the xNVMe
Python bindings to the KV-API, loading data from your network-attached appliances serving
up .npz / .tfrecord etc. via the NVMe KV-API.

Running a Fabrics client on operating systems not providing the infrastructure, such as macOS and
Windows. How cool is it to sit on your apple silicon and talking with network-attached NVMe device?


Setup and Initialization
------------------------

...

Queues
------

* One TCP connection per queue-pair
  - One connection for the Admin queue
  - One connection for a I/O queue
  - Connection limits? Payload limits?

Data-transfer
-------------

TODO:

* Visualize command-data-buffers, capsules,

* Command Data Buffers (MANDATORY)
* In-capsule data (OPTIONAL)

* Host PDU Data Alignment (HPDA)
  - Is set in the ICReq PDU
  - Required alignment for PDU Data (DATA) for Controller ==> HOST transfer

* Controller PDU Data Alignment (CPDA)
  - Is set in the ICResp PDU
  - Required alignment for PDU Data (DATA) for HOST => Controller transfer

Capsules
~~~~~~~~

...

Design
======

* What to do with all the variable-length stuff?

* How to handle the need for multiple connections? For example when using an admin connection + at
  least on one I/O connection? The simplest is of course to just use the plain socket API, since
  it is also portable... but will it then need e.g. libuv for eventloop management? Or will this be
  fine as it is up the user regardless?

TODO
====

* Command-construction functions are missing in xNVMe for the Fabric command-set, this should be
  added.

 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @headerfile libxnvme_fabrics.h
 */

enum xnvme_fabrics_opcode {
	XNVME_FABRICS_OPCODE = 0x7f,
};

enum xnvme_fabrics_commmand_type {
	XNVME_FABRICS_COMMAND_TYPE_PROPERTY_SET             = 0x0,
	XNVME_FABRICS_COMMAND_TYPE_CONNECT                  = 0x1,
	XNVME_FABRICS_COMMAND_TYPE_PROPERTY_GET             = 0x4,
	XNVME_FABRICS_COMMAND_TYPE_AUTHENTIFICATION_SEND    = 0x5,
	XNVME_FABRICS_COMMAND_TYPE_AUTHENTIFICATION_RECEIVE = 0x6,
	XNVME_FABRICS_COMMAND_TYPE_DISCONNECT               = 0x8,
};

/**
 * Common fields for Fabrics Command Capsules e.g. non Fabrics-Command-Type-specific
 */
struct xnvme_fabrics_command_capsule_common {
	uint8_t opc;
	uint8_t fuse_psdt;
	uint16_t cid;

	uint8_t fctype;
	uint8_t _rsvd1[19];

	uint8_t sgl1[16];
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fabrics_command_capsule_common) == 40,
		    "Unexpected length");

/**
 * Figure 377: Authentication Receive Command - Submission Queue Entry
 *
 * @struct xnvme_fabrics_command_capsule_authentification_send_sqe
 */
struct xnvme_fabrics_command_capsule_authentification_receive_sqe {
	struct xnvme_fabrics_command_capsule_common common;

	uint8_t _rsvd1;

	uint8_t spsp0;
	uint8_t spsp1;
	uint8_t secp;
	uint32_t al;

	uint8_t _rsvd2[16];
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fabrics_command_capsule_authentification_receive_sqe) ==
			    64,
		    "Unexpected length");

/**
 * Figure 378: Authentication Receive Response
 *
 * @struct xnvme_fabrics_command_capsule_authentification_receive_response
 */
struct xnvme_fabrics_command_capsule_authentification_receive_response {
	uint8_t _rsvd1[8];

	uint16_t sqhd;

	uint8_t _rsvd2[2];

	uint16_t cid;
	uint16_t sts;
};
XNVME_STATIC_ASSERT(
	sizeof(struct xnvme_fabrics_command_capsule_authentification_receive_response) == 16,
	"Unexpected length");

/**
 * Figure 379: Authentication Send Command - Submission Queue Entry
 *
 * @struct xnvme_fabrics_command_capsule_authentification_send_sqe
 */
struct xnvme_fabrics_command_capsule_authentification_send_sqe {
	struct xnvme_fabrics_command_capsule_common common;

	uint8_t _rsvd1;

	uint8_t spsp0;
	uint8_t spsp1;
	uint8_t secp;
	uint32_t tl;

	uint8_t _rsvd2[16];
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fabrics_command_capsule_authentification_send_sqe) == 64,
		    "Unexpected length");

/**
 * Figure 380: Authentication Send Response
 *
 * @struct xnvme_fabrics_command_capsule_authentification_send_response
 */
struct xnvme_fabrics_command_capsule_authentification_send_response {
	uint8_t _rsvd1[8];

	uint16_t sqhd;

	uint8_t _rsvd2[2];

	uint16_t cid;
	uint16_t sts;
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fabrics_command_capsule_authentification_send_response) ==
			    16,
		    "Unexpected length");

/**
 * Figure 381: Connect Command - Submission Queue Entry
 *
 * @struct xnvme_fabrics_command_capsule_connect_sqe
 */
struct xnvme_fabrics_command_capsule_connect_sqe {
	struct xnvme_fabrics_command_capsule_common common;

	uint16_t recfmt;
	uint16_t qid;
	uint16_t sqsize;
	uint8_t cattr;

	uint8_t _rsvd1;

	uint32_t kato;

	uint8_t _rsvd2[12];
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fabrics_command_capsule_connect_sqe) == 64,
		    "Unexpected length");

/**
 * Figure 382: Connect Command - Data
 *
 * @struct xnvme_fabrics_command_capsule_connect_sqe
 */
struct xnvme_fabrics_command_capsule_connect_data {
	uint8_t hostid[16];
	uint16_t cntlid;

	uint8_t _rsvd1[238];

	uint8_t subnqn[256];
	uint8_t hostnqn[256];

	uint8_t _rsvd2[256];
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fabrics_command_capsule_connect_data) == 1024,
		    "Unexpected length");

/**
 * Figure 383: Connect Response
 *
 * @struct xnvme_fabrics_command_capsule_connect_response
 */
struct xnvme_fabrics_command_capsule_connect_response {
	uint32_t scs;

	uint32_t _rsvd1;

	uint16_t sqhd;

	uint16_t _rsvd2;

	uint16_t cid;
	uint16_t sts;
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fabrics_command_capsule_connect_response) == 16,
		    "Unexpected length");

/**
 * Figure 385: Disconnect Command - Submission Queue Entry
 *
 * @struct xnvme_fabrics_command_capsule_disconnect_sqe
 */
struct xnvme_fabrics_command_capsule_disconnect_sqe {
	struct xnvme_fabrics_command_capsule_common common;

	uint16_t recfmt;

	uint8_t _rsvd1[22];
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fabrics_command_capsule_disconnect_sqe) == 64,
		    "Unexpected length");

/**
 * Figure 386: Disconnect Response
 *
 * @struct xnvme_fabrics_command_capsule_disconnect_response
 */
struct xnvme_fabrics_command_capsule_disconnect_response {
	uint8_t _rsvd1[8];

	uint16_t sqhd;

	uint8_t _rsvd2[2];

	uint16_t cid;
	uint16_t sts;
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fabrics_command_capsule_disconnect_response) == 16,
		    "Unexpected length");

/**
 * Figure 387: Property Get Command - Submission Queue Entry
 *
 * @struct xnvme_fabrics_command_capsule_property_get_sqe
 */
struct xnvme_fabrics_command_capsule_property_get_sqe {
	struct xnvme_fabrics_command_capsule_common common;

	uint8_t attrib;

	uint8_t _rsvd1[3];

	uint32_t ofst;

	uint8_t _rsvd2[16];
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fabrics_command_capsule_property_get_sqe) == 64,
		    "Unexpected length");

/**
 * Figure 388: Property Get Response
 *
 * @struct xnvme_fabrics_command_capsule_property_get_response
 */
struct xnvme_fabrics_command_capsule_property_get_response {
	uint64_t value;
	uint16_t sqhd;

	uint8_t _rsvd1[2];

	uint16_t cid;
	uint16_t sts;
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fabrics_command_capsule_property_get_response) == 16,
		    "Unexpected length");

/**
 * Figure 389: Property Set Command - Submission Queue Entry
 *
 * @struct xnvme_fabrics_command_capsule_property_set_sqe
 */
struct xnvme_fabrics_command_capsule_property_set_sqe {
	struct xnvme_fabrics_command_capsule_common common;

	uint8_t attrib;

	uint8_t _rsvd1[3];

	uint32_t ofst;
	uint64_t value;

	uint8_t _rsvd2[8];
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fabrics_command_capsule_property_set_sqe) == 64,
		    "Unexpected length");

/**
 * Figure 390: Property Set Response
 *
 * @struct xnvme_fabrics_command_capsule_property_set_response
 */
struct xnvme_fabrics_command_capsule_property_set_response {
	uint8_t _rsvd1[8];

	uint16_t sqhd;

	uint8_t _rsvd2[2];

	uint16_t cid;
	uint16_t sts;
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fabrics_command_capsule_property_set_response) == 16,
		    "Unexpected length");

/**
 * Command-preppers for the NVMe Fabrics Command-Set
 */

/**
 * Prepare a NVMe Fabrics Authentification Receive command
 */
void
xnvme_fabrics_prep_authentification_receive(void *buf);

void
xnvme_fabrics_prep_authentification_send(void *buf);

void
xnvme_fabrics_prep_connect_sqe(void *buf, uint16_t qid, uint16_t sqsize);

void
xnvme_fabrics_prep_connect_data(void *buf, void *hostid, uint16_t cntlid, void *subnqn,
				void *hostnqn);

void
xnvme_fabrics_prep_disconnect(void *buf);

void
xnvme_fabrics_prep_property_get(void *buf);

void
xnvme_fabrics_prep_property_set(void *buf);

/**
 * NVMe/TCP Initiation Connection parameters
 */
struct xnvme_fabrics_connection_parameters {
	const char *addr; ///< TODO: this should use NVMe nomenclature
	int port;

	bool hdgst_enable;
	bool ddgst_enable;
};

/**
 * NVMe/TCP Connection state
 *
 * This is internal to the NVMe/TCP implementation, not something intended for external
 * consumption.
 *
 * - Initialize with _xnvme_fabrics_connection_init()
 * - Tear down with _xnvme_fabrics_connection_term()
 */
struct xnvme_fabrics_connection {
	struct xnvme_fabrics_connection_parameters params;
	int socket;

	uint32_t cpda_nbytes;       ///< Controller PDU data alignment in nbytes
	uint32_t maxh2cdata_nbytes; ///< Max h2c data-transfer in bytes

	void *request_buffer;  ///< Request buffer
	void *response_buffer; ///< Response buffers
};

/**
 * De-allocate the given connection structure and its associated buffers
 */
void
xnvme_fabrics_connection_term(struct xnvme_fabrics_connection *con);

/**
 * Allocate and initialize a connection structure
 *
 * This is a memory-handling helper-function that alloacates the connection structure, buffers
 * dedicated to command payloads, and assigns the given parameters to the connection. It is
 * primarily utilized by xnvme_fabrics_connect() but is made available to the user should they want
 * to prepare the struture for a "direct" call to e.g. xnvme_fabrics_tcp_connect() instead of via
 * xnvme_fabrics_connect().
 *
 * De-allocate by using xnvme_fabrics_connection_term().
 */
int
xnvme_fabrics_connection_init(struct xnvme_fabrics_connection **con,
			      struct xnvme_fabrics_connection_parameters *params);

void
xnvme_fabrics_disconnect(struct xnvme_fabrics_connection *con);

/**
 * Initiate a connection to the fabrics endpoint using the given configuration parameters
 *
 * @param con Connection initialized with xnvme_fabrics_connection_init()
 * @param params Pointer to connection configuration parameters e.g. digest negotiation and
 *               alignment
 *
 * @return On success, a pointer to the connection is returned. On error, NULL is returned and
 *         errno is set to indicate the error.
 */
struct xnvme_fabrics_connection *
xnvme_fabrics_connect(struct xnvme_fabrics_connection_parameters *params);