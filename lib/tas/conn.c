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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tas_ll.h>
#include <kernel_appif.h>
#include <rdma_queue.h>
#include "internal.h"
#include <tas_ll_connect.h>

static void connection_init(struct flextcp_connection *conn);

static inline void conn_mark_bump(struct flextcp_context *ctx,
    struct flextcp_connection *conn);

static inline void enqueue_appout(struct flextcp_context* ctx, struct kernel_appout kin) {
  int rv;
  if ((rv = rq_try_enqueue(ctx->kin, &kin, sizeof(kin))) != 0) {
    assert(rv < 0);
    fprintf(stderr, "enqueue_appout: enqueue failed\n");
    abort();
  }
  MEM_BARRIER();
  if ((rv = rq_flush(ctx->kin)) == sizeof(kin)) {
    flextcp_kernel_kick();
  } else {
    assert(rv < 0);
    fprintf(stderr, "enqueue_appout: failed to flush\n");
    abort();
  }
}

int flextcp_listen_open(struct flextcp_context *ctx,
    struct flextcp_listener *lst, uint16_t port, uint32_t backlog,
    uint32_t flags)
{
  struct kernel_appout kin;
  uint32_t f = 0;
  int rv;
  if ((rv = rq_try_reserve_flush(ctx->kin, sizeof(kin))) != 1) {
    assert(rv == 0);
    fprintf(stderr, "flextcp_connection_move: no queue space\n");
    return -1;
  }

  memset(lst, 0, sizeof(*lst));

  if ((flags & ~(FLEXTCP_LISTEN_REUSEPORT)) != 0) {
    fprintf(stderr, "flextcp_listen_open: unknown flags (%x)\n", flags);
    return -1;
  }

  if ((flags & FLEXTCP_LISTEN_REUSEPORT) == FLEXTCP_LISTEN_REUSEPORT) {
    f |= KERNEL_APPOUT_LISTEN_REUSEPORT;
  }

  lst->conns = NULL;
  lst->local_port = port;
  lst->status = 0;

  kin.data.listen_open.opaque = OPAQUE(lst);
  kin.data.listen_open.local_port = port;
  kin.data.listen_open.backlog = backlog;
  kin.data.listen_open.flags = f;
  kin.type = KERNEL_APPOUT_LISTEN_OPEN;
  enqueue_appout(ctx, kin);
  return 0;

}

static int flextcp_create_tcp_buffer(struct flextcp_connection* conn, uint64_t* rx_opaque, uint64_t* tx_opaque, uint32_t* lkey, uint32_t* rkey) {
  size_t tx_len = sizeof(struct rdma_queue) + tas_info->tcp_txbuf_len;
  size_t rx_len = sizeof(struct rdma_queue) + tas_info->tcp_rxbuf_len;
  size_t bytes_needed = tx_len + rx_len;
  // allocate necessary queues
  void* base = malloc(bytes_needed);
  if ((conn->q_mr = 
    ibv_reg_mr(
      nic_ctx->prot_domain, 
      base, 
      bytes_needed, 
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ)
    ) == NULL) 
  {
    fprintf(stderr, "flextcp_create_tcp_buffer: failed to protect tcp buffer\n");
    free(base);
    return -1;
  }
  conn->rx = rq_allocate_in_place(base, rx_len, nic_conn->qp, 
    conn->q_mr->lkey, conn->q_mr->rkey);
  assert(conn->rx->buffer_size == tas_info->tcp_rxbuf_len);
  conn->tx = rq_allocate_in_place((void*) ((uintptr_t) base + rx_len), 
    tx_len, nic_conn->qp, conn->q_mr->lkey, conn->q_mr->rkey);
  assert((uintptr_t) conn->rx + sizeof(struct rdma_queue) + conn->rx->buffer_size == (uintptr_t) conn->tx);
  assert((uintptr_t) conn->rx + bytes_needed == (uintptr_t) conn->tx + conn->tx->buffer_size + sizeof(struct rdma_queue));
  *rx_opaque = (uint64_t) conn->rx;
  *tx_opaque = (uint64_t) conn->tx;
  *lkey = conn->q_mr->lkey;
  *rkey = conn->q_mr->rkey;
  return 0;
}

int flextcp_listen_accept(struct flextcp_context *ctx,
    struct flextcp_listener *lst, struct flextcp_connection *conn)
{
  struct kernel_appout kin;
  int rv;
  if ((rv = rq_try_reserve_flush(ctx->kin, sizeof(kin))) != 1) {
    assert(rv == 0);
    fprintf(stderr, "flextcp_listen_accept: no queue space\n");
    return -1;
  }

  connection_init(conn);

  conn->status = CONN_ACCEPT_REQUESTED;
  conn->local_port = lst->local_port;

