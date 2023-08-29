/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   Copyright (C) 2023 AirMettle, Inc.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/thread.h"
#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/bdev_zone.h"
#include "spdk/endian.h"
#include "spdk/nvme_spec.h"

static char *g_bdev_name = "Malloc0";

static char *test_key = "test";
static size_t test_key_length = 4;
static uint32_t select_id = 0;

static void hello_kv_send_select(void *arg);
static void hello_kv_retrieve_select(void *arg);
static void hello_kv_store(void *arg);
static void hello_kv_retrieve(void *arg);
static void hello_kv_delete(void *arg);
static void hello_kv_exist(void *arg);
static void hello_kv_list(void *arg);

/*
 * We'll use this struct to gather housekeeping hello_context to pass between
 * our events and callbacks.
 */
struct hello_context {
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *bdev_desc;
	struct spdk_io_channel *bdev_io_channel;
	char *buff;
	uint32_t buff_size;
	char *bdev_name;
	struct spdk_bdev_io_wait_entry bdev_io_wait;
        struct iovec *iovs;
};

/*
 * Usage function for printing parameters that are specific to this application
 */
static void
hello_bdev_usage(void)
{
	printf(" -b <bdev>                 name of the bdev to use\n");
}

/*
 * This function is called to parse the parameters that are specific to this application
 */
static int
hello_bdev_parse_arg(int ch, char *arg)
{
	switch (ch) {
	case 'b':
		g_bdev_name = arg;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void
hello_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		    void *event_ctx)
{
	SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
}

static void
reset_zone_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct hello_context *hello_context = cb_arg;

	/* Complete the I/O */
	spdk_bdev_free_io(bdev_io);

	if (!success) {
		SPDK_ERRLOG("bdev io reset zone error: %d\n", EIO);
		spdk_put_io_channel(hello_context->bdev_io_channel);
		spdk_bdev_close(hello_context->bdev_desc);
		spdk_app_stop(-1);
		return;
	}

	hello_kv_store(hello_context);
}

static void
hello_reset_zone(void *arg)
{
	struct hello_context *hello_context = arg;
	int rc = 0;

	rc = spdk_bdev_zone_management(hello_context->bdev_desc, hello_context->bdev_io_channel,
				       0, SPDK_BDEV_ZONE_RESET, reset_zone_complete, hello_context);

	if (rc == -ENOMEM) {
		SPDK_NOTICELOG("Queueing io\n");
		/* In case we cannot perform I/O now, queue I/O */
		hello_context->bdev_io_wait.bdev = hello_context->bdev;
		hello_context->bdev_io_wait.cb_fn = hello_reset_zone;
		hello_context->bdev_io_wait.cb_arg = hello_context;
		spdk_bdev_queue_io_wait(hello_context->bdev, hello_context->bdev_io_channel,
					&hello_context->bdev_io_wait);
	} else if (rc) {
		SPDK_ERRLOG("%s error while resetting zone: %d\n", spdk_strerror(-rc), rc);
		spdk_put_io_channel(hello_context->bdev_io_channel);
		spdk_bdev_close(hello_context->bdev_desc);
		spdk_app_stop(-1);
	}
}

/*
 * Our initial event that kicks off everything from main().
 */
