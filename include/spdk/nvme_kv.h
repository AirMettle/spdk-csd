/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 AirMettle, Inc.
 *   All rights reserved.
 */

/**
 * \file
 * AirMettle Key-Value Select vendor-specific definitions
 *
 * Reference:  TBD
 */

#ifndef SPDK_NVME_KV_H
#define SPDK_NVME_KV_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "spdk/nvme.h"

/* KV Specific Opcodes */

enum NVME_KV_Opcodes {
    /* KV commands - these don't exactly match 2.0 spec since we can't use 0x01 and 0x02 */
    /* write commands need 0x01 set */
    /* read commands need 0x02 set */
    SPDK_NVME_OPC_KV_LIST            = 0x06,
    SPDK_NVME_OPC_KV_DELETE          = 0x10,
    SPDK_NVME_OPC_KV_EXIST           = 0x14,
    SPDK_NVME_OPC_KV_STORE           = 0x81,
    SPDK_NVME_OPC_KV_RETRIEVE        = 0x82,
    /* Send the select command */
    SPDK_NVME_OPC_KV_SEND_SELECT     = 0x85,
    /* Retrieve results from the select */
    SPDK_NVME_OPC_KV_RETRIEVE_SELECT = 0x86
};

/**
 * Fetch a list of available keys associated with key-values in given namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the write I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param prefix Prefix of keys to match against
 * \param prefix_len Length of prefix
 * \param buffer Virtual address pointer to the data buffer
 * \param buffer_size Size of data buffer
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined by the SPDK_NVME_IO_FLAGS_* entries in
 * spdk/nvme_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
int spdk_nvme_ns_cmd_kvlist(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			    unsigned char *prefix, size_t prefix_len,
			    void *buffer, uint64_t buffer_size,
			    spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags);

int spdk_nvme_ns_cmd_kvlistv(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			    unsigned char *prefix, size_t prefix_len,	uint64_t buffer_size,
			    spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
			    spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			    spdk_nvme_req_next_sge_cb next_sge_fn);

/**
 * Deletes a key-value pair.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the write I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param key Key associated with blob to be deleted
 * \param key_len Length of key
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined by the SPDK_NVME_IO_FLAGS_* entries in
 * spdk/nvme_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
int spdk_nvme_ns_cmd_kvdelete(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			      unsigned char *key, size_t key_len,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			      uint32_t io_flags);

/**
 * Determines if the given key is defined.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the write I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param key Key associated with blob to be deleted
 * \param key_len Length of key
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined by the SPDK_NVME_IO_FLAGS_* entries in
 * spdk/nvme_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
int spdk_nvme_ns_cmd_kvexist(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			     unsigned char *key, size_t key_len,
			     spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			     uint32_t io_flags);

/*
 * KV_STORE option flags
 *
 */
/* Only write if key already exists */
#define SPDK_NVME_KV_STORE_FLAG_MUST_EXIST	(1U << 0)
/* Only write if key does not exist */
#define SPDK_NVME_KV_STORE_FLAG_MUST_NOT_EXIST	(1U << 1)
/* Append data to object if exists rather than truncating */
#define SPDK_NVME_KV_STORE_FLAG_APPEND		(1U << 3)
#define SPDK_NVME_KV_STORE_FLAG_VALID_MASK	0x0b

/**
 * Stores a key-value pair
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the write I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param key Key to associate with data
 * \param key_len Length of key
 * \param payload Virtual address pointer to the data payload.
 * \param payload_size Size of data payload
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param store_flags Option flags for KV store operation
 * \param io_flags Set flags, defined by the SPDK_NVME_IO_FLAGS_* entries in
 * spdk/nvme_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
int spdk_nvme_ns_cmd_kvstore(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			     unsigned char *key, size_t key_len,
			     void *payload, uint64_t payload_size,
			     spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			     uint8_t store_flags, uint32_t io_flags);

int spdk_nvme_ns_cmd_kvstorev(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
					unsigned char *key, size_t key_len,
					uint64_t payload_size,
					spdk_nvme_cmd_cb cb_fn, void *cb_arg,
					uint8_t store_flags, uint32_t io_flags,
					spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
					spdk_nvme_req_next_sge_cb next_sge_fn);

/**
 * Retrieves the data blob associated with the given key.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the write I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param key Key to associate with data
 * \param key_len Length of key
 * \param buffer Virtual address pointer to the data buffer.
 * \param buffer_size Size of data buffer
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined by the SPDK_NVME_IO_FLAGS_* entries in
 * spdk/nvme_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
int spdk_nvme_ns_cmd_kvretrieve(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				unsigned char *key, size_t key_len,
				void *buffer, uint64_t buffer_size,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				uint64_t offset, uint32_t io_flags);

int spdk_nvme_ns_cmd_kvretrievev(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				unsigned char *key, size_t key_len,
				uint64_t buffer_size,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				uint64_t offset, uint32_t io_flags,
				spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
				spdk_nvme_req_next_sge_cb next_sge_fn);


/*
 * KV_SEND_SELECT Format Types
 */
