/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @headerfile libxnvme_be.h
 */

/**
 * Representation of xNVMe library backend attributes
 *
 * @struct xnvme_be_attr
 */
struct xnvme_be_attr {
	const char *name; ///< Backend name

	uint8_t enabled; ///< Whether the backend is 'enabled'

	uint8_t _rsvd[15];
};
