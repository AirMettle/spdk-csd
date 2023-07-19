/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 AirMettle, Inc.
 *   All rights reserved.
 */

#include "spdk_cunit.h"
#include "spdk_internal/mock.h"

#include "nvme/nvme_kv.c"

#include "common/lib/test_env.c"


pid_t g_spdk_nvme_pid;

static struct nvme_request *g_request = NULL;

/*
 * Mock code from QEMU implemenation to verify _nvme_cmd_kv_add_key()
 */

/* shortcut to use local endian.h as replacement for qemu defines */
#define le32_to_cpu(x)  from_le32(&x)

/* Copied from nvme-kv qemu implementation */

#define NVM_KV_GET_STORE_CMD_OPTIONS(dw) ((le32_to_cpu(dw) >> 8) & 0xff)
#define NVM_KV_GET_KEY_LENGTH(dw) (le32_to_cpu(dw) & 0xff)

typedef struct __attribute__((packed)) NvmeSglDescriptor {
    uint64_t addr;
    uint32_t len;
    uint8_t  rsvd[3];
    uint8_t  type;
} NvmeSglDescriptor;

typedef union NvmeCmdDptr {
    struct {
        uint64_t    prp1;
        uint64_t    prp2;
    };
    NvmeSglDescriptor sgl;
} NvmeCmdDptr;

typedef struct __attribute__((packed)) NvmeKvCmd {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    nsid;				/* dword 1 */
    uint32_t    key_word_1;			/* dword 2 */
    uint32_t    key_word_2;			/* dword 3 */
    uint64_t    mptr;
    NvmeCmdDptr dptr;
    uint32_t    host_buffer_size;		/* dword 10 */
    uint32_t    key_length_and_store_options;	/* dword 11 */
    uint32_t    read_offset;			/* dword 12 */
    uint32_t    select_id;			/* dword 13 */
    uint32_t    key_word_3;			/* dword 14 */
    uint32_t    key_word_4;			/* dword 15 */
} NvmeKvCmd;

/* helper function from NVMe-KV in QEMU */
static int nvme_kv_get_key(NvmeKvCmd *cmd, unsigned char *key_buf, size_t *key_len) {

   size_t kv_length = NVM_KV_GET_KEY_LENGTH(cmd->key_length_and_store_options);

   if (kv_length == 0 || kv_length > 16) {
      return -1;
   }

   uint32_t words[4];

   words[0] = le32_to_cpu(cmd->key_word_4);
   words[1] = le32_to_cpu(cmd->key_word_3);
   words[2] = le32_to_cpu(cmd->key_word_2);
   words[3] = le32_to_cpu(cmd->key_word_1);

   unsigned char *p = key_buf;
   size_t len = kv_length;
   for (int i=0; i<4 && (len > 0); i++) {
       for (int j=3; j>=0 && (len > 0); j--, len--, p++) {
          *p = (words[i] >> (8 * j)) & 0xFF;
       }
   }

   *key_len = kv_length;
   return 0;
}

int
nvme_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	g_request = req;

	return 0;
}

static void
prepare_for_test(struct spdk_nvme_ns *ns, struct spdk_nvme_ctrlr *ctrlr,
		 struct spdk_nvme_qpair *qpair)
{
	uint32_t num_requests = 32;
	uint32_t i;

	memset(ctrlr, 0, sizeof(*ctrlr));

	/*
	 * Clear the flags field - we especially want to make sure the SGL_SUPPORTED flag is not set
	 *  so that we test the SGL splitting path.
	 */
	ctrlr->flags = 0;
	ctrlr->min_page_size = 4096;
	ctrlr->page_size = 4096;
	memset(&ctrlr->opts, 0, sizeof(ctrlr->opts));

	memset(ns, 0, sizeof(*ns));
	ns->ctrlr = ctrlr;

	memset(qpair, 0, sizeof(*qpair));
	qpair->ctrlr = ctrlr;
	qpair->req_buf = calloc(num_requests, sizeof(struct nvme_request));
	SPDK_CU_ASSERT_FATAL(qpair->req_buf != NULL);

	for (i = 0; i < num_requests; i++) {
		struct nvme_request *req = qpair->req_buf + i * sizeof(struct nvme_request);

		req->qpair = qpair;
		STAILQ_INSERT_HEAD(&qpair->free_req, req, stailq);
	}

	g_request = NULL;
}

static void
cleanup_after_test(struct spdk_nvme_qpair *qpair)
{
	free(qpair->req_buf);
}

/*
 * Unit Tests
 */
