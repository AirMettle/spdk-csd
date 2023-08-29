/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023 AirMettle, Inc.
 *   Portions Copyright (c) Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/nvme_kv.h"
#include "spdk/env.h"
#include "spdk/endian.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

/*
 * Leverages code from simple_copy.c and other similar examples/test programs.
 */

static struct ns_entry *g_namespaces = NULL;
static struct spdk_nvme_transport_id g_trid;
static bool g_use_trid = false;
static uint32_t g_block_size = 0;


bool g_reset_state = false;

char g_test_keys[4][16] = {
	"~TEST_01",
	"~TEST_02XX",
	"~TEST_03YYYYY",
	"~TEST_04"
};

char *g_test_key1 = g_test_keys[0];
char *g_test_key2 = g_test_keys[1];
char *g_test_key3 = g_test_keys[2];
char *g_test_key4 = g_test_keys[3];

struct ns_entry {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns	*ns;
	struct ns_entry		*next;
	struct spdk_nvme_qpair	*qpair;
};

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry				*entry;
	const struct spdk_nvme_ctrlr_data	*cdata;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (!spdk_nvme_ns_is_active(ns)) {
		printf("Controller %-20.20s (%-20.20s): Skipping inactive NS %u\n",
		       cdata->mn, cdata->sn,
		       spdk_nvme_ns_get_id(ns));
		return;
	}

	entry = malloc(sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}

	entry->ctrlr = ctrlr;
	entry->ns = ns;
	entry->next = g_namespaces;
	g_namespaces = entry;
	g_block_size = spdk_nvme_ns_get_sector_size(ns);

	printf("  Namespace ID: %d size: %juGB\n", spdk_nvme_ns_get_id(ns),
	       spdk_nvme_ns_get_size(ns) / 1000000000);
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attaching to %s\n", trid->traddr);

	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	int			num_ns;
	struct spdk_nvme_ns	*ns;

	/*
	const struct spdk_nvme_ctrlr_data	*cdata;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

        // TODO: cdata->vs is uint8_t[1024] for vendor specific identification
        //       use this to determine if has necessary KV support
	*/

        /*
         * Use only the first namespace from each controller since we are testing controller level functionality.
         */
        num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
        if (num_ns < 1) {
                printf("No valid namespaces in controller\n");
        } else {
                ns = spdk_nvme_ctrlr_get_ns(ctrlr, 1);
                register_ns(ctrlr, ns);
        }
}

struct sgl_element {
	void *base;
	size_t offset;
	size_t len;
};

struct io_request {
	uint32_t current_iov_index;
	uint32_t current_iov_bytes_left;
	struct sgl_element iovs[2];
	uint32_t nseg;
	bool cmd_done;
	/* expected return values */
	struct spdk_nvme_status exp_status;
	/* validation status */
	int num_errors;
};

static void
nvme_request_reset_sgl(void *cb_arg, uint32_t sgl_offset)
{
	uint32_t i;
	uint32_t offset = 0;
	struct sgl_element *iov;
	struct io_request *req = (struct io_request *)cb_arg;

	for (i = 0; i < req->nseg; i++) {
		iov = &req->iovs[i];
		offset += iov->len;
		if (offset > sgl_offset) {
			break;
		}
	}
	req->current_iov_index = i;
	req->current_iov_bytes_left = offset - sgl_offset;
	return;
}

static int
nvme_request_next_sge(void *cb_arg, void **address, uint32_t *length)
{
	struct io_request *req = (struct io_request *)cb_arg;
	struct sgl_element *iov;

	if (req->current_iov_index >= req->nseg) {
		*length = 0;
		*address = NULL;
		return 0;
	}

	iov = &req->iovs[req->current_iov_index];

	if (req->current_iov_bytes_left) {
		*address = iov->base + iov->offset + iov->len - req->current_iov_bytes_left;
		*length = req->current_iov_bytes_left;
		req->current_iov_bytes_left = 0;
	} else {
		*address = iov->base + iov->offset;
		*length = iov->len;
	}

	req->current_iov_index++;

	return 0;
}