static void
hello_start(void *arg1)
{
	struct hello_context *hello_context = arg1;
	uint32_t buf_align;
	int rc = 0;
	hello_context->bdev = NULL;
	hello_context->bdev_desc = NULL;

	SPDK_NOTICELOG("Successfully started the application\n");

	/*
	 * There can be many bdevs configured, but this application will only use
	 * the one input by the user at runtime.
	 *
	 * Open the bdev by calling spdk_bdev_open_ext() with its name.
	 * The function will return a descriptor
	 */
	SPDK_NOTICELOG("Opening the bdev %s\n", hello_context->bdev_name);
	rc = spdk_bdev_open_ext(hello_context->bdev_name, true, hello_bdev_event_cb, NULL,
				&hello_context->bdev_desc);
	if (rc) {
		SPDK_ERRLOG("Could not open bdev: %s\n", hello_context->bdev_name);
		spdk_app_stop(-1);
		return;
	}

	/* A bdev pointer is valid while the bdev is opened. */
	hello_context->bdev = spdk_bdev_desc_get_bdev(hello_context->bdev_desc);


	SPDK_NOTICELOG("Opening io channel\n");
	/* Open I/O channel */
	hello_context->bdev_io_channel = spdk_bdev_get_io_channel(hello_context->bdev_desc);
	if (hello_context->bdev_io_channel == NULL) {
		SPDK_ERRLOG("Could not create bdev I/O channel!!\n");
		spdk_bdev_close(hello_context->bdev_desc);
		spdk_app_stop(-1);
		return;
	}

	/* Allocate memory for the write buffer.
	 * Initialize the write buffer with the string "Hello World!"
	 */
	hello_context->buff_size = spdk_bdev_get_block_size(hello_context->bdev) *
				   spdk_bdev_get_write_unit_size(hello_context->bdev);
	buf_align = spdk_bdev_get_buf_align(hello_context->bdev);
	hello_context->buff = spdk_dma_zmalloc(hello_context->buff_size, buf_align, NULL);
	hello_context->iovs = malloc(sizeof(struct iovec) * 2);
	if (!hello_context->buff) {
		SPDK_ERRLOG("Failed to allocate buffer\n");
		spdk_put_io_channel(hello_context->bdev_io_channel);
		spdk_bdev_close(hello_context->bdev_desc);
		spdk_app_stop(-1);
		return;
	}
	snprintf(hello_context->buff, hello_context->buff_size, "%s", "a,b,c\n1,2,3\n");

	if (spdk_bdev_is_zoned(hello_context->bdev)) {
		hello_reset_zone(hello_context);
		/* If bdev is zoned, the callback, reset_zone_complete, will call hello_kv_store() */
		return;
	}

	hello_kv_store(hello_context);
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc = 0;
	struct hello_context hello_context = {};

	/* Set default values in opts structure. */
	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "hello_bdev";

	/*
	 * Parse built-in SPDK command line parameters as well
	 * as our custom one(s).
	 */
	if ((rc = spdk_app_parse_args(argc, argv, &opts, "b:", NULL, hello_bdev_parse_arg,
				      hello_bdev_usage)) != SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}
	hello_context.bdev_name = g_bdev_name;

	/*
	 * spdk_app_start() will initialize the SPDK framework, call hello_start(),
	 * and then block until spdk_app_stop() is called (or if an initialization
	 * error occurs, spdk_app_start() will return with rc even without calling
	 * hello_start().
	 */
	rc = spdk_app_start(&opts, hello_start, &hello_context);
	if (rc) {
		SPDK_ERRLOG("ERROR starting application\n");
	}

	/* At this point either spdk_app_stop() was called, or spdk_app_start()
	 * failed because of internal error.
	 */

	/* When the app stops, free up memory that we allocated. */
	spdk_dma_free(hello_context.buff);
        free(hello_context.iovs);

	/* Gracefully close out all of the SPDK subsystems. */
	spdk_app_fini();
	return rc;
}

static void
kv_exist_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg) {
        struct hello_context *hello_context = cb_arg;

        /* Complete the I/O */
        spdk_bdev_free_io(bdev_io);

        if (success) {
                SPDK_NOTICELOG("bdev kv exist completed successfully\n");
                uint32_t rc;
                int sct, sc;
                spdk_bdev_io_get_nvme_status(bdev_io, &rc, &sct, &sc);
                switch (sc) {
                        case SPDK_NVME_SC_SUCCESS:
                                SPDK_NOTICELOG("Key found\n");
                                break;
                        case SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST:
                                SPDK_NOTICELOG("Key not found\n");
                                break;
                        case SPDK_NVME_SC_INVALID_KEY_SIZE:
                                SPDK_NOTICELOG("Invalid key size\n");
                                break;
                }
        } else {
                SPDK_ERRLOG("bdev kv exist error: %d\n", EIO);
                spdk_put_io_channel(hello_context->bdev_io_channel);
                spdk_bdev_close(hello_context->bdev_desc);
                spdk_app_stop(-1);
                return;
        }
        hello_kv_send_select(hello_context);
}