static void
test_add_key(void) {

	int rc;
	char key[17];
	size_t key_len;

	struct spdk_nvme_cmd	cmd;

	/* expect fail - key length > 16 */
	memset(&cmd, 0, sizeof(struct spdk_nvme_cmd));
	rc = _nvme_cmd_kv_add_key(&cmd, "12345678901234567", 17, 0);
	CU_ASSERT(rc != 0);

	/* add key and check with qmeu function */
	char test_key[] = "abcdefghijklmnop";
	memset(&cmd, 0, sizeof(struct spdk_nvme_cmd));
	rc = _nvme_cmd_kv_add_key(&cmd, test_key, strlen(test_key), 0);
	CU_ASSERT(rc == 0);

	rc = nvme_kv_get_key((NvmeKvCmd *)&cmd, key, &key_len);
	CU_ASSERT_NSTRING_EQUAL(test_key, key, strlen(test_key));
	CU_ASSERT(key_len == strlen(test_key));

	char test_key2[] = "foo";
	memset(&cmd, 0, sizeof(struct spdk_nvme_cmd));
	rc = _nvme_cmd_kv_add_key(&cmd, test_key2, strlen(test_key2), 0);
	CU_ASSERT(rc == 0);

	rc = nvme_kv_get_key((NvmeKvCmd *)&cmd, key, &key_len);
	CU_ASSERT_NSTRING_EQUAL(test_key2, key, strlen(test_key2));
	CU_ASSERT(key_len == strlen(test_key2));

	char test_key3[] = "foobar";
	memset(&cmd, 0, sizeof(struct spdk_nvme_cmd));
	rc = _nvme_cmd_kv_add_key(&cmd, test_key3, strlen(test_key3), 0);
	CU_ASSERT(rc == 0);

	rc = nvme_kv_get_key((NvmeKvCmd *)&cmd, key, &key_len);
	CU_ASSERT_NSTRING_EQUAL(test_key3, key, strlen(test_key3));
	CU_ASSERT(key_len == strlen(test_key3));


	/* add key with storage option */
	memset(&cmd, 0, sizeof(struct spdk_nvme_cmd));
	cmd.opc = SPDK_NVME_OPC_KV_STORE;
	rc = _nvme_cmd_kv_add_key(&cmd, test_key, strlen(test_key), SPDK_NVME_KV_STORE_FLAG_APPEND | SPDK_NVME_KV_STORE_FLAG_MUST_EXIST);
	CU_ASSERT(rc == 0);

	rc = nvme_kv_get_key((NvmeKvCmd *)&cmd, key, &key_len);
	CU_ASSERT_NSTRING_EQUAL(test_key, key, strlen(test_key));
	CU_ASSERT(key_len == strlen(test_key));

	uint8_t flags = (cmd.cdw11 >> 8) & 0xff;

	CU_ASSERT(flags ==  (SPDK_NVME_KV_STORE_FLAG_APPEND | SPDK_NVME_KV_STORE_FLAG_MUST_EXIST));

	/* should fail due to incompatible flags */
	memset(&cmd, 0, sizeof(struct spdk_nvme_cmd));
	cmd.opc = SPDK_NVME_OPC_KV_STORE;
	rc = _nvme_cmd_kv_add_key(&cmd, test_key, strlen(test_key), SPDK_NVME_KV_STORE_FLAG_MUST_NOT_EXIST | SPDK_NVME_KV_STORE_FLAG_MUST_EXIST);
	CU_ASSERT(rc != 0);

	/* should fail due to invalid flags */
	memset(&cmd, 0, sizeof(struct spdk_nvme_cmd));
	cmd.opc = SPDK_NVME_OPC_KV_STORE;
	rc = _nvme_cmd_kv_add_key(&cmd, test_key, strlen(test_key), 0x04);
	CU_ASSERT(rc != 0);

	return;
};

static void
dummy_test_cb(void *arg, const struct spdk_nvme_cpl *cpl) {};