  kin.data.accept_conn.listen_opaque = OPAQUE(lst);
  kin.data.accept_conn.conn_opaque = OPAQUE(conn);
  kin.data.accept_conn.local_port = lst->local_port;
  if (rdmafd != -1) {
    int rv = flextcp_create_tcp_buffer(
      conn, 
      &kin.data.accept_conn.opaque_rx, 
      &kin.data.accept_conn.opaque_tx, 
      &kin.data.accept_conn.lkey, 
      &kin.data.accept_conn.rkey
    );
    if (rv < 0) {
      return -1;
    }
  } else {
    kin.data.accept_conn.opaque_rx = 0;
    kin.data.accept_conn.opaque_tx = 0;
  }
  kin.type = KERNEL_APPOUT_ACCEPT_CONN;
  enqueue_appout(ctx, kin);
  return 0;
}

int flextcp_connection_open(struct flextcp_context *ctx,
    struct flextcp_connection *conn, uint32_t dst_ip, uint16_t dst_port)
{
  uint32_t f = 0;
  struct kernel_appout kin;

  connection_init(conn);

  int rv;
  if ((rv = rq_try_reserve_flush(ctx->kin, sizeof(kin))) != 1) {
    assert(rv == 0);
    fprintf(stderr, "flextcp_connection_open: no queue space\n");
    return -1;
  }

  conn->status = CONN_OPEN_REQUESTED;
  conn->remote_ip = dst_ip;
  conn->remote_port = dst_port;

  kin.data.conn_open.opaque = OPAQUE(conn);
  kin.data.conn_open.remote_ip = dst_ip;
  kin.data.conn_open.remote_port = dst_port;
  kin.data.conn_open.flags = f;
  if (rdmafd != -1) {
    int rv = flextcp_create_tcp_buffer(
      conn, 
      &kin.data.conn_open.opaque_rx, 
      &kin.data.conn_open.opaque_tx, 
      &kin.data.conn_open.lkey, 
      &kin.data.conn_open.rkey
    );
    if (rv < 0) {
      return -1;
    }
  } else {
    kin.data.conn_open.opaque_rx = 0;
    kin.data.conn_open.opaque_tx = 0;
  }

  kin.type = KERNEL_APPOUT_CONN_OPEN;

  enqueue_appout(ctx, kin);
  return 0;


}

int flextcp_connection_close(struct flextcp_context *ctx,
    struct flextcp_connection *conn)
{
  uint32_t f = 0;
  struct kernel_appout kin;
  struct flextcp_connection *p_c;

  /* need to remove connection from bump queue */
  if (conn->bump_pending != 0) {
    if (conn == ctx->bump_pending_first) {
      ctx->bump_pending_first = conn->bump_next;
    } else {
      for (p_c = ctx->bump_pending_first;
          p_c != NULL && p_c->bump_next != conn;
          p_c = p_c->bump_next);

      if (p_c == NULL) {
        fprintf(stderr, "connection_close: didn't find connection in "
            "bump list\n");
        abort();
      }

      p_c->bump_next = conn->bump_next;
      if (p_c->bump_next == NULL) {
        ctx->bump_pending_last = p_c;
      }
    }

    conn->bump_pending = 0;
  }

  /*if (reset)
    f |= KERNEL_APPOUT_CLOSE_RESET;*/
  
  int rv;
  if ((rv = rq_try_reserve_flush(ctx->kin, sizeof(kin))) != 1) {
    assert(rv == 0);
    fprintf(stderr, "flextcp_connection_close: no queue space\n");
    return -1;
  }

  conn->status = CONN_CLOSE_REQUESTED;

  kin.data.conn_close.opaque = (uintptr_t) conn;
  kin.data.conn_close.remote_ip = conn->remote_ip;
  kin.data.conn_close.remote_port = conn->remote_port;
  kin.data.conn_close.local_ip = conn->local_ip;
  kin.data.conn_close.local_port = conn->local_port;
  kin.data.conn_close.flags = f;
  kin.type = KERNEL_APPOUT_CONN_CLOSE;
  enqueue_appout(ctx, kin);
  return 0;
}

int flextcp_connection_rx_done(struct flextcp_context *ctx,
    struct flextcp_connection *conn, size_t len)
{
  int rv;
  if ((rv = rq_try_dequeue(conn->rx, NULL, len)) != 0) {
    assert(rv > 0);
    return -1;
  }

  /* Occasionally update the NIC on what we've already read. Force if buffer was
   * previously completely full*/
  conn->rxb_bump += len;
  if(conn->rxb_bump > conn->rx->buffer_size / 4) {
    conn_mark_bump(ctx, conn);
  }

  return 0;
}

ssize_t flextcp_connection_tx_alloc(struct flextcp_connection *conn, size_t len,
    void **buf)
{
  uint32_t avail;
  uint32_t head;

  /* if outgoing connection has already been closed, abort */
  if ((conn->flags & CONN_FLAG_TXEOS) == CONN_FLAG_TXEOS)
    return -1;

  avail = rq_nbytes_can_reserve(conn->tx);
  assert(avail >= 0);
  /* truncate if necessary */
  if (avail < len) {
    len = avail;
  }

  head = conn->tx->endpoints.tx.offset;

  /* short alloc if we wrap around */
  if (head + len > conn->tx->buffer_size) {
    len = conn->tx->buffer_size - head;
  }

  *buf = rq_tx_tail(conn->tx);

  /* bump head alloc counter */
  conn->txb_allocated += len;

  return len;
}