static void
hello_kv_exist(void *arg) {
        struct hello_context *hello_context = arg;
        int rc = 0;

        SPDK_NOTICELOG("Calling kv exist\n");
        rc = spdk_bdev_kv_exist(hello_context->bdev_desc, hello_context->bdev_io_channel,
                             test_key, test_key_length,
                             kv_exist_complete, hello_context);

        if (rc == -ENOMEM) {
                SPDK_NOTICELOG("Queueing io\n");
                /* In case we cannot perform I/O now, queue I/O */
                hello_context->bdev_io_wait.bdev = hello_context->bdev;
                hello_context->bdev_io_wait.cb_fn = hello_kv_exist;
                hello_context->bdev_io_wait.cb_arg = hello_context;
                spdk_bdev_queue_io_wait(hello_context->bdev, hello_context->bdev_io_channel,
                                        &hello_context->bdev_io_wait);
        } else if (rc) {
                SPDK_ERRLOG("%s error while calling kv exist: %d\n", spdk_strerror(-rc), rc);
                spdk_put_io_channel(hello_context->bdev_io_channel);
                spdk_bdev_close(hello_context->bdev_desc);
                spdk_app_stop(-1);
        }

}

static void
kv_delete_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg) {
        struct hello_context *hello_context = cb_arg;

        if (success) {
                SPDK_NOTICELOG("bdev kv delete completed successfully\n");
        } else {
                SPDK_ERRLOG("bdev kv delete error\n");
        }

        /* Complete the bdev io and close the channel */
        spdk_bdev_free_io(bdev_io);
        spdk_put_io_channel(hello_context->bdev_io_channel);
        spdk_bdev_close(hello_context->bdev_desc);
        SPDK_NOTICELOG("Stopping app\n");
        spdk_app_stop(success ? 0 : -1);
}

static void
hello_kv_delete(void *arg) {
        struct hello_context *hello_context = arg;
        int rc = 0;

        SPDK_NOTICELOG("Calling kv delete\n");
        rc = spdk_bdev_kv_delete(hello_context->bdev_desc, hello_context->bdev_io_channel,
                             test_key, test_key_length,
                             kv_delete_complete, hello_context);

        if (rc == -ENOMEM) {
                SPDK_NOTICELOG("Queueing io\n");
                /* In case we cannot perform I/O now, queue I/O */
                hello_context->bdev_io_wait.bdev = hello_context->bdev;
                hello_context->bdev_io_wait.cb_fn = hello_kv_delete;
                hello_context->bdev_io_wait.cb_arg = hello_context;
                spdk_bdev_queue_io_wait(hello_context->bdev, hello_context->bdev_io_channel,
                                        &hello_context->bdev_io_wait);
        } else if (rc) {
                SPDK_ERRLOG("%s error while calling kv delete: %d\n", spdk_strerror(-rc), rc);
                spdk_put_io_channel(hello_context->bdev_io_channel);
                spdk_bdev_close(hello_context->bdev_desc);
                spdk_app_stop(-1);
        }

}

static inline int
read_key_from_buffer(void *buffer, size_t buffer_size) {
	uint32_t num_keys;
	size_t bytes_read;
	uint16_t len, pad_len;

	num_keys = *(uint32_t *) buffer;
        SPDK_NOTICELOG("num keys in the buffer: %d\n", num_keys);

	bytes_read = 4;
	void *ptr;

	for  (uint32_t i = 0; i < num_keys; ++i) {
		ptr = buffer + bytes_read;
		len = *(uint16_t*) ptr;

		pad_len = len;
		if (pad_len % 4) {
			pad_len += 4 - (pad_len % 4);
		}

                SPDK_NOTICELOG("key[%d] = %.*s\n", i, len, (char*)(ptr + 2));

		if (bytes_read + pad_len + 2 > buffer_size) {
			printf("ERROR: buffer overflow when reading keys\n");
			return -1;
		}

		bytes_read += pad_len + 2;
	}
	return 0;
}