static void
test_spdk_nvme_ns_cmd_kvlist(void) {
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;
	struct spdk_nvme_cmd	*cmd;
	int			rc = 0;
	char			*prefix;
	size_t			prefix_len;
	char			*buffer = NULL;
	uint32_t		buffer_size;
	int			cb_arg;

	buffer_size = 1024;
	buffer = malloc(buffer_size);
	SPDK_CU_ASSERT_FATAL(buffer != NULL);

	prefix = "TEST";
	prefix_len = strlen(prefix);

	prepare_for_test(&ns, &ctrlr, &qpair);

	rc = spdk_nvme_ns_cmd_kvlist(&ns, &qpair, prefix, prefix_len, buffer, buffer_size, dummy_test_cb, &cb_arg, 0);

	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->num_children == 0);
	CU_ASSERT(g_request->cb_arg == &cb_arg);
	CU_ASSERT(g_request->cb_fn == dummy_test_cb);

	cmd = &g_request->cmd;
	CU_ASSERT(cmd->opc == SPDK_NVME_OPC_KV_LIST);
	CU_ASSERT(cmd->nsid == ns.id);

	CU_ASSERT(cmd->cdw10 == buffer_size);
	CU_ASSERT(cmd->cdw11 == strlen(prefix));

	char key[16];
	size_t key_len;
	rc = nvme_kv_get_key((NvmeKvCmd *)cmd, key, &key_len);
	CU_ASSERT(rc == 0);
	CU_ASSERT_NSTRING_EQUAL(prefix, key, strlen(prefix));
	CU_ASSERT(key_len == strlen(prefix));

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);
	free(buffer);
}

static void
test_spdk_nvme_ns_cmd_kvexist(void) {
	struct spdk_nvme_ns	 ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;
	struct spdk_nvme_cmd	*cmd;
	int			rc = 0;
	char			*test_key;
	size_t			test_key_len;
	int			cb_arg;

	test_key = "TEST_12345";
	test_key_len = strlen(test_key);

	prepare_for_test(&ns, &ctrlr, &qpair);

	rc = spdk_nvme_ns_cmd_kvexist(&ns, &qpair, test_key, test_key_len, dummy_test_cb, &cb_arg, 0);

	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->num_children == 0);
	CU_ASSERT(g_request->cb_arg == &cb_arg);
	CU_ASSERT(g_request->cb_fn == dummy_test_cb);

	cmd = &g_request->cmd;
	CU_ASSERT(cmd->opc == SPDK_NVME_OPC_KV_EXIST);
	CU_ASSERT(cmd->nsid == ns.id);

	CU_ASSERT(cmd->cdw11 = test_key_len);

	char key[16];
	size_t key_len;
	rc = nvme_kv_get_key((NvmeKvCmd *)cmd, key, &key_len);
	CU_ASSERT(rc == 0);
	CU_ASSERT_NSTRING_EQUAL(test_key, key, test_key_len);
	CU_ASSERT(key_len == test_key_len);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);
}

static void
test_spdk_nvme_ns_cmd_kvdelete(void) {
	struct spdk_nvme_ns	 ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;
	struct spdk_nvme_cmd	*cmd;
	int			rc = 0;
	char			*test_key;
	size_t			test_key_len;
	int			cb_arg;

	test_key = "TEST_DELETE";
	test_key_len = strlen(test_key);

	prepare_for_test(&ns, &ctrlr, &qpair);

	rc = spdk_nvme_ns_cmd_kvdelete(&ns, &qpair, test_key, test_key_len, dummy_test_cb, &cb_arg, 0);

	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->num_children == 0);
	CU_ASSERT(g_request->cb_arg == &cb_arg);
	CU_ASSERT(g_request->cb_fn == dummy_test_cb);

	cmd = &g_request->cmd;
	CU_ASSERT(cmd->opc == SPDK_NVME_OPC_KV_DELETE);
	CU_ASSERT(cmd->nsid == ns.id);

	CU_ASSERT(cmd->cdw11 = test_key_len);

	char key[16];
	size_t key_len;
	rc = nvme_kv_get_key((NvmeKvCmd *)cmd, key, &key_len);
	CU_ASSERT(rc == 0);
	CU_ASSERT_NSTRING_EQUAL(test_key, key, test_key_len);
	CU_ASSERT(key_len == test_key_len);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);
}

