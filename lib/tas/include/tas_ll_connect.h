/*
 * Copyright 2019 University of Washington, Max Planck Institute for
 * Software Systems, and The University of Texas at Austin
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef FLEXNIC_DRIVER_H_
#define FLEXNIC_DRIVER_H_

#include <stddef.h>
#include <tas_memif.h>
#include <rdma.h>

/**
 * Connect to flexnic. Returns 0 on success, < 0 on error, > 0 if flexnic is not
 * ready yet.
 */
int flexnic_driver_connect(struct flexnic_info **info, void **mem_start);

/** Connect to flexnic internal memory. */
int flexnic_driver_internal(void **int_mem_start);

#define RAPPC_APP_ACKED (1 << 0)
#define RAPPC_APP_ERR (1 << 1)
#define RAPPC_CTX_ACKED (1 << 2)
struct rdma_app_context {
    uint64_t flags;
    struct flextcp_connection* conn;
    struct rdma_ack_app_register rapp_info;
    struct rdma_ack_ctx_register rctx_info;
};

#endif /* ndef FLEXNIC_DRIVER_H_ */
