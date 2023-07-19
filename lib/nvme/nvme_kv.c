/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 AirMettle, Inc.
 *   All rights reserved.
 */

#include "nvme_internal.h"
#include "spdk/endian.h"
#include "spdk/nvme_kv.h"


/*
 * Utility function for adding key to NVMe command.
 * Should be used by any spdk_nvme_ns_cmd_* function needing to add
 * a key to the NVMe command.
 */
static int
_nvme_cmd_kv_add_key(struct spdk_nvme_cmd *cmd, unsigned char *key, size_t key_len, uint32_t flags) {

	if (key_len > 16) {
		return -1;
	}

	switch (cmd->opc) {
		case SPDK_NVME_OPC_KV_STORE:
			if (flags & ~SPDK_NVME_KV_STORE_FLAG_VALID_MASK) {
				SPDK_ERRLOG("Invalid store_flags 0x%x\n", flags);
				return -1;
			} else if ((flags & (SPDK_NVME_KV_STORE_FLAG_MUST_EXIST | SPDK_NVME_KV_STORE_FLAG_MUST_NOT_EXIST)) == (SPDK_NVME_KV_STORE_FLAG_MUST_EXIST | SPDK_NVME_KV_STORE_FLAG_MUST_NOT_EXIST)) {
				SPDK_ERRLOG("Incompatible store_flags. MUST_EXIST and MUST_NOT_EXIST cannot both be set\n");
				return -1;
			}
			to_le32(&cmd->cdw11, ((uint32_t) flags << 8) | (key_len & 0xff));
			break;
		case SPDK_NVME_OPC_KV_SEND_SELECT:
		case SPDK_NVME_OPC_KV_RETRIEVE_SELECT:
			to_le32(&cmd->cdw11, ((uint32_t) flags << 8) | (key_len & 0xff));
			break;
		default:
			to_le32(&cmd->cdw11, key_len & 0xff);
	}

	/* key stored in 4 dwords */
	/* TODO - implement as loop for only key_len chars */
	to_le32(&cmd->cdw15, (uint32_t) key[3] | (uint32_t) (key[2] << 8) |(uint32_t) key[1] << 16 | (uint32_t) key[0] << 24);
	to_le32(&cmd->cdw14, (uint32_t) key[7] | (uint32_t) (key[6] << 8) |(uint32_t) key[5] << 16 | (uint32_t) key[4] << 24);
	to_le32(&cmd->rsvd3, (uint32_t) key[11] | (uint32_t) (key[10] << 8) |(uint32_t) key[9] << 16 | (uint32_t) key[8] << 24);
	to_le32(&cmd->rsvd2, (uint32_t) key[15] | (uint32_t) (key[14] << 8) |(uint32_t) key[13] << 16 | (uint32_t) key[12] << 24);

	return 0;
}