static void
test_spdk_nvme_ns_cmd_kvstore(void) {
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;
	struct spdk_nvme_cmd	*cmd;
	int			rc = 0;
	char			*buffer = NULL;
	char			*test_key;
	size_t			test_key_len;
	uint32_t		buffer_size;
	int			cb_arg;

	buffer_size = 1024;
	buffer = malloc(buffer_size);
	SPDK_CU_ASSERT_FATAL(buffer != NULL);

	test_key = "STORE_TEST";
	test_key_len = strlen(test_key);
	prepare_for_test(&ns, &ctrlr, &qpair);

	rc = spdk_nvme_ns_cmd_kvstore(&ns, &qpair, test_key, test_key_len, buffer, buffer_size,
				      dummy_test_cb, &cb_arg, 0, 0);

	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->num_children == 0);
	CU_ASSERT(g_request->cb_arg == &cb_arg);
	CU_ASSERT(g_request->cb_fn == dummy_test_cb);

	cmd = &g_request->cmd;
	CU_ASSERT(cmd->opc == SPDK_NVME_OPC_KV_STORE);
	CU_ASSERT(cmd->nsid == ns.id);

	CU_ASSERT(cmd->cdw10 == buffer_size);
	CU_ASSERT(cmd->cdw11 == test_key_len);

	char key[16];
	size_t key_len;
	rc = nvme_kv_get_key((NvmeKvCmd *)cmd, key, &key_len);
	CU_ASSERT(rc == 0);
	CU_ASSERT_NSTRING_EQUAL(test_key, key, test_key_len);
	CU_ASSERT(key_len == test_key_len);

	nvme_free_request(g_request);

	/* Additional tests for flag validation */
	uint8_t test_flags;
	uint32_t flags;

	/* incompatible flags - should fail */
	test_flags = SPDK_NVME_KV_STORE_FLAG_MUST_EXIST | SPDK_NVME_KV_STORE_FLAG_MUST_NOT_EXIST;
	rc = spdk_nvme_ns_cmd_kvstore(&ns, &qpair, test_key, test_key_len, buffer, buffer_size,
				      dummy_test_cb, &cb_arg, test_flags, 0);
	CU_ASSERT(rc != 0);
	nvme_free_request(g_request);

	/* invalid flag value - should fail */
	test_flags = (1U << 2);
	rc = spdk_nvme_ns_cmd_kvstore(&ns, &qpair, test_key, test_key_len, buffer, buffer_size,
				      dummy_test_cb, &cb_arg, test_flags, 0);
	CU_ASSERT(rc != 0);
	nvme_free_request(g_request);

	/* verify flag values */
	test_flags = SPDK_NVME_KV_STORE_FLAG_APPEND | SPDK_NVME_KV_STORE_FLAG_MUST_EXIST;
	rc = spdk_nvme_ns_cmd_kvstore(&ns, &qpair, test_key, test_key_len, buffer, buffer_size,
				      dummy_test_cb, &cb_arg, test_flags, 0);
	CU_ASSERT(rc == 0);
	flags = (from_le32(&cmd->cdw11) >> 8) & 0xff;
	CU_ASSERT(flags == test_flags);
	nvme_free_request(g_request);

	cleanup_after_test(&qpair);
	free(buffer);
}

static void
test_spdk_nvme_ns_cmd_kvretrieve(void) {
	struct spdk_nvme_ns             ns;
	struct spdk_nvme_ctrlr          ctrlr;
	struct spdk_nvme_qpair          qpair;
	struct spdk_nvme_cmd		*cmd;
	int                             rc = 0;
	char				*buffer = NULL;
	char				*key;
	size_t				key_len;
	uint32_t			buffer_size;
	int				cb_arg;

	buffer_size = 1024;
	buffer = malloc(buffer_size);
	SPDK_CU_ASSERT_FATAL(buffer != NULL);

	key = "RETRIEVE_TEST";
	key_len = strlen(key);

	prepare_for_test(&ns, &ctrlr, &qpair);

	rc = spdk_nvme_ns_cmd_kvretrieve(&ns, &qpair, key, key_len, buffer, buffer_size, dummy_test_cb, &cb_arg, 10, 0);

	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->num_children == 0);
	CU_ASSERT(g_request->cb_arg == &cb_arg);
	CU_ASSERT(g_request->cb_fn == dummy_test_cb);

	cmd = &g_request->cmd;
	CU_ASSERT(cmd->opc == SPDK_NVME_OPC_KV_RETRIEVE);
	CU_ASSERT(cmd->nsid == ns.id);

	CU_ASSERT(cmd->cdw10 = buffer_size);
	CU_ASSERT(cmd->cdw11 = strlen(key));
	CU_ASSERT(cmd->cdw12 = 10);

	char result_key[16];
	size_t result_key_len;
	rc = nvme_kv_get_key((NvmeKvCmd *)cmd, result_key, &result_key_len);
	CU_ASSERT(rc == 0);
	CU_ASSERT_NSTRING_EQUAL(key, result_key, strlen(key));
	CU_ASSERT(result_key_len == strlen(key));

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);
	free(buffer);
}