static void
kv_list_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg) {
        struct hello_context *hello_context = cb_arg;

        /* Complete the I/O */
        spdk_bdev_free_io(bdev_io);

        if (success) {
                int rc, sct, sc;
                uint32_t num_keys;
                spdk_bdev_io_get_nvme_status(bdev_io, &num_keys, &sct, &sc);
                SPDK_NOTICELOG("total num keys: %d\n", num_keys);
                rc = read_key_from_buffer(hello_context->buff, hello_context->buff_size);
                if (rc != 0) {
                        printf("Error reading keys from buffer\n");
                        spdk_put_io_channel(hello_context->bdev_io_channel);
                        spdk_bdev_close(hello_context->bdev_desc);
                        spdk_app_stop(-1);
                        return;
                }
        } else {
                SPDK_ERRLOG("bdev kv list error: %d\n", EIO);
                spdk_put_io_channel(hello_context->bdev_io_channel);
                spdk_bdev_close(hello_context->bdev_desc);
                spdk_app_stop(-1);
                return;
        }
        memset(hello_context->buff, 0, hello_context->buff_size);
        hello_kv_exist(hello_context);
}

static void
hello_kv_list(void *arg) {
        struct hello_context *hello_context = arg;
        int rc = 0;

        hello_context->iovs[0].iov_base = hello_context->buff;
        hello_context->iovs[0].iov_len = 6;
        hello_context->iovs[1].iov_base = hello_context->buff + 6;
        hello_context->iovs[1].iov_len = hello_context->buff_size - 6;

        SPDK_NOTICELOG("Calling kv list\n");
        rc = spdk_bdev_kv_listv(hello_context->bdev_desc, hello_context->bdev_io_channel,
                             "", 0,
			     hello_context->iovs, 2, hello_context->buff_size,
                             kv_list_complete, hello_context);

        if (rc == -ENOMEM) {
                SPDK_NOTICELOG("Queueing io\n");
                /* In case we cannot perform I/O now, queue I/O */
                hello_context->bdev_io_wait.bdev = hello_context->bdev;
                hello_context->bdev_io_wait.cb_fn = hello_kv_list;
                hello_context->bdev_io_wait.cb_arg = hello_context;
                spdk_bdev_queue_io_wait(hello_context->bdev, hello_context->bdev_io_channel,
                                        &hello_context->bdev_io_wait);
        } else if (rc) {
                SPDK_ERRLOG("%s error while calling kv list: %d\n", spdk_strerror(-rc), rc);
                spdk_put_io_channel(hello_context->bdev_io_channel);
                spdk_bdev_close(hello_context->bdev_desc);
                spdk_app_stop(-1);
        }

}

static void
kv_send_select_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg) {
        struct hello_context *hello_context = cb_arg;
        int sct, sc;

        /* Complete the I/O */
        spdk_bdev_free_io(bdev_io);

        if (success) {
                spdk_bdev_io_get_nvme_status(bdev_io, &select_id, &sct, &sc);
                SPDK_NOTICELOG("bdev kv send select completed successfully with select id %d\n", select_id);
        } else {
                SPDK_ERRLOG("bdev kv send select error: %d\n", EIO);
                spdk_put_io_channel(hello_context->bdev_io_channel);
                spdk_bdev_close(hello_context->bdev_desc);
                spdk_app_stop(-1);
                return;
        }

        
        memset(hello_context->buff, 0, hello_context->buff_size);
        hello_kv_retrieve_select(hello_context);
}