static void basic_error_checking(void *arg, const struct spdk_nvme_cpl *cpl, char *cmd_name) {
	struct io_request	*ctx = arg;

	ctx->cmd_done = 1;

	if (spdk_nvme_cpl_is_error(cpl)) {
		/* just print for informational reasons*/
		fprintf(stderr, "%s: scc cpl error. SC 0x%x SCT 0x%x\n", cmd_name, cpl->status.sc, cpl->status.sct);
	}

	// verify returned status
	if (cpl->status.sc != ctx->exp_status.sc) {
		fprintf(stderr, "%s: unexpected return status: got %d expected %d\n", cmd_name, cpl->status.sc, ctx->exp_status.sc);
		ctx->num_errors++;
	}

	if (cpl->status.sct != ctx->exp_status.sct) {
		fprintf(stderr, "%s: unexpected return status type: got %d expected %d\n", cmd_name, cpl->status.sct, ctx->exp_status.sct);
		ctx->num_errors++;
	}
}

/*
 * NVME_CMD_KV_STORE
 */

struct run_kvstore_ctx {
	struct io_request	_base_ctx;
};

static void
run_kvstore_cb(void *arg, const struct spdk_nvme_cpl *cpl) {
	basic_error_checking(arg, cpl, "run_kvstore");
};

static int
run_kvstore(struct ns_entry *ns_entry, char *key, size_t key_len, void *buffer, size_t buffer_len, uint8_t flags, uint16_t sc) {

	int rc;
	struct run_kvstore_ctx 	ctx;

	memset(&ctx, 0, sizeof(struct run_kvstore_ctx));

	ctx._base_ctx.cmd_done = 0;
	ctx._base_ctx.iovs[0].base = buffer;
	ctx._base_ctx.iovs[0].len = 6;
	ctx._base_ctx.iovs[1].base = buffer + 6;
	ctx._base_ctx.iovs[1].len = buffer_len - 6;
	ctx._base_ctx.nseg = 2;
	ctx._base_ctx.num_errors = 0;
	ctx._base_ctx.exp_status.sc = sc;

	rc = spdk_nvme_ns_cmd_kvstorev(ns_entry->ns, ns_entry->qpair, key, key_len, buffer_len,
				     run_kvstore_cb, &ctx, flags, 0, nvme_request_reset_sgl, nvme_request_next_sge);
	if (rc != 0) {
		fprintf(stderr, "%s:%d: ERROR: spdk_nvme_ns_cmd_kvstore() returned error: %d\n", __FUNCTION__, __LINE__, rc);
		return -1;
	}

	/* Process completions until cb marks done */
	while (!ctx._base_ctx.cmd_done) {
		spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
	}

	return ctx._base_ctx.num_errors;
}

/*
 * NVME_CMD_KV_LIST
 */

struct run_kvlist_ctx {
	struct io_request	_base_ctx;
	char 			*buffer;
	size_t			buffer_len;

	/* expected return values */
	uint32_t		exp_num_entries;
	char		(*exp_keys)[16];
};


static inline int
verify_keys(void *buffer, size_t buffer_size, struct run_kvlist_ctx *ctx) {
	if (buffer_size < 4) {
		fprintf(stderr, "ERROR: buffer size too small to read key count.\n");
		return -1;
	}

	uint32_t num_keys;
	size_t bytes_read;
	uint16_t len, pad_len;

	num_keys = *(uint32_t *) buffer;
	printf("num keys in the buffer: %d\n", num_keys);

	if (num_keys != ctx->exp_num_entries) {
		fprintf(stderr, "run_kvlist: unexpected number of entries in the buffer: got %d expected %d\n", num_keys, ctx->exp_num_entries);
		ctx->_base_ctx.num_errors++;
		return -1;
	}

	bytes_read = 4;
	void *ptr;

	for  (uint32_t i = 0; i < num_keys; ++i) {
		ptr = buffer + bytes_read;
		if (bytes_read + 1 >= buffer_size) {
			fprintf(stderr, "ERROR: buffer overflow for key %d.\n", i);
			return -1;
		}

		len = *(uint16_t*) ptr;

		pad_len = len;
		if (pad_len % 4) {
			pad_len += 4 - (pad_len % 4);
		}

		bytes_read += pad_len + 2;

		if (bytes_read >= buffer_size) {
			fprintf(stderr, "ERROR: buffer overflow for key %d.\n", i);
			return -1;
		}

		if (len != strlen(ctx->exp_keys[i])) {
			fprintf(stderr, "ERROR: key length mismatch for key %d.\n", i);
			ctx->_base_ctx.num_errors++;
		}

		if (memcmp(ptr + 2, ctx->exp_keys[i], len)) {
			fprintf(stderr, "ERROR: key value mismatch for key %d.\n", i);
			ctx->_base_ctx.num_errors++;
		}

		printf("key[%d] = %.*s\n", i, len, (char*)(ptr + 2));

	}
	return 0;
}