static void
test_spdk_nvme_ns_cmd_kvselect_send(void) {
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;
	struct spdk_nvme_cmd	*cmd;
	int			rc = 0;
	char			*test_query;
	char			*test_key;
	size_t			test_key_len;
	int			cb_arg;

	test_query = "SELECT item,qty,price from s3_object";

	test_key = "STORE_TEST";
	test_key_len = strlen(test_key);

	spdk_nvme_kv_datatype input_type = SPDK_NVME_KV_DATATYPE_JSON;
	spdk_nvme_kv_datatype output_type = SPDK_NVME_KV_DATATYPE_PARQUET;
	uint8_t header_opts = SPDK_NVME_KV_SELECT_INPUT_HEADER | SPDK_NVME_KV_SELECT_OUTPUT_HEADER;

	prepare_for_test(&ns, &ctrlr, &qpair);

	rc = spdk_nvme_ns_cmd_kvselect_send(&ns, &qpair, test_key, test_key_len, test_query,
				      	    input_type, output_type, header_opts,
					    dummy_test_cb, &cb_arg, 0);

	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->num_children == 0);

	/* Note we are using an internal callback wrapper so therse are not true */
	// CU_ASSERT(g_request->cb_arg == &cb_arg);
	// CU_ASSERT(g_request->cb_fn == dummy_test_cb);

	cmd = &g_request->cmd;
	CU_ASSERT(cmd->opc == SPDK_NVME_OPC_KV_SEND_SELECT);
	CU_ASSERT(cmd->nsid == ns.id);

	CU_ASSERT(cmd->cdw10 == strlen(test_query));
	CU_ASSERT((from_le32(&cmd->cdw11) & 0xff) == test_key_len);

	char key[16];
	size_t key_len;
	rc = nvme_kv_get_key((NvmeKvCmd *)cmd, key, &key_len);
	CU_ASSERT(rc == 0);
	CU_ASSERT_NSTRING_EQUAL(test_key, key, test_key_len);
	CU_ASSERT(key_len == test_key_len);

	uint32_t flags = from_le32(&cmd->cdw11) >> 8;
	CU_ASSERT((flags & 0xff) == header_opts);
	CU_ASSERT(((flags >> 8) & 0xff) == input_type);
	CU_ASSERT(((flags >> 16) & 0xff) == output_type);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);
}

static void
test_spdk_nvme_ns_cmd_kvselect_retrieve(void) {
	struct spdk_nvme_ns	ns;
	struct spdk_nvme_ctrlr	ctrlr;
	struct spdk_nvme_qpair	qpair;
	struct spdk_nvme_cmd	*cmd;
	char			*buffer = NULL;
	uint32_t		buffer_size;
	int			rc = 0;
	int			cb_arg;

	buffer_size = 1024;
	buffer = malloc(buffer_size);
	SPDK_CU_ASSERT_FATAL(buffer != NULL);

	spdk_nvme_kv_select_opts opts = SPDK_NVME_KV_SELECT_NO_FREE;
	uint32_t offset = 123;
	uint32_t id = 0xdeadbeef;

	prepare_for_test(&ns, &ctrlr, &qpair);

	rc = spdk_nvme_ns_cmd_kvselect_retrieve(&ns, &qpair, id, offset, buffer,
						buffer_size, opts, dummy_test_cb, &cb_arg, 0);

	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_request != NULL);
	CU_ASSERT(g_request->num_children == 0);
	CU_ASSERT(g_request->cb_arg == &cb_arg);
	CU_ASSERT(g_request->cb_fn == dummy_test_cb);

	cmd = &g_request->cmd;
	CU_ASSERT(cmd->opc == SPDK_NVME_OPC_KV_RETRIEVE_SELECT);
	CU_ASSERT(cmd->nsid == ns.id);

	CU_ASSERT((from_le32(&cmd->cdw10)) == buffer_size);
	CU_ASSERT((from_le32(&cmd->cdw11) & 0xff) == opts);
	CU_ASSERT((from_le32(&cmd->cdw12)) == offset);
	CU_ASSERT((from_le32(&cmd->cdw13)) == id);

	nvme_free_request(g_request);
	cleanup_after_test(&qpair);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	/* disable printing log messages */
	spdk_log_set_print_level(SPDK_LOG_DISABLED);

	suite = CU_add_suite("nvme_kv", NULL, NULL);

	CU_ADD_TEST(suite, test_add_key);
	CU_ADD_TEST(suite, test_spdk_nvme_ns_cmd_kvlist);
	CU_ADD_TEST(suite, test_spdk_nvme_ns_cmd_kvexist);
	CU_ADD_TEST(suite, test_spdk_nvme_ns_cmd_kvdelete);
	CU_ADD_TEST(suite, test_spdk_nvme_ns_cmd_kvstore);
	CU_ADD_TEST(suite, test_spdk_nvme_ns_cmd_kvretrieve);
	CU_ADD_TEST(suite, test_spdk_nvme_ns_cmd_kvselect_send);
	CU_ADD_TEST(suite, test_spdk_nvme_ns_cmd_kvselect_retrieve);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}