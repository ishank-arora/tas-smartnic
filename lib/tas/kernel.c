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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <kernel_appif.h>
#include <utils_timeout.h>
#include "internal.h"
#include <tas_ll_connect.h>
#include <rdma_queue.h>
#include <rdma.h>

#define NIC_RXQ_LEN (64 * 32 * 1024)
#define NIC_TXQ_LEN (64 * 8192)

static int ksock_fd = -1;
static int kernel_evfd = 0;

void flextcp_kernel_kick(void)
{
  static uint64_t __thread last_ts = 0;
  uint64_t now = util_rdtsc();

  /* fprintf(stderr, "kicking kernel?\n"); */

  if(now - last_ts > tas_info->poll_cycle_tas) {
    // Kick kernel
    /* fprintf(stderr, "kicking kernel\n"); */
    assert(kernel_evfd != 0);
    uint64_t val = 1;
    int r = write(kernel_evfd, &val, sizeof(uint64_t));
    assert(r == sizeof(uint64_t));
  }

  last_ts = now;
}

int flextcp_kernel_connect(void)
{
  int fd, *pfd;
  uint8_t b;
  ssize_t r;
  uint32_t num_fds, off, i, n;
  struct sockaddr_un saun;
  struct cmsghdr *cmsg;

  /* prepare socket address */
  memset(&saun, 0, sizeof(saun));
  saun.sun_family = AF_UNIX;
  memcpy(saun.sun_path, KERNEL_SOCKET_PATH, sizeof(KERNEL_SOCKET_PATH));

  if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) == -1) {
    perror("flextcp_kernel_connect: socket failed");
    return -1;
  }

  if (connect(fd, (struct sockaddr *) &saun, sizeof(saun)) != 0) {
    perror("flextcp_kernel_connect: connect failed");
    return -1;
  }

  struct iovec iov = {
    .iov_base = &num_fds,
    .iov_len = sizeof(uint32_t),
  };
  union {
    char buf[CMSG_SPACE(sizeof(int) * 4)];
    struct cmsghdr align;
  } u;
  struct msghdr msg = {
    .msg_name = NULL,
    .msg_namelen = 0,
    .msg_iov = &iov,
    .msg_iovlen = 1,
    .msg_control = u.buf,
    .msg_controllen = sizeof(u.buf),
    .msg_flags = 0,
  };

  /* receive welcome message:
  *   contains the fd for the kernel, and the count of flexnic fds */
  if ((r = recvmsg(fd, &msg, 0)) != sizeof(uint32_t)) {
    fprintf(stderr, "flextcp_kernel_connect: recvmsg failed (bytes sent: %zd error: %s)\n", 
      r, strerror(errno));
    abort();
  }

  /* get kernel fd from welcome message */
  cmsg = CMSG_FIRSTHDR(&msg);
  pfd = (int *) CMSG_DATA(cmsg);
  if (msg.msg_controllen <= 0 || cmsg->cmsg_len != CMSG_LEN(sizeof(int))) {
    fprintf(stderr, "flextcp_kernel_connect: accessing ancillary data "
        "failed\n");
    abort();
  }
  kernel_evfd = *pfd;

  /* receive fast path fds in batches of 4 */
  off = 0;
  for (off = 0 ; off < num_fds; ) {
    iov.iov_base = &b;
    iov.iov_len = 1;

    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = u.buf;
    msg.msg_controllen = sizeof(u);

    /* receive fd message (up to 4 fds at once) */
    if ((r = recvmsg(fd, &msg, 0)) != 1) {
      fprintf(stderr, "flextcp_kernel_connect: recvmsg fd failed (%zd)\n", r);
      abort();
    }

    n = (num_fds - off >= 4 ? 4 : num_fds - off);

    /* get kernel fd from welcome message */
    cmsg = CMSG_FIRSTHDR(&msg);
    pfd = (int *) CMSG_DATA(cmsg);
    if (msg.msg_controllen <= 0 || cmsg->cmsg_len != CMSG_LEN(sizeof(int) * n)) {
      fprintf(stderr, "flextcp_kernel_connect: accessing ancillary data fds "
          "failed\n");
      abort();
    }

    for (i = 0; i < n; i++) {
      flexnic_evfd[off++] = pfd[i];
    }
  }
  ksock_fd = fd;
  if (rdmafd != -1) {
    while (nic_conn == NULL || nic_conn->context == NULL) {
      if (on_rdmaif_event() != 0) {
        fprintf(stderr, "flextcp_kernel_connect: failed to handle event\n");
        abort();
      }
    }

    struct rdma_app_context* ctx = nic_conn->context;
    while ((ctx->flags & RAPPC_APP_ACKED) == 0 && 
          (ctx->flags & RAPPC_APP_ERR) == 0) 
    {
      if (on_rdmaif_event() != 0) {
        fprintf(stderr, "flextcp_kernel_connect: failed to handle event\n");
        abort();
      }
    }

    if ((ctx->flags & RAPPC_APP_ERR) != 0) {
      fprintf(stderr, "flextcp_kernel_connect: failed to connect\n");
      abort();
    }
  }
  return 0;
}

