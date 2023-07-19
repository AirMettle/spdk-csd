/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *   Copyright (C) 2023 AirMettle, Inc. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev.h"

#include "spdk/config.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/queue.h"
#include "spdk/nvme_spec.h"
#include "spdk/scsi_spec.h"
#include "spdk/notify.h"
#include "spdk/util.h"
#include "spdk/trace.h"
#include "spdk/dma.h"

#include "spdk/bdev_module.h"
#include "spdk/log.h"
#include "spdk/string.h"

#include "bdev_internal.h"
#include "spdk_internal/trace_defs.h"

#define __io_ch_to_bdev_ch(io_ch)       ((struct spdk_bdev_channel *)spdk_io_channel_get_ctx(io_ch))

int spdk_bdev_kv_list(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
                   unsigned char *key, size_t key_length,
                   void *buf, uint64_t nbytes,
                   spdk_bdev_io_completion_cb cb, void *cb_arg) {
    if (key_length >= NVME_KV_MAX_KEY_LENGTH) {
        return -EINVAL;
    }

    struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
    struct spdk_bdev_io *bdev_io;
    struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);
   
    if (!spdk_bdev_check_desc_write(desc)) {
        return -EBADF;
    }

    bdev_io = bdev_channel_get_io(channel);
    if (!bdev_io) {
        return -ENOMEM;
    }
                 
    bdev_io->internal.ch = channel;
    bdev_io->internal.desc = desc;
    bdev_io->type = SPDK_BDEV_IO_KV_LIST;
    bdev_io->u.bdev.iovs = &bdev_io->iov;
    bdev_io->u.bdev.iovs[0].iov_base = buf;
    bdev_io->u.bdev.iovs[0].iov_len = nbytes;
    bdev_io->u.bdev.iovcnt = 1;
    bdev_io->u.bdev.ext_opts = NULL;
    memcpy(bdev_io->u.nvme_kv.key, key, key_length);
    bdev_io->u.nvme_kv.key_length = key_length;
    bdev_io_init(bdev_io, bdev, cb_arg, cb);
    bdev_io_submit(bdev_io);
    return 0;        
}

int spdk_bdev_kv_delete(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
                   unsigned char *key, size_t key_length,
                   spdk_bdev_io_completion_cb cb, void *cb_arg) {
    if (key_length == 0 || key_length >= NVME_KV_MAX_KEY_LENGTH) {
        return -EINVAL;
    }
    struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
    struct spdk_bdev_io *bdev_io;
    struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);
        
    if (!spdk_bdev_check_desc_write(desc)) {
        return -EBADF;
    }
    
    bdev_io = bdev_channel_get_io(channel);
    if (!bdev_io) {
        return -ENOMEM;
    }
    
    bdev_io->internal.ch = channel;
    bdev_io->internal.desc = desc;
    bdev_io->type = SPDK_BDEV_IO_KV_DELETE;
    bdev_io->u.bdev.iovcnt = 0;
    bdev_io->u.bdev.ext_opts = NULL;
    memcpy(bdev_io->u.nvme_kv.key, key, key_length);
    bdev_io->u.nvme_kv.key_length = key_length;
    bdev_io_init(bdev_io, bdev, cb_arg, cb);
    bdev_io_submit(bdev_io);
    return 0;
}

int spdk_bdev_kv_exist(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
                   unsigned char *key, size_t key_length,
                   spdk_bdev_io_completion_cb cb, void *cb_arg) {
    if (key_length == 0 || key_length >= NVME_KV_MAX_KEY_LENGTH) {
        return -EINVAL;
    }
    struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
    struct spdk_bdev_io *bdev_io;
    struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);


    bdev_io = bdev_channel_get_io(channel);
    if (!bdev_io) {
        return -ENOMEM;
    }

    bdev_io->internal.ch = channel;
    bdev_io->internal.desc = desc;
    bdev_io->type = SPDK_BDEV_IO_KV_EXIST;
    bdev_io->u.bdev.iovcnt = 0;
    bdev_io->u.bdev.ext_opts = NULL;
    memcpy(bdev_io->u.nvme_kv.key, key, key_length);
    bdev_io->u.nvme_kv.key_length = key_length;
    bdev_io_init(bdev_io, bdev, cb_arg, cb);
    bdev_io_submit(bdev_io);
    return 0;
}

int spdk_bdev_kv_store(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
                   unsigned char *key, size_t key_length,
                   void *buf, uint64_t nbytes, uint8_t options,
                   spdk_bdev_io_completion_cb cb, void *cb_arg) {
    if (key_length == 0 || key_length >= NVME_KV_MAX_KEY_LENGTH) {
        return -EINVAL;
    }
    struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
    struct spdk_bdev_io *bdev_io;
    struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

    if (!spdk_bdev_check_desc_write(desc)) {
        return -EBADF;
    }

    bdev_io = bdev_channel_get_io(channel);
    if (!bdev_io) {
        return -ENOMEM;
    }

    bdev_io->internal.ch = channel;
    bdev_io->internal.desc = desc;
    bdev_io->type = SPDK_BDEV_IO_KV_STORE;
    bdev_io->u.bdev.iovs = &bdev_io->iov;
    bdev_io->u.bdev.iovs[0].iov_base = buf;
    bdev_io->u.bdev.iovs[0].iov_len = nbytes;
    bdev_io->u.bdev.iovcnt = 1;
    bdev_io->u.bdev.ext_opts = NULL;
    memcpy(bdev_io->u.nvme_kv.key, key, key_length);
    bdev_io->u.nvme_kv.key_length = key_length;
    bdev_io->u.nvme_kv.options = options;
    bdev_io_init(bdev_io, bdev, cb_arg, cb);
    bdev_io_submit(bdev_io);
    return 0;
}

