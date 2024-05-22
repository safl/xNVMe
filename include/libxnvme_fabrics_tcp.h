enum xnvme_fabrics_tcp_port {
	XNVME_FABRICS_TCP_PORT      = 4420,
	XNVME_FABRICS_TCP_DISCOVERY = 8009,
};

enum xnvme_fabrics_tcp_opcode {
	XNVME_FABRICS_TCP_OPC_ICREQ        = 0x0,
	XNVME_FABRICS_TCP_OPC_ICRESP       = 0x1,
	XNVME_FABRICS_TCP_OPC_H2C_TERM_REQ = 0x2,
	XNVME_FABRICS_TCP_OPC_C2H_TERM_REQ = 0x3,
	XNVME_FABRICS_TCP_OPC_CAPSULE_CMD  = 0x4,
	XNVME_FABRICS_TCP_OPC_CAPSULE_RESP = 0x5,
	XNVME_FABRICS_TCP_OPC_H2C_DATA     = 0x6,
	XNVME_FABRICS_TCP_OPC_C2H_DATA     = 0x7,
	XNVME_FABRICS_TCP_OPC_R2T          = 0x9,
};

/**
 * Figure 22: Host to Controller Terminate Connection Request PDU (H2CTermReq)
 */
enum xnvme_fabrics_tcp_fes {
	XNVME_FABRICS_TCP_FES_INVALID_PDU_HEADER_FIELD   = 0x1,
	XNVME_FABRICS_TCP_FES_PDU_SEQUENCE_ERROR         = 0x2,
	XNVME_FABRICS_TCP_FES_HEADER_DIGEST_ERROR        = 0x3,
	XNVME_FABRICS_TCP_FES_DATA_TRANSFER_OUT_OF_RANGE = 0x4,
	XNVME_FABRICS_TCP_FES_R2T_LIMIT_EXCEEDED         = 0x5,
	XNVME_FABRICS_TCP_FES_UNSUPPORTED_PARAMETERS     = 0x6,
};

/**
 * PDU Common Header (ch)
 */
struct xnvme_fabrics_tcp_ch {
	uint8_t type;
	uint8_t flags;
	uint8_t hlen;
	uint8_t pdo;
	uint32_t plen;
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fabrics_tcp_ch) == 8, "Unexpected length");

/**
 * Initialize Connection Request PDU (ICReq)
 */
struct xnvme_fabrics_tcp_ic_req {
	struct xnvme_fabrics_tcp_ch ch;

	uint16_t pfv;
	uint8_t hpda;
	uint8_t dgst;
	uint32_t maxr2t;

	uint8_t _reserved0[112];
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fabrics_tcp_ic_req) == 128, "Unexpected length");

/**
 * Initialize Connection Response PDU (ICResp)
 */
struct xnvme_fabrics_tcp_ic_resp {
	struct xnvme_fabrics_tcp_ch ch;

	uint16_t pfv;
	uint8_t cpda;
	uint8_t dgst;
	uint32_t maxh2cdata;

	uint8_t _reserved0[112];
};

/**
 * Host to Controller Terminate Connection Request PDU (H2CTermReq)
 */
#pragma pack(push, 1)
struct xnvme_fabrics_tcp_h2c_term_req {
	struct xnvme_fabrics_tcp_ch ch;

	uint16_t fes;
	uint32_t fei;

	uint8_t _reserved0[10];

	uint8_t data[];
};
#pragma pack(pop)
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fabrics_tcp_h2c_term_req) == 24, "Unexpected length");

/**
 * Controller to Host Terminate Connection Request PDU (C2HTermReq)
 */
#pragma pack(push, 1)
struct xnvme_fabrics_tcp_c2h_term_req {
	struct xnvme_fabrics_tcp_ch ch;

	uint16_t fes;
	uint32_t fei;

	uint8_t _reserved0[10];

	uint8_t data[];
};
#pragma pack(pop)
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fabrics_tcp_c2h_term_req) == 24, "Unexpected length");

struct xnvme_fabrics_tcp_capsule_cmd {
	struct xnvme_fabrics_tcp_ch ch;

	uint8_t ccsqe[64];

	// variable length hdgst, pad, data, ddgst goes here ...
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fabrics_tcp_capsule_cmd) == 72, "Unexpected length");

struct xnvme_fabrics_tcp_capsule_resp {
	struct xnvme_fabrics_tcp_ch ch;

	uint8_t rccqe[16];

	// hdgst goes here when present ...
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fabrics_tcp_capsule_resp) == 24, "Unexpected length");

struct xnvme_fabrics_tcp_h2c_data {
	struct xnvme_fabrics_tcp_ch ch;

	uint16_t cccid;
	uint16_t ttag;
	uint32_t datao;
	uint32_t datal;

	uint8_t _reserved0[4];