int
spdk_nvme_ns_cmd_kvlist(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			unsigned char *prefix, size_t prefix_len,
			void *buffer, uint64_t buffer_size,
			spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags) {

	int rc;
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;
	struct nvme_payload	payload;

	payload = NVME_PAYLOAD_CONTIG(buffer, NULL);

	req = nvme_allocate_request(qpair, &payload, buffer_size, 0, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_KV_LIST;
	cmd->nsid = ns->id;

	rc = _nvme_cmd_kv_add_key(cmd, prefix, prefix_len, 0);
	if (rc != 0) {
		return -EINVAL;
	}

	to_le32(&cmd->cdw10, buffer_size);

	return nvme_qpair_submit_request(qpair, req);
}

int
spdk_nvme_ns_cmd_kvdelete(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			  unsigned char *key, size_t key_len,
			  spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			  uint32_t io_flags) {

	int rc;
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;

	req = nvme_allocate_request_null(qpair, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_KV_DELETE;
	cmd->nsid = ns->id;

	rc = _nvme_cmd_kv_add_key(cmd, key, key_len, 0);
	if (rc != 0) {
		return -EINVAL;
	}

	return nvme_qpair_submit_request(qpair, req);
}

int
spdk_nvme_ns_cmd_kvexist(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			  unsigned char *key, size_t key_len,
			  spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			  uint32_t io_flags) {

	int rc;
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;

	req = nvme_allocate_request_null(qpair, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_KV_EXIST;
	cmd->nsid = ns->id;

	rc = _nvme_cmd_kv_add_key(cmd, key, key_len, 0);
	if (rc != 0) {
		return -EINVAL;
	}

	return nvme_qpair_submit_request(qpair, req);
}

int
spdk_nvme_ns_cmd_kvstore(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			 unsigned char *key, size_t key_len,
			 void *buffer, uint64_t buffer_size,
			 spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			 uint8_t store_flags, uint32_t io_flags) {

	int rc;
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;
	struct nvme_payload	payload;

	/* validate storage flags */
	if (store_flags & ~SPDK_NVME_KV_STORE_FLAG_VALID_MASK) {
		SPDK_ERRLOG("Invalid store_flags 0x%x\n", store_flags);
		return -1;
	} else if ((store_flags & (SPDK_NVME_KV_STORE_FLAG_MUST_EXIST | SPDK_NVME_KV_STORE_FLAG_MUST_NOT_EXIST)) == (SPDK_NVME_KV_STORE_FLAG_MUST_EXIST | SPDK_NVME_KV_STORE_FLAG_MUST_NOT_EXIST)) {
		SPDK_ERRLOG("Incompatible store_flags. MUST_EXIST and MUST_NOT_EXIST cannot both be set\n");
		return -1;
	}

	payload = NVME_PAYLOAD_CONTIG(buffer, NULL);

	req = nvme_allocate_request(qpair, &payload, buffer_size, 0, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_KV_STORE;
	cmd->nsid = ns->id;

	rc = _nvme_cmd_kv_add_key(cmd, key, key_len, store_flags);
	if (rc != 0) {
		return -EINVAL;
	}

	to_le32(&cmd->cdw10, buffer_size);

	return nvme_qpair_submit_request(qpair, req);
}

int
spdk_nvme_ns_cmd_kvretrieve(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			    unsigned char *key, size_t key_len,
			    void *buffer, uint64_t buffer_size,
			    spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				uint64_t offset, uint32_t io_flags) {

	int rc;
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;
	struct nvme_payload	payload;

	payload = NVME_PAYLOAD_CONTIG(buffer, NULL);

	req = nvme_allocate_request(qpair, &payload, buffer_size, 0, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_KV_RETRIEVE;
	cmd->nsid = ns->id;

	rc = _nvme_cmd_kv_add_key(cmd, key, key_len, 0);
	if (rc != 0) {
		return -EINVAL;
	}
	to_le32(&cmd->cdw12, offset);
	to_le32(&cmd->cdw10, buffer_size);

	return nvme_qpair_submit_request(qpair, req);
}

struct _kvselect_send_cb_internal_ctx {
	char *buffer;
	spdk_nvme_cmd_cb cb_fn;
	void *cb_arg;
};

static void
_kvselect_send_cb_internal(void *arg, const struct spdk_nvme_cpl *cpl) {

	struct _kvselect_send_cb_internal_ctx *ctx = arg;

	spdk_free(ctx->buffer);
	(ctx->cb_fn)(ctx->cb_arg, cpl);
	free(ctx);
};

int
spdk_nvme_ns_cmd_kvselect_send(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			       unsigned char *key, size_t key_len, char *query,
			       spdk_nvme_kv_datatype input_type, spdk_nvme_kv_datatype output_type,
			       uint8_t header_opts, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			       uint32_t io_flags) {

	int rc;
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;
	struct nvme_payload	payload;
	uint32_t 		flags = 0;

	/* validate types and header options */
	if (header_opts & ~SPDK_NVME_KV_SELECT_HEADER_VALID_MASK) {
		SPDK_ERRLOG("Invalid header options 0x%x\n", header_opts);
		return -1;
	}

	flags = header_opts | (input_type << 8) | (output_type << 16);

	/* we cannot simply paste an arbitrary buffer (query) into the command as a payload
	 * instead we must create an SPDK buffer, copy the string to that and then send
	 */

	char *query_buffer = spdk_zmalloc(strlen(query)+1, 0, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (query_buffer == NULL) {
		return -ENOMEM;
	}
	strcpy(query_buffer, query);

	/* use an internal-only callback and context to free above when command completes */
	struct _kvselect_send_cb_internal_ctx *_ctx = calloc(1, sizeof(struct _kvselect_send_cb_internal_ctx));
	_ctx->buffer = query_buffer;
	_ctx->cb_fn = cb_fn;
	_ctx->cb_arg = cb_arg;

	payload = NVME_PAYLOAD_CONTIG(query_buffer, NULL);

	req = nvme_allocate_request(qpair, &payload, strlen(query)+1, 0, _kvselect_send_cb_internal, _ctx);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_KV_SEND_SELECT;
	cmd->nsid = ns->id;

	rc = _nvme_cmd_kv_add_key(cmd, key, key_len, flags);
	if (rc != 0) {
		return -EINVAL;
	}

	to_le32(&cmd->cdw10, strlen(query));

	return nvme_qpair_submit_request(qpair, req);
}


int
spdk_nvme_ns_cmd_kvselect_retrieve(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				   uint32_t select_id, uint32_t offset, void *buffer,
				   uint32_t buffer_size, spdk_nvme_kv_select_opts opts,
				   spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				   uint32_t io_flags) {

	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;
	struct nvme_payload	payload;

	payload = NVME_PAYLOAD_CONTIG(buffer, NULL);

	req = nvme_allocate_request(qpair, &payload, buffer_size, 0, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_KV_RETRIEVE_SELECT;
	cmd->nsid = ns->id;

	to_le32(&cmd->cdw10, buffer_size);
	to_le32(&cmd->cdw11, opts);
	to_le32(&cmd->cdw12, offset);
	to_le32(&cmd->cdw13, select_id);

	return nvme_qpair_submit_request(qpair, req);

}