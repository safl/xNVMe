/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @headerfile libxnvme_geo.h
 */

/**
 * Representation of the type of device / geo / namespace
 *
 * @enum xnvme_geo_type
 */
enum xnvme_geo_type {
	XNVME_GEO_UNKNOWN      = 0x0,
	XNVME_GEO_CONVENTIONAL = 0x1,
	XNVME_GEO_ZONED        = 0x2,
	XNVME_GEO_KV           = 0x3,
};

/**
 * Representation of device "geometry"
 *
 * This will remain in some, encapsulating IO parameters such as MDTS, ZONE
 * APPEND MDTS, nbytes, nsect etc. mapping to zone characteristics, as well as
 * extended LBA formats.
 *
 * @struct xnvme_geo
 */
struct xnvme_geo {
	enum xnvme_geo_type type;

	uint32_t npugrp; ///< Nr. of Parallel Unit Groups
	uint32_t npunit; ///< Nr. of Parallel Units in PUG

	uint32_t nzone;      ///< Nr. of zones in PU
	uint64_t nsect;      ///< Nr. of sectors per zone
	uint32_t nbytes;     ///< Nr. of bytes per sector
	uint32_t nbytes_oob; ///< Nr. of bytes per sector in OOB

	uint64_t tbytes; ///< Total # bytes in geometry
	uint64_t ssw;    ///< Bit-width for LBA fmt conversion

	uint32_t mdts_nbytes; ///< Maximum-data-transfer-size in unit of bytes

	uint32_t lba_nbytes;  ///< Size of an LBA in bytes
	uint8_t lba_extended; ///< Extended LBA: 1=Supported, 0=Not-Supported

	uint8_t pi_type;   ///< Protection Information Type
	uint8_t pi_loc;    ///< PI location in metadata: 1=Start, 0=End
	uint8_t pi_format; ///< Protection Information Format

	uint8_t _rsvd[4];
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_geo) == 64, "Incorrect size")

/**
 * Prints the given ::xnvme_geo to the given output stream
 *
 * @param stream output stream used for printing
 * @param geo pointer to the the ::xnvme_geo to print
 * @param opts printer options, see ::xnvme_pr
 *
 * @return On success, the number of characters printed is returned.
 */
int
xnvme_geo_fpr(FILE *stream, const struct xnvme_geo *geo, int opts);

/**
 * Prints the given ::xnvme_geo to stdout
 *
 * @param geo pointer to the the ::xnvme_geo to print
 * @param opts printer options, see ::xnvme_pr
 *
 * @return On success, the number of characters printed is returned.
 */
int
xnvme_geo_pr(const struct xnvme_geo *geo, int opts);