/* Callback for verifying returned results */
static void
run_kvlist_cb(void *arg, const struct spdk_nvme_cpl *cpl) {
	basic_error_checking(arg, cpl, "run_kvlist");
	struct run_kvlist_ctx	*ctx = arg;

	// verify expected count
	uint32_t num_keys = cpl->cdw0;
	if (num_keys != ctx->exp_num_entries) {
		fprintf(stderr, "run_kvlist: unexpected number of list entries: got %d expected %d\n", num_keys, ctx->exp_num_entries);
		ctx->_base_ctx.num_errors++;
		return;
	}

	int rc;

	fprintf(stderr, "total number of keys: %d\n", num_keys);
	rc = verify_keys(ctx->buffer, ctx->buffer_len, ctx);
	if (rc != 0) {
		fprintf(stderr, "Error reading keys from buffer\n");
		return;
	}
	return;

};

static int
run_kvlist(struct ns_entry *ns_entry, char *key, size_t key_len, uint32_t exp_num_keys, uint16_t exp_sc, char (*exp_keys)[16]) {

	int rc;
	struct run_kvlist_ctx 	ctx;

	memset(&ctx, 0, sizeof(struct run_kvlist_ctx));

	ctx._base_ctx.cmd_done = 0;
	ctx._base_ctx.num_errors = 0;

	/* Currently testing against an unpopulated device */
	ctx.exp_num_entries = exp_num_keys;
	ctx._base_ctx.exp_status.sc = exp_sc;
	ctx.exp_keys = exp_keys;

	// create buffer
	ctx.buffer_len = 16384;
	ctx.buffer = spdk_zmalloc(ctx.buffer_len, g_block_size, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	ctx._base_ctx.iovs[0].base = ctx.buffer;
	ctx._base_ctx.iovs[0].len = 6;
	ctx._base_ctx.iovs[1].base = ctx.buffer + 6;
	ctx._base_ctx.iovs[1].len = ctx.buffer_len - 6;
	ctx._base_ctx.nseg = 2;

	rc = spdk_nvme_ns_cmd_kvlistv(ns_entry->ns, ns_entry->qpair, key, key_len,
				     ctx.buffer_len,
				     run_kvlist_cb, &ctx, 0, nvme_request_reset_sgl, nvme_request_next_sge);
	if (rc != 0) {
		fprintf(stderr, "run_kvlist: ERROR: spdk_nvme_ns_cmd_kvlist() returned error: %d\n", rc);
		spdk_free(ctx.buffer);
		return -1;
	}

	/* Process completions until cb marks done */
	while (!ctx._base_ctx.cmd_done) {
		spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
	}
	spdk_free(ctx.buffer);
	return ctx._base_ctx.num_errors;
}

struct run_kvexist_ctx {
	struct io_request	_base_ctx;
};

/* Callback for verifying returned results */
static void
run_kvexist_cb(void *arg, const struct spdk_nvme_cpl *cpl) {
	basic_error_checking(arg, cpl, "run_kvexist");
}

static int
run_kvexist(struct ns_entry *ns_entry, char *key, size_t key_len, uint16_t exp_sc) {

	int rc;
	struct run_kvexist_ctx 	ctx;

	memset(&ctx, 0, sizeof(struct run_kvexist_ctx));

	ctx._base_ctx.num_errors = 0;

	ctx._base_ctx.exp_status.sc = exp_sc;

	rc = spdk_nvme_ns_cmd_kvexist(ns_entry->ns, ns_entry->qpair, key, key_len,
				      run_kvexist_cb, &ctx, 0);
	if (rc != 0) {
		fprintf(stderr, "run_kvexist: ERROR: spdk_nvme_ns_cmd_kvexist() returned error: %d\n", rc);
		return -1;
	}

	/* Process completions until cb marks done */
	while (!ctx._base_ctx.cmd_done) {
		spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
	}

	return ctx._base_ctx.num_errors;
}


struct run_kvdelete_ctx {
	struct io_request	_base_ctx;
};

/* Callback for verifying returned results */
static void
run_kvdelete_cb(void *arg, const struct spdk_nvme_cpl *cpl) {
	basic_error_checking(arg, cpl, "run_kvdelete");
}

static int
run_kvdelete(struct ns_entry *ns_entry, char *key, size_t key_len, uint16_t exp_sc) {

	int rc;
	struct run_kvdelete_ctx 	ctx;

	memset(&ctx, 0, sizeof(struct run_kvdelete_ctx));

	ctx._base_ctx.num_errors = 0;

	ctx._base_ctx.exp_status.sc = exp_sc;

	rc = spdk_nvme_ns_cmd_kvdelete(ns_entry->ns, ns_entry->qpair, key, key_len,
				      run_kvdelete_cb, &ctx, 0);
	if (rc != 0) {
		fprintf(stderr, "run_kvdelete: ERROR: spdk_nvme_ns_cmd_kvdelete() returned error: %d\n", rc);
		return -1;
	}

	/* Process completions until cb marks done */
	while (!ctx._base_ctx.cmd_done) {
		spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
	}

	return ctx._base_ctx.num_errors;
}


struct run_kvretrieve_ctx {
	struct io_request	_base_ctx;
	char 			*buffer;
	size_t			buffer_len;
	char			*expected_value;
	size_t			expected_value_len;
	size_t			offset;
};

/* Callback for verifying returned results */
static void
run_kvretrieve_cb(void *arg, const struct spdk_nvme_cpl *cpl) {
	basic_error_checking(arg, cpl, "run_kvretrieve");
	struct run_kvretrieve_ctx	*ctx = arg;

	if (ctx->_base_ctx.exp_status.sc == SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST) {
		return;
	}

	uint32_t total_value_size = cpl->cdw0;
	if (total_value_size != ctx->expected_value_len) {
		fprintf(stderr, "run_kvretrieve: unexpected value size: got %u expected %lu\n", total_value_size, ctx->expected_value_len);
		ctx->_base_ctx.num_errors++;
	}

	int actual_len = MIN(ctx->buffer_len, total_value_size - ctx->offset);
	if (strncmp(ctx->buffer, ctx->expected_value + ctx->offset, actual_len) != 0) {
		fprintf(stderr, "run_kvretrieve: unexpected value: got: %.*s\n expected: %.*s\n", actual_len, ctx->buffer, actual_len, ctx->expected_value);
		ctx->_base_ctx.num_errors++;
	}

};

static int
run_kvretrieve(struct ns_entry *ns_entry, char *key, size_t key_len, uint16_t exp_sc, char *expected_value, size_t expected_value_len) {

	int rc;
	struct run_kvretrieve_ctx ctx;

	memset(&ctx, 0, sizeof(struct run_kvretrieve_ctx));

	ctx._base_ctx.cmd_done = 0;
	ctx._base_ctx.num_errors = 0;

	ctx._base_ctx.exp_status.sc = exp_sc;
	ctx.expected_value = expected_value;
	ctx.expected_value_len = expected_value_len;

	// create buffer
	ctx.buffer_len = 200;
	ctx.buffer = spdk_zmalloc(ctx.buffer_len, g_block_size, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

	ctx._base_ctx.iovs[0].base = ctx.buffer;
	ctx._base_ctx.iovs[0].len = 6;
	ctx._base_ctx.iovs[1].base = ctx.buffer + 6;
	ctx._base_ctx.iovs[1].len = ctx.buffer_len - 6;
	ctx._base_ctx.nseg = 2;

	for (size_t i = 0; i < expected_value_len; i += ctx.buffer_len) {
		ctx.offset = i;
		rc = spdk_nvme_ns_cmd_kvretrievev(ns_entry->ns, ns_entry->qpair, key, key_len,
							ctx.buffer_len,
							run_kvretrieve_cb, &ctx, i, 0, nvme_request_reset_sgl, nvme_request_next_sge);
		if (rc != 0) {
			fprintf(stderr, "run_kvretrieve: ERROR: spdk_nvme_ns_cmd_kvretrieve() returned error: %d\n", rc);
			spdk_free(ctx.buffer);
			return -1;
		}

		/* Process completions until cb marks done */
		while (!ctx._base_ctx.cmd_done) {
			spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
		}
		ctx._base_ctx.cmd_done = 0;
	}
	spdk_free(ctx.buffer);
	return ctx._base_ctx.num_errors;
}


/* used for both kvselect_send and kvselect_retrieve */
struct run_kvselect_ctx {
	struct io_request	_base_ctx;
	char			*query;
	char			*buffer;
	size_t			buffer_len;

	uint32_t 		select_id;

	/* for validation of the content of the result */
	char			*expected_value;
	size_t			expected_value_len;
	size_t			offset;
};

/* Callback for verifying returned results */
static void
run_kvselect_send_cb(void *arg, const struct spdk_nvme_cpl *cpl) {
	basic_error_checking(arg, cpl, "run_kvselect_send");
	struct run_kvselect_ctx	*ctx = arg;

	/* set select ID */
	ctx->select_id = cpl->cdw0;
}

static int
run_kvselect_send(struct ns_entry *ns_entry, char *key, size_t key_len, char *query, uint32_t *select_id, uint16_t exp_sc) {

	int rc;
	struct run_kvselect_ctx 	ctx;

	memset(&ctx, 0, sizeof(struct run_kvselect_ctx));
	ctx.query = query;
	ctx._base_ctx.exp_status.sc = exp_sc;

	ctx._base_ctx.iovs[0].base = query;
	ctx._base_ctx.iovs[0].len = 6;
	ctx._base_ctx.iovs[1].base = query + 6;
	ctx._base_ctx.iovs[1].len = strlen(query) + 1 - 6;
	ctx._base_ctx.nseg = 2;

	rc = spdk_nvme_ns_cmd_kvselect_sendv(ns_entry->ns, ns_entry->qpair, key, key_len,
					    strlen(query), SPDK_NVME_KV_DATATYPE_PARQUET, SPDK_NVME_KV_DATATYPE_CSV,
					    SPDK_NVME_KV_SELECT_OUTPUT_HEADER,
					    run_kvselect_send_cb, &ctx, 0, nvme_request_reset_sgl, nvme_request_next_sge);
	if (rc != 0) {
		fprintf(stderr, "run_kvselect_send: ERROR: spdk_nvme_ns_cmd_kvselect_sendv() returned error: %d\n", rc);
		return -1;
	}

	/* Process completions until cb marks done */
	while (!ctx._base_ctx.cmd_done) {
		spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
	}

	if (ctx._base_ctx.num_errors == 0) {
		*select_id = ctx.select_id;
	}

	return ctx._base_ctx.num_errors;
}

static void
run_kvselect_retr_cb(void *arg, const struct spdk_nvme_cpl *cpl) {
	basic_error_checking(arg, cpl, "run_kvselect_retr");
	struct run_kvselect_ctx	*ctx = arg;

	uint32_t total_value_size = cpl->cdw0;
	if (total_value_size != ctx->expected_value_len) {
		fprintf(stderr, "run_kvselect_retr: unexpected value size: got %u expected %lu\n", total_value_size, ctx->expected_value_len);
		ctx->_base_ctx.num_errors++;
	}

	int actual_len = MIN(ctx->buffer_len, total_value_size - ctx->offset);
	if (strncmp(ctx->buffer, ctx->expected_value + ctx->offset, actual_len) != 0) {
		fprintf(stderr, "run_kvselect_retr: unexpected value: got: %.*s\n expected: %.*s\n", actual_len, ctx->buffer, actual_len, ctx->expected_value + ctx->offset);
		ctx->_base_ctx.num_errors++;
	}

	return;
}

static int
run_kvselect_retr(struct ns_entry *ns_entry, uint32_t select_id, void *buffer, size_t buffer_size, uint16_t exp_sc, char *exp_value) {
	int rc;
	struct run_kvselect_ctx 	ctx;

	memset(&ctx, 0, sizeof(struct run_kvselect_ctx));
	ctx._base_ctx.exp_status.sc = exp_sc;
	ctx.select_id = select_id;
	ctx.buffer = buffer;
	ctx.buffer_len = buffer_size;
	ctx.expected_value = exp_value;
	ctx.expected_value_len = strlen(exp_value);

	ctx._base_ctx.iovs[0].base = buffer;
	ctx._base_ctx.iovs[0].len = 6;
	ctx._base_ctx.iovs[1].base = buffer + 6;
	ctx._base_ctx.iovs[1].len = buffer_size - 6;
	ctx._base_ctx.nseg = 2;

	for (ctx.offset = 0; ctx.offset < ctx.expected_value_len; ctx.offset += buffer_size) {
		rc = spdk_nvme_ns_cmd_kvselect_retrievev(ns_entry->ns, ns_entry->qpair, select_id, ctx.offset,
				buffer_size, SPDK_NVME_KV_SELECT_NO_FREE, run_kvselect_retr_cb, &ctx, 0, nvme_request_reset_sgl, nvme_request_next_sge);
		if (rc != 0) {
			fprintf(stderr, "%s: ERROR: spdk_nvme_ns_cmd_kvselect_retrieve() returned error: %d\n", __FUNCTION__, rc);
			return -1;
		}

		/* Process completions until cb marks done */
		while (!ctx._base_ctx.cmd_done) {
			spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
		}

		ctx._base_ctx.cmd_done = 0;
	}

	return ctx._base_ctx.num_errors;
}

#define ABORT_ON_FAIL(rc, cmd, key) if (rc!=0) {fprintf(stderr, "%s:%d: %s failed for key %s\nTry running with 'reset' option to clear state then run again.\n", __FUNCTION__, __LINE__, cmd, key); return -1; };

static int
test_select(struct ns_entry *ns_entry) {

	size_t buffer_len = 16384;
	FILE *st;
	int rc;
	uint32_t select_id = 0;

	char *buffer = spdk_zmalloc(buffer_len, g_block_size, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

	st = fopen("data.parquet", "rb");
	if (!st) {
		fprintf(stderr, "could not open data file\n");
		return -1;
	}
	int bytes = fread(buffer, sizeof(char), buffer_len, st);
	fclose(st);

	rc = run_kvstore(ns_entry, g_test_key4, strlen(g_test_key4), buffer, bytes, 0, 0);
	ABORT_ON_FAIL(rc, "KV_STORE", g_test_key4);

	spdk_free(buffer);
	buffer = spdk_zmalloc(buffer_len, g_block_size, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	strcpy(buffer, "select s_name,s_address,s_city from s3object where s_nation = 'UNITED STATES'");

	rc = run_kvselect_send(ns_entry, g_test_key4, strlen(g_test_key4), buffer, &select_id, SPDK_NVME_SC_SUCCESS);
	ABORT_ON_FAIL(rc, "KV_SELECT_SEND", g_test_key4);

	spdk_free(buffer);
	size_t retr_buffer_len = 200;
	buffer = spdk_zmalloc(retr_buffer_len, g_block_size, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	char *expected_value = "s_name,s_address,s_city\n"
									"Supplier#000000010,9QtKQKXK24f,UNITED ST0\n"
									"Supplier#000000019,NN17XNz0Dpmn,UNITED ST9\n"
									"Supplier#000000046,\"N,6964Lnc2fNgMZV1VJV9y\",UNITED ST4\n"
									"Supplier#000000049,ewArUFQOl,UNITED ST7\n"
									"Supplier#000000055,dAN28JcaMkX,UNITED ST5\n"
									"Supplier#000000064,\"wS,hHEibrFlCfN6I9xyPxSZK\",UNITED ST1\n"
									"Supplier#000000084,oO2H4fI1kaBmgchJ,UNITED ST1\n"
									"Supplier#000000087,5ovT6anHSsD1T,UNITED ST4\n";

	rc = run_kvselect_retr(ns_entry, select_id, buffer, retr_buffer_len, SPDK_NVME_SC_SUCCESS, expected_value);
	ABORT_ON_FAIL(rc, "KV_SELECT_RETRIEVE", g_test_key4);
	spdk_free(buffer);
	rc = run_kvdelete(ns_entry, g_test_key4, strlen(g_test_key4), SPDK_NVME_SC_SUCCESS);
	ABORT_ON_FAIL(rc, "KV_DELETE", g_test_key4);
	
	return 0;

}

static int
run_tests(void)
{
	struct ns_entry	*ns_entry;
	int rc;

	ns_entry = g_namespaces;

	/*
	 * Perform a series of commands against a working device
	 * At the moment we assume the device is in a empty state with no stored keys
	 */

	// Use ~ as first character to isolate from most other keys
	char *prefix = "~TEST";

	rc = run_kvlist(ns_entry, prefix, strlen(prefix), 0, SPDK_NVME_SC_SUCCESS, NULL);
	ABORT_ON_FAIL(rc, "KV_LIST", prefix);

	size_t buffer_len = 1024;
	char *buffer = spdk_zmalloc(buffer_len, g_block_size, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

	strncpy(buffer, "Introduction\n"
                       "The NVMe driver is a C library that may be linked directly into an application that provides direct, zero-copy data transfer to and from NVMe SSDs. It is entirely passive, meaning that it spawns no threads and only performs actions in response to function calls from the application itself. The library controls NVMe devices by directly mapping the PCI BAR into the local process and performing MMIO. I/O is submitted asynchronously via queue pairs and the general flow isn't entirely dissimilar from Linux's libaio.\n"
                       "\n"
                       "More recently, the library has been improved to also connect to remote NVMe devices via NVMe over Fabrics. Users may now call spdk_nvme_probe() on both local PCI busses and on remote NVMe over Fabrics discovery services. The API is otherwise unchanged.\n"
                       "\n"
                       "Examples\n"
                       "Getting Start with Hello World\n"
                       "There are a number of examples provided that demonstrate how to use the NVMe library. They are all in the examples/nvme directory in the repository. The best place to start is hello_world.\n"
											 , 1015);
	/* test key 1*/

	/* Verify key does not exist */
	rc = run_kvexist(ns_entry, g_test_key1, strlen(g_test_key1), SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST);
	ABORT_ON_FAIL(rc, "KV_EXIST (pre-store)", g_test_key1);

	/* Store a key then verify with kvexist and kvlist */
	rc = run_kvstore(ns_entry, g_test_key1, strlen(g_test_key1), buffer, buffer_len, 0, SPDK_NVME_SC_SUCCESS);
	ABORT_ON_FAIL(rc, "KV_STORE", g_test_key1);

	rc = run_kvlist(ns_entry, prefix, strlen(prefix), 1, SPDK_NVME_SC_SUCCESS, g_test_keys);
	ABORT_ON_FAIL(rc, "KV_LIST", prefix);

	rc = run_kvexist(ns_entry, g_test_key1, strlen(g_test_key1), SPDK_NVME_SC_SUCCESS);
	ABORT_ON_FAIL(rc, "KV_EXIST (post-store)", g_test_key1);

	/* test key 2 */

	/* Verify key does not exist */
	rc = run_kvexist(ns_entry, g_test_key2, strlen(g_test_key2), SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST);
	ABORT_ON_FAIL(rc, "KV_EXIST", g_test_key2);

	rc = run_kvstore(ns_entry, g_test_key2, strlen(g_test_key2), buffer, buffer_len, 0, SPDK_NVME_SC_SUCCESS);
	ABORT_ON_FAIL(rc, "KV_STORE", g_test_key2);

	rc = run_kvlist(ns_entry, prefix, strlen(prefix), 2, SPDK_NVME_SC_SUCCESS, g_test_keys);
	ABORT_ON_FAIL(rc, "KV_LIST", prefix);

	rc = run_kvexist(ns_entry, g_test_key2, strlen(g_test_key2), SPDK_NVME_SC_SUCCESS);
	ABORT_ON_FAIL(rc, "KV_EXIST (post-store)", g_test_key2);

	/* Try to store with a MUST_NOT_EXIST flag set, should pass */
	rc = run_kvstore(ns_entry, g_test_key2, strlen(g_test_key2), buffer, buffer_len, SPDK_NVME_KV_STORE_FLAG_MUST_NOT_EXIST, SPDK_NVME_SC_KEY_EXISTS);
	ABORT_ON_FAIL(rc, "KV_STORE", g_test_key2);

	/* test key 3 */

	/* Verify key does not exist */
	rc = run_kvexist(ns_entry, g_test_key3, strlen(g_test_key3), SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST);
	ABORT_ON_FAIL(rc, "KV_EXIST", g_test_key3);

	/* Try to store with a MUST_EXIST flag set, should fail */
	rc = run_kvstore(ns_entry, g_test_key3, strlen(g_test_key3), buffer, buffer_len, SPDK_NVME_KV_STORE_FLAG_MUST_EXIST, SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST);
	ABORT_ON_FAIL(rc, "KV_STORE", g_test_key3);

	/* Try to store with a MUST_NOT_EXIST flag set, should pass */
	rc = run_kvstore(ns_entry, g_test_key3, strlen(g_test_key3), buffer, buffer_len, SPDK_NVME_KV_STORE_FLAG_MUST_NOT_EXIST, SPDK_NVME_SC_SUCCESS);
	ABORT_ON_FAIL(rc, "KV_STORE", g_test_key3);

	rc = run_kvlist(ns_entry, prefix, strlen(prefix), 3, SPDK_NVME_SC_SUCCESS, g_test_keys);
	ABORT_ON_FAIL(rc, "KV_LIST", prefix);

	rc = run_kvexist(ns_entry, g_test_key3, strlen(g_test_key3), SPDK_NVME_SC_SUCCESS);
	ABORT_ON_FAIL(rc, "KV_EXIST (post-store)", g_test_key3);

	rc = run_kvretrieve(ns_entry, g_test_key1, strlen(g_test_key1), SPDK_NVME_SC_SUCCESS, buffer, buffer_len);
	ABORT_ON_FAIL(rc, "KV_RETRIEVE", g_test_key1);

	/* Try to retrieve a key that doesn't exist, should fail */
	rc = run_kvretrieve(ns_entry, prefix, strlen(prefix), SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST, NULL, 0);
	ABORT_ON_FAIL(rc, "KV_RETRIEVE", prefix);

	/* Delete the 3 keys */
	rc = run_kvdelete(ns_entry, g_test_key1, strlen(g_test_key1), SPDK_NVME_SC_SUCCESS);
	ABORT_ON_FAIL(rc, "KV_DELETE", g_test_key1);

	rc = run_kvdelete(ns_entry, g_test_key2, strlen(g_test_key2), SPDK_NVME_SC_SUCCESS);
	ABORT_ON_FAIL(rc, "KV_DELETE", g_test_key2);

	rc = run_kvdelete(ns_entry, g_test_key3, strlen(g_test_key3), SPDK_NVME_SC_SUCCESS);
	ABORT_ON_FAIL(rc, "KV_DELETE", g_test_key3);

	rc = run_kvdelete(ns_entry, g_test_key3, strlen(g_test_key3), SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST);
	ABORT_ON_FAIL(rc, "KV_DELETE", g_test_key3);

	rc = run_kvlist(ns_entry, prefix, strlen(prefix), 0, SPDK_NVME_SC_SUCCESS, NULL);
	ABORT_ON_FAIL(rc, "KV_LIST", prefix);

	spdk_free(buffer);
	return test_select(ns_entry);
}


static int
parse_args(int argc, char **argv, struct spdk_env_opts *env_opts)
{
	if (argc >= 2) {
		if (!strncmp("reset", argv[1], 5)) {
			g_reset_state = true;
		}
	}

	return 0;
}

static int
reset(void) {
	struct ns_entry	*ns_entry;

	ns_entry = g_namespaces;
	run_kvdelete(ns_entry, g_test_key1, strlen(g_test_key1), 0);
	run_kvdelete(ns_entry, g_test_key2, strlen(g_test_key2), 0);
	run_kvdelete(ns_entry, g_test_key3, strlen(g_test_key3), 0);
	run_kvdelete(ns_entry, g_test_key4, strlen(g_test_key4), 0);

	return 0;
}

int
main(int argc, char **argv)
{
	int			rc;
	struct spdk_env_opts	opts;

	spdk_env_opts_init(&opts);
	rc = parse_args(argc, argv, &opts);
	if (rc != 0) {
		return rc;
	}

	opts.name = "key_value";
	opts.shm_id = 0;
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	fprintf(stderr, "Initializing NVMe Controllers\n");

	rc = spdk_nvme_probe(g_use_trid ? &g_trid : NULL, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	if (g_namespaces == NULL) {
		fprintf(stderr, "no NVMe controllers found\n");
		return 1;
	}

	struct ns_entry	*ns_entry;
	ns_entry = g_namespaces;

	ns_entry->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, NULL, 0);
	if (ns_entry->qpair == NULL) {
		fprintf(stderr, "ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
		return -1;
	}

	fprintf(stderr, "Initialization complete.\n");

	if (g_reset_state) {
		return reset();
	}

	return run_tests();
}