	// hdgst, pad, pdu-data, ddgst goes here
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fabrics_tcp_h2c_data) == 24, "Unexpected length");

struct xnvme_fabrics_tcp_c2h_data {
	struct xnvme_fabrics_tcp_ch ch;

	uint16_t cccid;

	uint8_t _reserved0[2];

	uint32_t datao;
	uint32_t datal;

	uint8_t _reserved1[4];

	// hdgst, pad, pdu-data, ddgst goes here
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fabrics_tcp_c2h_data) == 24, "Unexpected length");

struct xnvme_fabrics_tcp_r2t {
	struct xnvme_fabrics_tcp_ch ch;

	uint16_t cccid;
	uint16_t ttag;
	uint32_t r2to;
	uint32_t r2tl;

	uint8_t _reserved0[4];
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fabrics_tcp_r2t) == 24, "Unexpected length");

/**
 * Prepare an Initialize Connection Request PDU (ICReq)
 *
 * @param buf Pointer to a buffer to prepare
 * @param hpda Specifies the data alignment for all PDUs transferred from the controller to the
 *             host that contain data. This value is 0’s based value in units of dwords in the
 *             range 0 to 31 (e.g., values 0, 1, and 2 correspond to 4 byte, 8 byte, and 12 byte
 *             alignment).
 * @param hdgst_enable If set to ‘1’, the use of header digest is requested by the host for the
 *                     connection. If cleared to ‘0’, header digest shall not be used for the
 *                     connection.
 * @param ddgst_enable If set to ‘1’, the use of data digest is requested by the host for the
 *                     connection. If cleared to ‘0’, data digest shall not be used for the
 *                     connection.
 * @param maxr2t Specifies the maximum number of outstanding R2T PDUs for a command at any point in
 *               time on the connection. This is a 0’s based value.
 */
void
xnvme_fabrics_prep_tcp_ic_req(void *buf, uint8_t hpda, bool hdgst_enable, bool ddgst_enable,
			      uint32_t maxr2t);

/**
 * Preprare an Initialize Connection Response PDU (ICResp)
 *
 * @param buf Pointer to a buffer to prepare
 * @param cpda Specifies the data alignment for all PDUs that transfer data in addiion to the PDU
 *             Header (refer to section 2). This is a 0’s based value in units of dwords in the
 *             range 0 to 31 (e.g., values 0, 1, and 2 correspond to 4 byte, 8 byte, and 12 byte
 *             alignment).
 * @param hdgst_enable If set to ‘1’, the use of header digest is requested by the host for the
 *                     connection. If cleared to ‘0’, header digest shall not be used for the
 *                     connection.
 * @param ddgst_enable If set to ‘1’, the use of data digest is requested by the host for the
 *                     connection. If cleared to ‘0’, data digest shall not be used for the
 *                     connection.
 * @param maxh2cdata Specifies the maximum number of PDU-Data bytes per H2CData PDU in bytes. This
 *                   value is a multiple of dwords and should be no less than 4,096.
 */
void
xnvme_fabrics_prep_tcp_ic_resp(void *buf, uint8_t cpda, bool hdgst_enable, bool ddgst_enable,
			       uint32_t maxh2cdata);

void
xnvme_fabrics_prep_tcp_h2c_term_req(void *buf, uint16_t fes, uint16_t fei, uint32_t plen);

void
xnvme_fabrics_prep_tcp_c2h_term_req(void *buf, uint16_t fes, uint16_t fei, uint32_t plen);

void
xnvme_fabrics_prep_tcp_capsule_cmd(void *buf, bool hdgst_enable, bool ddgst_enable, uint8_t pdo,
				   uint32_t plen, void *ccsqe);

void
xnvme_fabrics_prep_tcp_capsule_resp(void *buf, bool hdgst_enable, uint32_t plen, void *rccqe);

void
xnvme_fabrics_prep_tcp_h2c_data(void *buf, bool hdgst_enable, bool ddgst_enable, bool last_pdu,
				uint8_t pdo, uint32_t plen, uint16_t cccid, uint16_t ttag,
				uint32_t datao, uint32_t datal);

void
xnvme_fabrics_prep_tcp_c2h_data(void *buf, bool hdgst_enable, bool ddgst_enable, bool last_pdu,
				bool success, uint8_t pdo, uint32_t plen, uint16_t cccid,
				uint32_t datao, uint32_t datal);

void
xnvme_fabrics_prep_tcp_r2t(void *buf, bool hdgst_enable, uint32_t plen, uint16_t cccid,
			   uint16_t ttag, uint32_t r2to, uint32_t r2tl);

int
xnvme_fabrics_tcp_connect(struct xnvme_fabrics_connection *con);

void
xnvme_fabrics_tcp_disconnect(struct xnvme_fabrics_connection *con);