static void
hello_kv_send_select(void *arg) {
        struct hello_context *hello_context = arg;
        int rc = 0;
	snprintf(hello_context->buff, hello_context->buff_size, "%s", "select * from x where a=1");

        hello_context->iovs[0].iov_base = hello_context->buff;
        hello_context->iovs[0].iov_len = 6;
        hello_context->iovs[1].iov_base = hello_context->buff + 6;
        hello_context->iovs[1].iov_len = strlen(hello_context->buff) + 1 - 6;

        SPDK_NOTICELOG("Calling kv send select\n");
        rc = spdk_bdev_kv_send_selectv(hello_context->bdev_desc, hello_context->bdev_io_channel,
                             test_key, test_key_length,
			     hello_context->iovs, 2, strlen(hello_context->buff),
                             NVME_KV_SELECT_CMD_OUTPUT_TYPE_USE_CSV_HEADERS_INPUT,
                             NVME_KV_SELECT_TYPE_CSV, NVME_KV_SELECT_TYPE_CSV,
                             kv_send_select_complete, hello_context);

        if (rc == -ENOMEM) {
                SPDK_NOTICELOG("Queueing io\n");
                /* In case we cannot perform I/O now, queue I/O */
                hello_context->bdev_io_wait.bdev = hello_context->bdev;
                hello_context->bdev_io_wait.cb_fn = hello_kv_send_select;
                hello_context->bdev_io_wait.cb_arg = hello_context;
                spdk_bdev_queue_io_wait(hello_context->bdev, hello_context->bdev_io_channel,
                                        &hello_context->bdev_io_wait);
        } else if (rc) {
                SPDK_ERRLOG("%s error while writing to bdev: %d\n", spdk_strerror(-rc), rc);
                spdk_put_io_channel(hello_context->bdev_io_channel);
                spdk_bdev_close(hello_context->bdev_desc);
                spdk_app_stop(-1);
        }
}

static void
kv_retrieve_select_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg) {
        struct hello_context *hello_context = cb_arg;

        /* Complete the I/O */
        spdk_bdev_free_io(bdev_io);

        if (success) {
                SPDK_NOTICELOG("bdev kv retrieve select returned: %s\n", hello_context->buff);
        } else {
                SPDK_ERRLOG("bdev kv retrieve select error: %d\n", EIO);
                spdk_put_io_channel(hello_context->bdev_io_channel);
                spdk_bdev_close(hello_context->bdev_desc);
                spdk_app_stop(-1);
                return;
        }
        memset(hello_context->buff, 0, hello_context->buff_size);
        hello_kv_delete(hello_context);
}

static void
hello_kv_retrieve_select(void *arg) {
        struct hello_context *hello_context = arg;
        int rc = 0;

        hello_context->iovs[0].iov_base = hello_context->buff;
        hello_context->iovs[0].iov_len = 3;
        hello_context->iovs[1].iov_base = hello_context->buff + 3;
        hello_context->iovs[1].iov_len = hello_context->buff_size - 3;

        SPDK_NOTICELOG("Calling kv retrieve select\n");
        rc = spdk_bdev_kv_retrieve_selectv(hello_context->bdev_desc, hello_context->bdev_io_channel,
                             hello_context->iovs, 2, 0, hello_context->buff_size,
                             select_id, 0,
                             kv_retrieve_select_complete, hello_context);

        if (rc == -ENOMEM) {
                SPDK_NOTICELOG("Queueing io\n");
                /* In case we cannot perform I/O now, queue I/O */
                hello_context->bdev_io_wait.bdev = hello_context->bdev;
                hello_context->bdev_io_wait.cb_fn = hello_kv_retrieve_select;
                hello_context->bdev_io_wait.cb_arg = hello_context;
                spdk_bdev_queue_io_wait(hello_context->bdev, hello_context->bdev_io_channel,
                                        &hello_context->bdev_io_wait);
        } else if (rc) {
                SPDK_ERRLOG("%s error while writing to bdev: %d\n", spdk_strerror(-rc), rc);
                spdk_put_io_channel(hello_context->bdev_io_channel);
                spdk_bdev_close(hello_context->bdev_desc);
                spdk_app_stop(-1);
        }

}

static void
kv_retrieve_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
        struct hello_context *hello_context = cb_arg;

        /* Complete the I/O */
        spdk_bdev_free_io(bdev_io);

        if (success) {
                SPDK_NOTICELOG("bdev kv retrieve returned: %s", hello_context->buff);
        } else {
                SPDK_ERRLOG("bdev kv retrieve error: %d\n", EIO);
                spdk_put_io_channel(hello_context->bdev_io_channel);
                spdk_bdev_close(hello_context->bdev_desc);
                spdk_app_stop(-1);
                return;
        }
        memset(hello_context->buff, 0, hello_context->buff_size);
        hello_kv_list(hello_context);
}