int flextcp_kernel_newctx(struct flextcp_context *ctx)
{
  struct kernel_uxsock_request req = {
        .rxq_len = NIC_RXQ_LEN,
        .txq_len = NIC_TXQ_LEN,
      };
  /* send request on kernel socket */
  struct iovec iov;
  memset(&iov, 0, sizeof(iov));
  iov.iov_base = &req;
  iov.iov_len = sizeof(req);
  union {
    char buf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr align;
  } u;
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_name = NULL;
  msg.msg_namelen = 0;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = &u.buf;
  msg.msg_controllen = sizeof(u.buf);
  msg.msg_flags = 0;

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  int *myfd = (int *)CMSG_DATA(cmsg);
  *myfd = ctx->evfd;

  ssize_t sz = sendmsg(ksock_fd, &msg, 0);
  if (sz != sizeof(req)) {
    fprintf(stderr, "flextcp_kernel_newctx: failed to send %ld bytes "
      "(fd: %d, error: %s)\n", sz, ksock_fd, strerror(errno));
    abort();
  }
  
  if (rdmafd == -1) {
    ssize_t off, total_sz;
    struct kernel_uxsock_response *resp;
    uint8_t resp_buf[sizeof(*resp) +
        FLEXTCP_MAX_FTCPCORES * sizeof(resp->flexnic_qs[0])];
    uint16_t i;

    /* receive response on kernel socket */
    resp = (struct kernel_uxsock_response *) resp_buf;
    off = 0;
    while (off < sizeof(*resp)) {
      sz = read(ksock_fd, (uint8_t *) resp + off, sizeof(*resp) - off);
      if (sz < 0) {
        perror("flextcp_kernel_newctx: read failed");
        return -1;
      }
      off += sz;
    }

    if (resp->flexnic_qs_num > FLEXTCP_MAX_FTCPCORES) {
      fprintf(stderr, "flextcp_kernel_newctx: stack only supports up to %u "
          "queues, got %u\n", FLEXTCP_MAX_FTCPCORES, resp->flexnic_qs_num);
      abort();
    }
    /* receive queues in response */
    total_sz = sizeof(*resp) + resp->flexnic_qs_num * sizeof(resp->flexnic_qs[0]);
    while (off < total_sz) {
      sz = read(ksock_fd, (uint8_t *) resp + off, total_sz - off);
      if (sz < 0) {
        perror("flextcp_kernel_newctx: read failed");
        return -1;
      }
      off += sz;
    }

    if (resp->status != 0) {
      fprintf(stderr, "flextcp_kernel_newctx: request failed\n");
      return -1;
    }

    /* fill in ctx struct */
    ctx->kin = (struct rdma_queue*) ((uint8_t *) flexnic_mem + resp->app_out_off);
    ctx->kout = (struct rdma_queue*) ((uint8_t *) flexnic_mem + resp->app_in_off);
    
    ctx->db_id = resp->flexnic_db_id;
    ctx->num_queues = resp->flexnic_qs_num;
    ctx->next_queue = 0;

    for (i = 0; i < resp->flexnic_qs_num; i++) {
      // TODO: allocate own buffers when using RDMA
      ctx->queues[i].rx =
        (struct rdma_queue*) ((uint8_t *) flexnic_mem + resp->flexnic_qs[i].rxq_off);
      ctx->queues[i].tx =
        (struct rdma_queue*) ((uint8_t *) flexnic_mem + resp->flexnic_qs[i].txq_off);
      ctx->queues[i].last_ts = 0;
    }
  } else {
    int host_ctx_fd;
    if (read(ksock_fd, &host_ctx_fd, sizeof(host_ctx_fd)) != sizeof(host_ctx_fd)) {
      fprintf(stderr, "flextcp_kernel_newctx: failed to receive host fd\n");
      abort();
    }

    /* allocate memory queues */
    uint64_t kin_qsize = tas_info->kin_len + sizeof(struct rdma_queue);
    uint64_t kout_qsize = tas_info->kout_len + sizeof(struct rdma_queue);
    uint64_t nic_rx_len = NIC_RXQ_LEN + sizeof(struct rdma_queue);
    uint64_t nic_tx_len = NIC_TXQ_LEN + sizeof(struct rdma_queue);
    uint64_t bytes_needed = kin_qsize + kout_qsize + (nic_rx_len + nic_tx_len) * tas_info->cores_num;
    void* ctx_buffer = malloc(bytes_needed);
    /* register ctx buffer */
    struct ibv_mr* ctx_buffer_mr; // TODO: store this somewhere
    if ((ctx_buffer_mr = ibv_reg_mr(
        nic_ctx->prot_domain,
        ctx_buffer,
        bytes_needed,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ
    )) == 0) {
        fprintf(stderr, "flextcp_kernel_newctx: failed to register ctx buffer\n");
        return -1;
    }
    /* allocate buffers */
    int offset = 0;
    uintptr_t ctx_buffer_opaque = (uintptr_t) ctx_buffer;
    ctx->kin = rq_allocate_in_place((void*) (ctx_buffer_opaque + offset), kin_qsize, nic_conn->qp, ctx_buffer_mr->lkey, ctx_buffer_mr->rkey);
    offset += kin_qsize;
    ctx->kout = rq_allocate_in_place((void*) (ctx_buffer_opaque + offset), kout_qsize, nic_conn->qp, ctx_buffer_mr->lkey, ctx_buffer_mr->rkey);
    offset += kout_qsize;
    for (int i = 0; i < tas_info->cores_num; i++) {
      ctx->queues[i].rx = rq_allocate_in_place((void*) (ctx_buffer_opaque + offset), nic_rx_len, nic_conn->qp, ctx_buffer_mr->lkey, ctx_buffer_mr->rkey);
      offset += nic_rx_len;
      ctx->queues[i].tx = rq_allocate_in_place((void*) (ctx_buffer_opaque + offset), nic_tx_len, nic_conn->qp, ctx_buffer_mr->lkey, ctx_buffer_mr->rkey);
      offset += nic_tx_len;
    }
    assert(offset == bytes_needed);
    MEM_BARRIER();
    /* send buffers to nic */
    struct rdma_msg msg;
    struct rdma_register_ctx ctx_reg;
    msg.type = RDMA_MSG_REGISTER_CTX;
    ctx_reg.lkey = ctx_buffer_mr->lkey;
    ctx_reg.rkey = ctx_buffer_mr->rkey;
    ctx_reg.memq_base = (uintptr_t) ctx_buffer;
    ctx_reg.rx_len = NIC_RXQ_LEN;
    ctx_reg.tx_len = NIC_TXQ_LEN;
    ctx_reg.ctx_evfd = host_ctx_fd;
    msg.data.ctx_reg = ctx_reg;

    if (rdma_send_msg(nic_conn, msg) != 0) {
      fprintf(stderr, "flextcp_kernel_newctx: failed to send buffers to nic\n");
      return -1;
    }

    struct rdma_app_context* nic_app_ctx = nic_conn->context;
    while ((nic_app_ctx->flags & RAPPC_CTX_ACKED) == 0) {
      if (on_rdmaif_event() != 0) {
        fprintf(stderr, "flextcp_kernel_newctx: failed to handle event\n");
        return -1;
      }
    }
    nic_app_ctx->flags &= ~RAPPC_CTX_ACKED;
    MEM_BARRIER();

    ctx->db_id = nic_app_ctx->rctx_info.flexnic_db_id;
    ctx->num_queues = nic_app_ctx->rctx_info.flexnic_qs_num;
    ctx->next_queue = 0;

    /* pair txs with rxs */
    offset = 0;
    struct rdma_queue_endpoint recv_endpoint;
    recv_endpoint.lkey = nic_app_ctx->rctx_info.lkey;
    recv_endpoint.rkey = nic_app_ctx->rctx_info.rkey;
    recv_endpoint.offset = 0;
    recv_endpoint.addr = nic_app_ctx->rctx_info.memq_base;

    int rv;
    rv = rq_pair_receiver(ctx->kin, recv_endpoint);
    assert(rv == 0);
    offset += kin_qsize;
    recv_endpoint.addr += kin_qsize;

    offset += kout_qsize;
    recv_endpoint.addr += kout_qsize;
    for (int i = 0; i < tas_info->cores_num; i++) {
      offset += nic_rx_len;
      recv_endpoint.addr += nic_rx_len;
      
      rv = rq_pair_receiver(ctx->queues[i].tx, recv_endpoint);
      assert(rv == 0);
      offset += nic_tx_len;
      recv_endpoint.addr += nic_tx_len;
    }
  }
  return 0;
}

int flextcp_kernel_reqscale(struct flextcp_context *ctx, uint32_t cores)
{
  struct kernel_appout kin;
  kin.data.req_scale.num_cores = cores;
  kin.type = KERNEL_APPOUT_REQ_SCALE;
  int rv;
  if ((rv = rq_try_reserve_flush(ctx->kin, sizeof(kin))) != 1) {
    assert(rv == 0);
    fprintf(stderr, "flextcp_kernel_reqscale: no queue space\n");
    return -1;
  }

  if ((rv = rq_try_enqueue(ctx->kin, &kin, sizeof(kin))) != 0) {
    assert(rv < 0);
    fprintf(stderr, "flextcp_kernel_reqscale: enqueue failed\n");
    abort();
  }
  MEM_BARRIER();
  flextcp_kernel_kick();

  return 0;
}