typedef enum {
	SPDK_NVME_KV_DATATYPE_CSV 	= 0x0,
	SPDK_NVME_KV_DATATYPE_JSON	= 0x1,
	SPDK_NVME_KV_DATATYPE_PARQUET	= 0x2,
} spdk_nvme_kv_datatype;

#define SPDK_NVME_KV_SELECT_INPUT_HEADER	(1U << 0)
#define SPDK_NVME_KV_SELECT_OUTPUT_HEADER	(1U << 1)
#define SPDK_NVME_KV_SELECT_HEADER_VALID_MASK	0x3

/**
 * Sends a SELECT query to the NVMe device. Returns a unique select-id
 * as part of command completion. Use this select-id with the command
 * spdk_nvme_ns_cmd_kvselect_retrieve() to fetch query results.
 *
 * \param ns NVMe namespace to submit the write I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param key Key of data to query
 * \param key_len Length of key
 * \param query SQL-style SELECT statement
 * \param input_type Data format of input data
 * \param output_type Data format to output
 * \param header_opts Option flag for handing headers
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param send_flags Option flags for kvselect_send
 * \param io_flags Set flags, defined by the SPDK_NVME_IO_FLAGS_* entries in
 * spdk/nvme_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
int spdk_nvme_ns_cmd_kvselect_send(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				   unsigned char *key, size_t key_len, char *query,
				   spdk_nvme_kv_datatype input_type, spdk_nvme_kv_datatype output_type,
				   uint8_t header_opts, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				   uint32_t io_flags);

int spdk_nvme_ns_cmd_kvselect_sendv(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				   unsigned char *key, size_t key_len, uint64_t query_len,
				   spdk_nvme_kv_datatype input_type, spdk_nvme_kv_datatype output_type,
				   uint8_t header_opts, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				   uint32_t io_flags, spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
				   spdk_nvme_req_next_sge_cb next_sge_fn);

/* Options on retrieving selection */
typedef enum {
	SPDK_NVME_KV_SELECT_FREE_ALL 	= 0,
	SPDK_NVME_KV_SELECT_NO_FREE	= 1,
	SPDK_NVME_KV_SELECT_FREE_IF_FIT	= 2,
} spdk_nvme_kv_select_opts;

/**
 * Retreives results of a SELECT command using the select-id returned
 * as part of command completion of spdk_nvme_ns_cmd_kvselect_send().
 *
 * \param ns NVMe namespace to submit the write I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param select_id Select ID from prior kvselect_send() call.
 * \param offset Offset (in bytes) to start retrieving from
 * \param buffer Virtual address pointer to the data buffer
 * \param buffer_size Size of data buffer
 * \param options Command options
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param retrieve_flags Option flags for kvselect_retrieve
 * \param io_flags Set flags, defined by the SPDK_NVME_IO_FLAGS_* entries in
 * spdk/nvme_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, negated errnos on the following error conditions:
 * -EINVAL: The request is malformed.
 * -ENOMEM: The request cannot be allocated.
 * -ENXIO: The qpair is failed at the transport level.
 */
int spdk_nvme_ns_cmd_kvselect_retrieve(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				       uint32_t select_id, uint32_t offset, void *buffer,
				       uint32_t buffer_size, spdk_nvme_kv_select_opts opts,
				       spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				       uint32_t io_flags);

int spdk_nvme_ns_cmd_kvselect_retrievev(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				       uint32_t select_id, uint32_t offset,
				       uint32_t buffer_size, spdk_nvme_kv_select_opts opts,
				       spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				       uint32_t io_flags, spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
				       spdk_nvme_req_next_sge_cb next_sge_fn);

#ifdef __cplusplus
}
#endif

#endif