int spdk_bdev_kv_retrieve(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
                   unsigned char *key, size_t key_length,
                   void *buf, uint64_t offset, uint64_t nbytes,
                   spdk_bdev_io_completion_cb cb, void *cb_arg) {
    if (key_length == 0 || key_length >= NVME_KV_MAX_KEY_LENGTH) {
        return -EINVAL;
    }
    struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
    struct spdk_bdev_io *bdev_io;
    struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);
    
    bdev_io = bdev_channel_get_io(channel);
    if (!bdev_io) {
        return -ENOMEM;
    }
    
    bdev_io->internal.ch = channel;
    bdev_io->internal.desc = desc;
    bdev_io->type = SPDK_BDEV_IO_KV_RETRIEVE;
    bdev_io->u.bdev.iovs = &bdev_io->iov;
    bdev_io->u.bdev.iovs[0].iov_base = buf;
    bdev_io->u.bdev.iovs[0].iov_len = nbytes;
    bdev_io->u.bdev.iovcnt = 1;
    bdev_io->u.bdev.ext_opts = NULL; 
    memcpy(bdev_io->u.nvme_kv.key, key, key_length);
    bdev_io->u.nvme_kv.key_length = key_length;
    bdev_io->u.nvme_kv.offset = offset;
    bdev_io_init(bdev_io, bdev, cb_arg, cb); 
    bdev_io_submit(bdev_io);
    return 0;
}

int spdk_bdev_kv_send_select(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
                   unsigned char *key, size_t key_length,
                   void *buf, uint64_t nbytes, uint8_t options,
                   uint8_t input_type, uint8_t output_type,
                   spdk_bdev_io_completion_cb cb, void *cb_arg) {
    if (key_length == 0 || key_length >= NVME_KV_MAX_KEY_LENGTH) {
        return -EINVAL;
    }
    struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
    struct spdk_bdev_io *bdev_io;
    struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

    if (key_length >= NVME_KV_MAX_KEY_LENGTH) {
        return -EINVAL;
    }

    if (!spdk_bdev_check_desc_write(desc)) {
        return -EBADF;
    }

    bdev_io = bdev_channel_get_io(channel);
    if (!bdev_io) {
        return -ENOMEM;
    }

    bdev_io->internal.ch = channel;
    bdev_io->internal.desc = desc;    
    bdev_io->type = SPDK_BDEV_IO_KV_SEND_SELECT;
    bdev_io->u.bdev.iovs = &bdev_io->iov;
    bdev_io->u.bdev.iovs[0].iov_base = buf;
    bdev_io->u.bdev.iovs[0].iov_len = nbytes;
    bdev_io->u.bdev.iovcnt = 1;
    bdev_io->u.bdev.ext_opts = NULL;
    memcpy(bdev_io->u.nvme_kv.key, key, key_length);
    bdev_io->u.nvme_kv.key_length = key_length;
    bdev_io->u.nvme_kv.options = options;
    bdev_io->u.nvme_kv.select_input_type = input_type;
    bdev_io->u.nvme_kv.select_output_type = output_type;
    bdev_io_init(bdev_io, bdev, cb_arg, cb);
    bdev_io_submit(bdev_io);
    return 0;
}
 
int spdk_bdev_kv_retrieve_select(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
                   void *buf, uint64_t offset, uint64_t nbytes,
                   uint32_t select_id, uint8_t options,
                   spdk_bdev_io_completion_cb cb, void *cb_arg) {
    struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
    struct spdk_bdev_io *bdev_io;
    struct spdk_bdev_channel *channel = __io_ch_to_bdev_ch(ch);

    bdev_io = bdev_channel_get_io(channel);
    if (!bdev_io) {
        return -ENOMEM;
    }
 
    bdev_io->internal.ch = channel;
    bdev_io->internal.desc = desc;
    bdev_io->type = SPDK_BDEV_IO_KV_RETRIEVE_SELECT;
    bdev_io->u.bdev.iovs = &bdev_io->iov;
    bdev_io->u.bdev.iovs[0].iov_base = buf; 
    bdev_io->u.bdev.iovs[0].iov_len = nbytes;
    bdev_io->u.bdev.iovcnt = 1;
    bdev_io->u.bdev.ext_opts = NULL;
    bdev_io->u.nvme_kv.offset = offset;
    bdev_io->u.nvme_kv.options = options;
    bdev_io->u.nvme_kv.select_id = select_id;
    bdev_io_init(bdev_io, bdev, cb_arg, cb);
    bdev_io_submit(bdev_io);
    return 0;
}