static void
hello_kv_retrieve(void *arg)
{
        struct hello_context *hello_context = arg;
        int rc = 0;

        hello_context->iovs[0].iov_base = hello_context->buff;
        hello_context->iovs[0].iov_len = 6;
        hello_context->iovs[1].iov_base = hello_context->buff + 6;
        hello_context->iovs[1].iov_len = hello_context->buff_size - 6;

        SPDK_NOTICELOG("Calling kv retrieve\n");
        rc = spdk_bdev_kv_retrievev(hello_context->bdev_desc, hello_context->bdev_io_channel,
                            test_key, test_key_length,
                            hello_context->iovs, 2, 0, hello_context->buff_size, kv_retrieve_complete,
                            hello_context);

        if (rc == -ENOMEM) {
                SPDK_NOTICELOG("Queueing io\n");
                /* In case we cannot perform I/O now, queue I/O */
                hello_context->bdev_io_wait.bdev = hello_context->bdev;
                hello_context->bdev_io_wait.cb_fn = hello_kv_retrieve;
                hello_context->bdev_io_wait.cb_arg = hello_context;
                spdk_bdev_queue_io_wait(hello_context->bdev, hello_context->bdev_io_channel,
                                        &hello_context->bdev_io_wait);
        } else if (rc) {
                SPDK_ERRLOG("%s error while reading from bdev: %d\n", spdk_strerror(-rc), rc);
                spdk_put_io_channel(hello_context->bdev_io_channel);
                spdk_bdev_close(hello_context->bdev_desc);
                spdk_app_stop(-1);
        }
}

static void
kv_store_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
        struct hello_context *hello_context = cb_arg;

        /* Complete the I/O */
        spdk_bdev_free_io(bdev_io);

        if (success) {
                SPDK_NOTICELOG("bdev kv store completed successfully\n");
        } else {
                SPDK_ERRLOG("bdev kv store error: %d\n", EIO);
                spdk_put_io_channel(hello_context->bdev_io_channel);
                spdk_bdev_close(hello_context->bdev_desc);
                spdk_app_stop(-1);
                return;
        }
        memset(hello_context->buff, 0, hello_context->buff_size);
        hello_kv_retrieve(hello_context);
}

static void
hello_kv_store(void *arg)
{
        struct hello_context *hello_context = arg;
        int rc = 0;

        hello_context->iovs[0].iov_base = hello_context->buff;
        hello_context->iovs[0].iov_len = 6;
        hello_context->iovs[1].iov_base = hello_context->buff + 6;
        hello_context->iovs[1].iov_len = strlen(hello_context->buff) - 6;

        SPDK_NOTICELOG("Calling kv store\n");
        rc = spdk_bdev_kv_storev(hello_context->bdev_desc, hello_context->bdev_io_channel,
                             test_key, test_key_length,
                             hello_context->iovs, 2, strlen(hello_context->buff), 0, kv_store_complete,
                             hello_context);

        if (rc == -ENOMEM) {
                SPDK_NOTICELOG("Queueing io\n");
                /* In case we cannot perform I/O now, queue I/O */
                hello_context->bdev_io_wait.bdev = hello_context->bdev;
                hello_context->bdev_io_wait.cb_fn = hello_kv_store;
                hello_context->bdev_io_wait.cb_arg = hello_context;
                spdk_bdev_queue_io_wait(hello_context->bdev, hello_context->bdev_io_channel,
                                        &hello_context->bdev_io_wait);
        } else if (rc) {
                SPDK_ERRLOG("%s error while writing to bdev: %d\n", spdk_strerror(-rc), rc);
                spdk_put_io_channel(hello_context->bdev_io_channel);
                spdk_bdev_close(hello_context->bdev_desc);
                spdk_app_stop(-1);
        }
}