ssize_t flextcp_connection_tx_alloc2(struct flextcp_connection *conn, size_t len,
    void **buf_1, size_t *len_1, void **buf_2)
{
  uint32_t avail, head;

  /* if outgoing connection has already been closed, abort */
  if ((conn->flags & CONN_FLAG_TXEOS) == CONN_FLAG_TXEOS)
    return -1;

  /* truncate if necessary */
  avail = rq_nbytes_can_reserve(conn->tx);
  assert(avail >= 0);
  /* truncate if necessary */
  if (avail < len) {
    len = avail;
  }

  head = conn->tx->endpoints.tx.offset;

  *buf_1 = rq_tx_tail(conn->tx);

  /* short alloc if we wrap around */
  if (head + len > conn->tx->buffer_size) {
    *len_1 = conn->tx->buffer_size - head;
    *buf_2 = conn->tx->buffer;
  } else {
    *len_1 = len;
    *buf_2 = NULL;
  }

  /* bump head alloc counter */
  conn->txb_allocated += len;
  return len;
}

int flextcp_connection_tx_send(struct flextcp_context *ctx,
    struct flextcp_connection *conn, size_t len)
{
  if (conn->txb_allocated < len) {
    return -1;
  }

  int rv;
  if ((rv = rq_try_enqueue(conn->tx, NULL, len)) != 0) {
    assert(rv < 0);
    fprintf(stderr, "flextcp_connection_tx_send: enqueue failed\n");
    abort();
  }
  // if ((rv = rq_nflush(conn->tx, len)) != len) {
  //   assert(rv < 0);
  //   fprintf(stderr, "flextcp_connection_tx_flush: flush failed\n");
  //   abort();
  // }

  conn->txb_allocated -= len;
  conn->txb_bump += len;
  conn_mark_bump(ctx, conn);
  return 0;
}

int flextcp_connection_tx_close(struct flextcp_context *ctx,
        struct flextcp_connection *conn)
{
  /* if app hasn't sent all data yet, abort */
  if (rq_nbytes_to_flush(conn->tx) > 0) {
    fprintf(stderr, "flextcp_connection_tx_close: has unsent data\n");
    return -1;
  }

  /* if already closed, abort too */
  if ((conn->flags & CONN_FLAG_TXEOS) == CONN_FLAG_TXEOS) {
    fprintf(stderr, "flextcp_connection_tx_close: already closed\n");
    return -1;
  }

  conn->flags |= CONN_FLAG_TXEOS;

  /* try to push out to fastpath */
  flextcp_conn_pushtxeos(ctx, conn);

  return 0;
}

int flextcp_conn_pushtxeos(struct flextcp_context *ctx,
        struct flextcp_connection *conn)
{
  assert(rq_nbytes_to_flush(conn->tx) == 0);
  assert((conn->flags & CONN_FLAG_TXEOS));

  /* if there is no tx buffer space we'll postpone until the next tx bump */
  if (rq_nbytes_empty(conn->tx) == 0) {
    return -1;
  }

  int rv;
  uint8_t dummy = 1;
  if ((rv = rq_try_enqueue(conn->tx, &dummy, sizeof(dummy))) != 0) {
    assert(rv < 0);
    fprintf(stderr, "flextcp_conn_pushtxeos: failed enqueue\n");
    abort();
  }

  conn->flags |= CONN_FLAG_TXEOS_ALLOC;
  conn->txb_bump++;
  conn_mark_bump(ctx, conn);
  return 0;
}

int flextcp_connection_tx_possible(struct flextcp_context *ctx,
    struct flextcp_connection *conn)
{
  return 0;
}

int flextcp_connection_move(struct flextcp_context *ctx,
        struct flextcp_connection *conn)
{
  struct kernel_appout kin;
  int rv;
  if ((rv = rq_try_reserve_flush(ctx->kin, sizeof(kin))) != 1) {
    assert(rv == 0);
    fprintf(stderr, "flextcp_connection_move: no queue space\n");
    return -1;
  }

  kin.data.conn_move.local_ip = conn->local_ip;
  kin.data.conn_move.remote_ip = conn->remote_ip;
  kin.data.conn_move.local_port = conn->local_port;
  kin.data.conn_move.remote_port = conn->remote_port;
  kin.data.conn_move.db_id = ctx->db_id;
  kin.data.conn_move.opaque = OPAQUE(conn);
  kin.type = KERNEL_APPOUT_CONN_MOVE;
  enqueue_appout(ctx, kin);
  return 0;
}

static void connection_init(struct flextcp_connection *conn)
{
  memset(conn, 0, sizeof(*conn));
  conn->status = CONN_CLOSED;
}

static inline void conn_mark_bump(struct flextcp_context *ctx,
    struct flextcp_connection *conn)
{
  struct flextcp_connection *c_prev;

  if (conn->bump_pending) {
    return;
  }

  c_prev = ctx->bump_pending_last;
  conn->bump_next = NULL;
  conn->bump_prev = c_prev;
  if (c_prev != NULL) {
    c_prev->bump_next = conn;
  } else {
    ctx->bump_pending_first = conn;
  }
  ctx->bump_pending_last = conn;

  conn->bump_pending = 1;
}