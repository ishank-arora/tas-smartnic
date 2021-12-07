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

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include <tas_ll_connect.h>
#include <kernel_appif.h>
#include <tas_ll.h>
#include <tas_memif.h>
#include <utils_timeout.h>
#include "internal.h"
#include <rdma_queue.h>

static inline int event_kappin_conn_opened(
    const struct kernel_appin_conn_opened *inev, struct flextcp_event *outev,
    unsigned avail);
static inline void event_kappin_listen_newconn(
    const struct kernel_appin_listen_newconn *inev, struct flextcp_event *outev);
static inline int event_kappin_accept_conn(
    const struct kernel_appin_accept_conn *inev, struct flextcp_event *outev,
    unsigned avail);
static inline void event_kappin_st_conn_move(
    const struct kernel_appin_status *inev, struct flextcp_event *outev);
static inline void event_kappin_st_listen_open(
    const struct kernel_appin_status *inev, struct flextcp_event *outev);
static inline void event_kappin_st_conn_closed(
    const struct kernel_appin_status *inev, struct flextcp_event *outev);

static inline int event_arx_connupdate(struct flextcp_context *ctx,
    struct flextcp_pl_arx_connupdate *inev,
    struct flextcp_event *outevs, int outn, uint16_t fn_core);

static int kernel_poll(struct flextcp_context *ctx, int num,
    struct flextcp_event *events, int *used) __attribute__((noinline));
static int fastpath_poll(struct flextcp_context *ctx, int num,
    struct flextcp_event *events, int *used)
    __attribute__((used,noinline));
static int fastpath_poll_vec(struct flextcp_context *ctx, int num,
    struct flextcp_event *events, int *used) __attribute__((used,noinline));
static void conns_bump(struct flextcp_context *ctx) __attribute__((noinline));
// static void txq_probe(struct flextcp_context *ctx, unsigned n) __attribute__((noinline));

void *flexnic_mem = NULL;
struct flexnic_info *tas_info = NULL;
int flexnic_evfd[FLEXTCP_MAX_FTCPCORES];
int rdmafd;

int flextcp_init(void)
{
  if (flexnic_driver_connect(&tas_info, &flexnic_mem) != 0) {
    fprintf(stderr, "flextcp_init: connecting to flexnic failed\n");
    return -1;
  }

  if (flextcp_kernel_connect() != 0) {
    fprintf(stderr, "flextcp_init: connecting to kernel failed\n");
    return -1;
  }

  return 0;
}

int flextcp_context_create(struct flextcp_context *ctx)
{
  static uint16_t ctx_id = 0;

  memset(ctx, 0, sizeof(*ctx));

  ctx->ctx_id = __sync_fetch_and_add(&ctx_id, 1);
  if (ctx->ctx_id >= FLEXTCP_MAX_CONTEXTS) {
    fprintf(stderr, "flextcp_context_create: maximum number of contexts "
        "exeeded\n");
    return -1;
  }

  ctx->evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (ctx->evfd < 0) {
    perror("flextcp_context_create: eventfd for waiting fd failed");
    return -1;
  }

  return flextcp_kernel_newctx(ctx);
}

#include <pthread.h>

int debug_flextcp_on = 0;

static int kernel_poll(struct flextcp_context *ctx, int num,
    struct flextcp_event *events, int *used)
{
  int i, j = 0;
  struct kernel_appin kout;
  uint8_t type;

  /* poll kernel queues */
  for (i = 0; i < num;) {
    j = 1;
    int rv;
    if ((rv = rq_try_dequeue(ctx->kout, &kout, sizeof(kout))) != 0) {
      assert(rv > 0);
      break;
    }

    type = kout.type;
    if (type == KERNEL_APPIN_CONN_OPENED) {
      printf("Connection opened\n");
      j = event_kappin_conn_opened(&kout.data.conn_opened, &events[i],
          num - i);
    } else if (type == KERNEL_APPIN_LISTEN_NEWCONN) {
      printf("New connection\n");
      event_kappin_listen_newconn(&kout.data.listen_newconn, &events[i]);
    } else if (type == KERNEL_APPIN_ACCEPTED_CONN) {
      printf("Connection accepted\n");
      j = event_kappin_accept_conn(&kout.data.accept_connection, &events[i],
          num - i);
    } else if (type == KERNEL_APPIN_STATUS_LISTEN_OPEN) {
      printf("Listen opened\n");
      event_kappin_st_listen_open(&kout.data.status, &events[i]);
    } else if (type == KERNEL_APPIN_STATUS_CONN_MOVE) {
      printf("Connection moved\n");
      event_kappin_st_conn_move(&kout.data.status, &events[i]);
    } else if (type == KERNEL_APPIN_STATUS_CONN_CLOSE) {
      printf("Connection closed\n");
      event_kappin_st_conn_closed(&kout.data.status, &events[i]);
    } else {
      fprintf(stderr, "flextcp_context_poll: unexpected kout type=%u\n",
          type);
      abort();
    }
    ctx->flags |= CTX_FLAG_POLL_EVENTS;

    if (j == -1) {
      break;
    }

    i += j;
  }

  *used = i;
  return (j == -1 ? -1 : 0);
}

static int fastpath_poll(struct flextcp_context *ctx, int num,
    struct flextcp_event *events, int *used)
{
  int i, j, ran_out;
  uint16_t k;

  i = 0;
  for (k = 0; k < ctx->num_queues && i < num; k++) {
    ran_out = 0;

    struct rdma_queue* rx = ctx->queues[ctx->next_queue].rx;
    for (; i < num;) {
      j = 0;
      struct flextcp_pl_arx arx;
      int rv;
      if ((rv = rq_try_dequeue(rx, &arx, sizeof(arx))) != 0) {
        assert(rv > 0);
        break;
      }

      if (arx.type == FLEXTCP_PL_ARX_CONNUPDATE) {
        j = event_arx_connupdate(ctx, &arx.msg.connupdate, events + i, num - i, ctx->next_queue);
      } else {
        fprintf(stderr, "flextcp_context_poll: kout type=%u\n", arx.type);
      }
      ctx->flags |= CTX_FLAG_POLL_EVENTS;

      if (j == -1) {
        ran_out = 1;
        break;
      }
      i += j;
    }

    if (ran_out) {
      *used = i;
      return -1;
    }

    ctx->next_queue = ctx->next_queue + 1;
    if (ctx->next_queue >= ctx->num_queues)
      ctx->next_queue -= ctx->num_queues;
  }

  *used = i;
  return 0;
}

// static inline void fetch_8ts(struct flextcp_context *ctx, uint32_t *heads,
//     uint16_t q, uint8_t *ts)
// {
//   struct flextcp_pl_arx *p0, *p1, *p2, *p3, *p4, *p5, *p6, *p7;

//   p0 = (struct flextcp_pl_arx *) (ctx->queues[q].rxq_base + heads[q]);
//   q = (q + 1 < ctx->num_queues ? q + 1 : 0);
//   p1 = (struct flextcp_pl_arx *) (ctx->queues[q].rxq_base + heads[q]);
//   q = (q + 1 < ctx->num_queues ? q + 1 : 0);
//   p2 = (struct flextcp_pl_arx *) (ctx->queues[q].rxq_base + heads[q]);
//   q = (q + 1 < ctx->num_queues ? q + 1 : 0);
//   p3 = (struct flextcp_pl_arx *) (ctx->queues[q].rxq_base + heads[q]);
//   q = (q + 1 < ctx->num_queues ? q + 1 : 0);
//   p4 = (struct flextcp_pl_arx *) (ctx->queues[q].rxq_base + heads[q]);
//   q = (q + 1 < ctx->num_queues ? q + 1 : 0);
//   p5 = (struct flextcp_pl_arx *) (ctx->queues[q].rxq_base + heads[q]);
//   q = (q + 1 < ctx->num_queues ? q + 1 : 0);
//   p6 = (struct flextcp_pl_arx *) (ctx->queues[q].rxq_base + heads[q]);
//   q = (q + 1 < ctx->num_queues ? q + 1 : 0);
//   p7 = (struct flextcp_pl_arx *) (ctx->queues[q].rxq_base + heads[q]);
//   q = (q + 1 < ctx->num_queues ? q + 1 : 0);
// #ifdef __amd64__
//   asm volatile(
//       "prefetcht0 32(%0);"
//       "prefetcht0 32(%1);"
//       "prefetcht0 32(%2);"
//       "prefetcht0 32(%3);"
//       "prefetcht0 32(%4);"
//       "prefetcht0 32(%5);"
//       "prefetcht0 32(%6);"
//       "prefetcht0 32(%7);"
//       "movb 31(%0), %b0;"
//       "movb 31(%1), %b1;"
//       "movb 31(%2), %b2;"
//       "movb 31(%3), %b3;"
//       "movb 31(%4), %b4;"
//       "movb 31(%5), %b5;"
//       "movb 31(%6), %b6;"
//       "movb 31(%7), %b7;"

//       "movb %b0, 0(%8);"
//       "movb %b1, 1(%8);"
//       "movb %b2, 2(%8);"
//       "movb %b3, 3(%8);"
//       "movb %b4, 4(%8);"
//       "movb %b5, 5(%8);"
//       "movb %b6, 6(%8);"
//       "movb %b7, 7(%8);"
//       :
//       : "r" (p0), "r" (p1), "r" (p2), "r" (p3),
//         "r" (p4), "r" (p5), "r" (p6), "r" (p7), "r" (ts)
//       : "memory");
// #elif __aarch64__
//   util_prefetch0((unsigned char *)p0 + 32);
//   util_prefetch0((unsigned char *)p1 + 32);
//   util_prefetch0((unsigned char *)p2 + 32);
//   util_prefetch0((unsigned char *)p3 + 32);
//   util_prefetch0((unsigned char *)p4 + 32);
//   util_prefetch0((unsigned char *)p5 + 32);
//   util_prefetch0((unsigned char *)p6 + 32);
//   util_prefetch0((unsigned char *)p7 + 32);
//   asm volatile (
//     "dmb ish;"
//     "ldrb %w0, [%0, 31];"
//     "ldrb %w1, [%1, 31];"
//     "ldrb %w2, [%2, 31];"
//     "ldrb %w3, [%3, 31];"
//     "ldrb %w4, [%4, 31];"
//     "ldrb %w5, [%5, 31];"
//     "ldrb %w6, [%6, 31];"
//     "ldrb %w7, [%7, 31];"

//     "strb %w0, [%8, 0];"
//     "strb %w1, [%8, 1];"
//     "strb %w2, [%8, 2];"
//     "strb %w3, [%8, 3];"
//     "strb %w4, [%8, 4];"
//     "strb %w5, [%8, 5];"
//     "strb %w6, [%8, 6];"
//     "strb %w7, [%8, 7];"
//     :
//     : "r" (p0), "r" (p1), "r" (p2), "r" (p3),
//       "r" (p4), "r" (p5), "r" (p6), "r" (p7), "r" (ts)
//     : "memory");
// #endif
// }

// static inline void fetch_4ts(struct flextcp_context *ctx, uint32_t *heads,
//     uint16_t q, uint8_t *ts)
// {
//   struct flextcp_pl_arx *p0, *p1, *p2, *p3;
//   p0 = (struct flextcp_pl_arx *) (ctx->queues[q].rxq_base + heads[q]);
//   q = (q + 1 < ctx->num_queues ? q + 1 : 0);
//   p1 = (struct flextcp_pl_arx *) (ctx->queues[q].rxq_base + heads[q]);
//   q = (q + 1 < ctx->num_queues ? q + 1 : 0);
//   p2 = (struct flextcp_pl_arx *) (ctx->queues[q].rxq_base + heads[q]);
//   q = (q + 1 < ctx->num_queues ? q + 1 : 0);
//   p3 = (struct flextcp_pl_arx *) (ctx->queues[q].rxq_base + heads[q]);
//   q = (q + 1 < ctx->num_queues ? q + 1 : 0);
// #ifdef __amd64__    
//   asm volatile(
//       "prefetcht0 32(%0);"
//       "prefetcht0 32(%1);"
//       "prefetcht0 32(%2);"
//       "prefetcht0 32(%3);"
//       "movb 31(%0), %b0;"
//       "movb 31(%1), %b1;"
//       "movb 31(%2), %b2;"
//       "movb 31(%3), %b3;"
//       "movb %b0, 0(%4);"
//       "movb %b1, 1(%4);"
//       "movb %b2, 2(%4);"
//       "movb %b3, 3(%4);"
//       :
//       : "r" (p0), "r" (p1), "r" (p2), "r" (p3), "r" (ts)
//       : "memory");
// #elif defined(__aarch64__)
//   // IMPLEMENT STUB
//   util_prefetch0((unsigned char *)p0 + 32);
//   util_prefetch0((unsigned char *)p1 + 32);
//   util_prefetch0((unsigned char *)p2 + 32);
//   util_prefetch0((unsigned char *)p3 + 32);
//   asm volatile (
//     "dmb ish;"
//     "ldrb %w0, [%0, 31];"
//     "ldrb %w1, [%1, 31];"
//     "ldrb %w2, [%2, 31];"
//     "ldrb %w3, [%3, 31];"

//     "strb %w0, [%4, 0];"
//     "strb %w1, [%4, 1];"
//     "strb %w2, [%4, 2];"
//     "strb %w3, [%4, 3];"
//     :
//     : "r" (p0), "r" (p1), "r" (p2), "r" (p3),
//       "r" (ts)
//     : "memory");
// #endif
// }


static int fastpath_poll_vec(struct flextcp_context *ctx, int num,
    struct flextcp_event *events, int *used)
{
  int num_used, ran_out, found;
  int queue_idx = ctx->next_queue;

  found = 0;
  num_used = 0;
  ran_out = 0;
  while (!ran_out && num_used < num) {
    int found_inner;
    for (found_inner = 1; !ran_out && found_inner && num_used < num; ) {
      found_inner = 0;

      int queues_encountered;
      for (queues_encountered = 0, queue_idx = ctx->next_queue; 
            !ran_out && queues_encountered < ctx->num_queues && num_used < num; 
            queues_encountered++) {
        struct rdma_queue* rx = ctx->queues[queue_idx].rx;
        struct flextcp_pl_arx arx;
        int rv;
        if ((rv = rq_try_peek(rx, &arx, sizeof(arx))) != 0) {
          assert(rv > 0);
        } else if (arx.type == FLEXTCP_PL_ARX_CONNUPDATE) {
          found_inner = 1;
          int add_used = event_arx_connupdate(ctx, &arx.msg.connupdate, events + num_used,
            num - num_used, queue_idx);
          
          found = 1;
          if (add_used == -1) {
            ran_out = 1;
            break;
          }

          num_used += add_used;

          // dequeue to move onto next record
          int rv = rq_try_dequeue(rx, &arx, sizeof(arx));
          assert(rv == 0);
        }
        queue_idx = (queue_idx + 1 < ctx->num_queues ? queue_idx + 1 : 0);
      }
    }
    if (!found_inner) {
      break;
    }
  }

  ctx->next_queue = queue_idx;

  if (found) {
    ctx->flags |= CTX_FLAG_POLL_EVENTS;
  }

  *used = num_used;
  return 0;
}


int flextcp_context_poll(struct flextcp_context *ctx, int num,
    struct flextcp_event *events)
{
  int i, j;

  i = 0;

  ctx->flags |= CTX_FLAG_POLL_CALLED;

  /* poll kernel */
  if (kernel_poll(ctx, num, events, &i) == -1) {
    /* not enough event space, abort */
    return i;
  }

  /* poll NIC queues */
  fastpath_poll_vec(ctx, num - i, events + i, &j);

  // txq_probe(ctx, num);
  conns_bump(ctx);

  return i + j;
}

// int flextcp_context_tx_alloc(struct flextcp_context *ctx,
//     struct flextcp_pl_atx **patx, uint16_t core)
// {
//   /* if queue is full, abort */
//   if (ctx->queues[core].txq_avail == 0) {
//     return -1;
//   }

//   *patx = (struct flextcp_pl_atx *)
//     (ctx->queues[core].txq_base + ctx->queues[core].txq_tail);
//   return 0;
// }

static void flextcp_flexnic_kick(struct flextcp_context *ctx, int core)
{
  uint64_t now = util_rdtsc();

  if (tas_info->poll_cycle_tas == UINT64_MAX) {
    /* blocking for TAS disabled */
    return;
  }

  if(now - ctx->queues[core].last_ts > tas_info->poll_cycle_tas) {
    // Kick
    uint64_t val = 1;
    int r = write(flexnic_evfd[core], &val, sizeof(uint64_t));
    assert(r == sizeof(uint64_t));
  }

  ctx->queues[core].last_ts = now;
}

// void flextcp_context_tx_done(struct flextcp_context *ctx, uint16_t core)
// {
//   ctx->queues[core].txq_tail += sizeof(struct flextcp_pl_atx);
//   if (ctx->queues[core].txq_tail >= ctx->txq_len) {
//     ctx->queues[core].txq_tail -= ctx->txq_len;
//   }

//   ctx->queues[core].txq_avail -= sizeof(struct flextcp_pl_atx);

//   flextcp_flexnic_kick(ctx, core);
// }

static inline int event_kappin_conn_opened(
    const struct kernel_appin_conn_opened *inev, struct flextcp_event *outev,
    unsigned avail)
{
  struct flextcp_connection *conn;
  int j = 1;

  conn = OPAQUE_PTR(inev->opaque);

  outev->event_type = FLEXTCP_EV_CONN_OPEN;
  outev->ev.conn_open.status = inev->status;
  outev->ev.conn_open.conn = conn;

  uint64_t enqueued_bytes = 0;
  assert(conn->rx->endpoints.rx.addr == (uintptr_t) conn->rx);
  assert(conn->tx->endpoints.tx.addr == (uintptr_t) conn->tx);
  if (inev->status != 0) {
    conn->status = CONN_CLOSED;
    return 1;
  } else {
    enqueued_bytes = rdmafd == -1 ? rq_nbytes_enqueued((struct rdma_queue*) (flexnic_mem + inev->rx_off)) : rq_nbytes_enqueued(conn->rx);
    if (enqueued_bytes > 0 && conn->rx_closed && avail < 3) {
      /* if we've already received updates, we'll need to inject them */
      return -1;
    } else if ((enqueued_bytes > 0 || conn->rx_closed) && avail < 2) {
      /* if we've already received updates, we'll need to inject them */
      return -1;
    }
  }

  conn->status = CONN_OPEN;
  conn->local_ip = inev->local_ip;
  conn->local_port = inev->local_port;
  conn->seq_rx = inev->seq_rx;
  conn->seq_tx = inev->seq_tx;
  conn->flow_id = inev->flow_id;
  conn->fn_core = inev->fn_core;

  assert(conn->rx->endpoints.rx.addr == (uintptr_t) conn->rx);
  assert(conn->tx->endpoints.tx.addr == (uintptr_t) conn->tx);
  if (rdmafd == -1) {
    conn->rx = (struct rdma_queue*) (flexnic_mem + inev->rx_off);
    conn->tx = (struct rdma_queue*) (flexnic_mem + inev->tx_off);
  } else {
    int rv;
    struct rdma_queue_endpoint rx_endpoint;
    struct rdma_app_context* app_ctx = (struct rdma_app_context*) nic_conn->context;
    rx_endpoint.addr = app_ctx->rapp_info.tas_shm_opaque + inev->tx_off;
    rx_endpoint.lkey = app_ctx->rapp_info.tas_shm_lkey;
    rx_endpoint.rkey = app_ctx->rapp_info.tas_shm_rkey;
    rx_endpoint.offset = 0;
    rv = rq_pair_receiver(conn->tx, rx_endpoint);
    assert(rv == 0);
  }

  /* inject bump if necessary */
  if (enqueued_bytes > 0) {
    conn->seq_rx += enqueued_bytes;

    outev[j].event_type = FLEXTCP_EV_CONN_RECEIVED;
    outev[j].ev.conn_received.conn = conn;
    outev[j].ev.conn_received.buf = conn->rx->buffer;
    outev[j].ev.conn_received.len = enqueued_bytes;
    j++;
  }

  /* add end of stream notification if necessary */
  if (conn->rx_closed) {
    outev[j].event_type = FLEXTCP_EV_CONN_RXCLOSED;
    outev[j].ev.conn_rxclosed.conn = conn;
    j++;
  }

  return j;
}

static inline void event_kappin_listen_newconn(
    const struct kernel_appin_listen_newconn *inev, struct flextcp_event *outev)
{
  struct flextcp_listener *listener;

  listener = OPAQUE_PTR(inev->opaque);

  outev->event_type = FLEXTCP_EV_LISTEN_NEWCONN;
  outev->ev.listen_newconn.remote_ip = inev->remote_ip;
  outev->ev.listen_newconn.remote_port = inev->remote_port;
  outev->ev.listen_open.listener = listener;
}

static inline int event_kappin_accept_conn(
    const struct kernel_appin_accept_conn *inev, struct flextcp_event *outev,
    unsigned avail)
{
  struct flextcp_connection *conn;
  int j = 1;

  conn = OPAQUE_PTR(inev->opaque);

  outev->event_type = FLEXTCP_EV_LISTEN_ACCEPT;
  outev->ev.listen_accept.status = inev->status;
  outev->ev.listen_accept.conn = conn;

  uint64_t enqueued_bytes = rdmafd == -1 ? 
    rq_nbytes_enqueued((struct rdma_queue*) (flexnic_mem + inev->rx_off)) :
    rq_nbytes_enqueued(conn->rx);
  if (inev->status != 0) {
    conn->status = CONN_CLOSED;
    return 1;
  } else if (enqueued_bytes > 0 && conn->rx_closed && avail < 3) {
    /* if we've already received updates, we'll need to inject them */
    return -1;
  } else if ((enqueued_bytes > 0 || conn->rx_closed) && avail < 2) {
    /* if we've already received updates, we'll need to inject them */
    return -1;
  }

  conn->status = CONN_OPEN;
  conn->local_ip = inev->local_ip;
  conn->remote_ip = inev->remote_ip;
  conn->remote_port = inev->remote_port;
  conn->seq_rx = inev->seq_rx;
  conn->seq_tx = inev->seq_tx;
  conn->flow_id = inev->flow_id;
  conn->fn_core = inev->fn_core;

  if (rdmafd == -1) {
    conn->rx = (struct rdma_queue*) (flexnic_mem + inev->rx_off);
    conn->tx = (struct rdma_queue*) (flexnic_mem + inev->tx_off);
  } else {
    int rv;
    struct rdma_queue_endpoint rx_endpoint;
    struct rdma_app_context* app_ctx = (struct rdma_app_context*) nic_conn->context;
    rx_endpoint.addr = app_ctx->rapp_info.tas_shm_opaque + inev->tx_off;
    rx_endpoint.lkey = app_ctx->rapp_info.tas_shm_lkey;
    rx_endpoint.rkey = app_ctx->rapp_info.tas_shm_rkey;
    rx_endpoint.offset = 0;
    rv = rq_pair_receiver(conn->tx, rx_endpoint);
    assert(rv == 0);
  }

  assert(conn->rx->endpoints.rx.addr == (uintptr_t) conn->rx);
  assert(conn->tx->endpoints.tx.addr == (uintptr_t) conn->tx);
  // TODO: create queues and pair with tas_nic

  /* inject bump if necessary */
  if (enqueued_bytes > 0) {
    conn->seq_rx += enqueued_bytes;

    outev[j].event_type = FLEXTCP_EV_CONN_RECEIVED;
    outev[j].ev.conn_received.conn = conn;
    outev[j].ev.conn_received.buf = conn->rx->buffer;
    outev[j].ev.conn_received.len = enqueued_bytes;
    j++;
  }

  /* add end of stream notification if necessary */
  if (conn->rx_closed) {
    outev[j].event_type = FLEXTCP_EV_CONN_RXCLOSED;
    outev[j].ev.conn_rxclosed.conn = conn;
    j++;
  }

  return j;
}

static inline void event_kappin_st_conn_move(
    const struct kernel_appin_status *inev, struct flextcp_event *outev)
{
  struct flextcp_connection *conn;

  conn = OPAQUE_PTR(inev->opaque);

  outev->event_type = FLEXTCP_EV_CONN_MOVED;
  outev->ev.conn_moved.status = inev->status;
  outev->ev.conn_moved.conn = conn;
}

static inline void event_kappin_st_listen_open(
    const struct kernel_appin_status *inev, struct flextcp_event *outev)
{
  struct flextcp_listener *listener;

  listener = OPAQUE_PTR(inev->opaque);

  outev->event_type = FLEXTCP_EV_LISTEN_OPEN;
  outev->ev.listen_open.status = inev->status;
  outev->ev.listen_open.listener = listener;
}

static inline void event_kappin_st_conn_closed(
    const struct kernel_appin_status *inev, struct flextcp_event *outev)
{
  struct flextcp_connection *conn;

  conn = OPAQUE_PTR(inev->opaque);

  outev->event_type = FLEXTCP_EV_CONN_CLOSED;
  outev->ev.conn_closed.status = inev->status;
  outev->ev.conn_closed.conn = conn;

  conn->status = CONN_CLOSED;
}

static inline int event_arx_connupdate(struct flextcp_context *ctx,
    struct flextcp_pl_arx_connupdate *inev,
    struct flextcp_event *outevs, int outn, uint16_t fn_core)
{
  struct flextcp_connection *conn;
  uint32_t rx_bump, rx_len, tx_bump, tx_sent;
  int i = 0, evs_needed, tx_avail_ev, eos;

  conn = OPAQUE_PTR(inev->opaque);

  conn->fn_core = fn_core;

  rx_bump = inev->rx_bump;
  tx_bump = inev->tx_bump;
  eos = ((inev->flags & FLEXTCP_PL_ARX_FLRXDONE) == FLEXTCP_PL_ARX_FLRXDONE);

  if (conn->status == CONN_OPEN_REQUESTED ||
      conn->status == CONN_ACCEPT_REQUESTED)
  {
    /* due to a race we might see connection updates before we see the
     * connection confirmation from the kernel */
    assert(tx_bump == 0);
    conn->rx_closed = !!eos;
    /* TODO: should probably handle eos here as well */
    return 0;
  } else if (conn->status == CONN_CLOSED ||
      conn->status == CONN_CLOSE_REQUESTED)
  {
    /* just drop bumps for closed connections */
    return 0;
  }

  assert(conn->status == CONN_OPEN);

  /* figure out how many events for rx */
  evs_needed = 0;
  if (rx_bump > 0) {
    evs_needed++;
    if (conn->rx->endpoints.rx.offset + rx_bump > conn->rx->buffer_size) {
      evs_needed++;
    }
  }

  /* if tx buffer was depleted, we'll generate a tx avail event */
  tx_avail_ev = (tx_bump > 0 && rq_nbytes_empty(conn->tx) == 0);
  if (tx_avail_ev) {
    evs_needed++;
  }

  tx_sent = rq_nbytes_to_flush(conn->tx);

  /* if tx close was acked, also add that event */
  if ((conn->flags & CONN_FLAG_TXEOS_ALLOC) == CONN_FLAG_TXEOS_ALLOC &&
      !tx_sent)
  {
    evs_needed++;
  }

  /* if receive stream closed need additional event */
  if (eos) {
    evs_needed++;
  }

  /* if we can't fit all events, try again later */
  if (evs_needed > outn) {
    return -1;
  }

  /* generate rx events */
  if (rx_bump > 0) {
    assert(rq_nbytes_enqueued(conn->rx) >= rx_bump);
    outevs[i].event_type = FLEXTCP_EV_CONN_RECEIVED;
    outevs[i].ev.conn_received.conn = conn;
    outevs[i].ev.conn_received.buf = rq_rx_head(conn->rx);
    if (conn->rx->endpoints.rx.offset + rx_bump > conn->rx->buffer_size) {
      /* wrap around in rx buffer */
      rx_len = conn->rx->buffer_size - conn->rx->endpoints.rx.offset;
      outevs[i].ev.conn_received.len = rx_len;

      i++;
      outevs[i].event_type = FLEXTCP_EV_CONN_RECEIVED;
      outevs[i].ev.conn_received.conn = conn;
      outevs[i].ev.conn_received.buf = conn->rx->buffer;
      outevs[i].ev.conn_received.len = rx_bump - rx_len;
    } else {
      outevs[i].ev.conn_received.len = rx_bump;
    }
    i++;

    /* update rx buffer */
    conn->seq_rx += rx_bump;
  }

  /* bump tx */
  if (tx_bump > 0) {
    if (tx_avail_ev) {
      outevs[i].event_type = FLEXTCP_EV_CONN_SENDBUF;
      outevs[i].ev.conn_sendbuf.conn = conn;
      i++;
    }

    /* if we were previously unable to push out TX EOS, do so now. */
    if ((conn->flags & CONN_FLAG_TXEOS) == CONN_FLAG_TXEOS &&
        !(conn->flags & CONN_FLAG_TXEOS_ALLOC))
    {
      if (flextcp_conn_pushtxeos(ctx, conn) != 0) {
        /* should never happen */
        fprintf(stderr, "event_arx_connupdate: flextcp_conn_pushtxeos "
            "failed\n");
        abort();
      }
    } else if ((conn->flags & CONN_FLAG_TXEOS_ALLOC) == CONN_FLAG_TXEOS_ALLOC) {
      /* There should be no data after we push out the EOS */
      assert(!(conn->flags & CONN_FLAG_TXEOS_ACK));

      /* if this was the last bump, mark TX EOS as acked */
      if (conn->txb_bump == 0) {
        conn->flags |= CONN_FLAG_TXEOS_ACK;

        outevs[i].event_type = FLEXTCP_EV_CONN_TXCLOSED;
        outevs[i].ev.conn_txclosed.conn = conn;
        i++;
      }
    }
  }

  /* add end of stream notification */
  if (eos) {
    outevs[i].event_type = FLEXTCP_EV_CONN_RXCLOSED;
    outevs[i].ev.conn_rxclosed.conn = conn;
    conn->rx_closed = 1;
    i++;
  }

  return i;
}


// static void txq_probe(struct flextcp_context *ctx, unsigned n)
// {
//   struct flextcp_pl_atx *atx;
//   uint32_t pos, i, q, tail, avail, len;

//   len = ctx->txq_len;
//   for (q = 0; q < ctx->num_queues; q++) {
//     avail = ctx->queues[q].txq_avail;

//     if (avail > len / 2)
//       continue;

//     tail = ctx->queues[q].txq_tail;

//     pos = tail + avail;
//     if (pos >= len)
//       pos -= len;

//     i = 0;
//     while (avail < len && i < 2 * n) {
//       atx = (struct flextcp_pl_atx *) (ctx->queues[q].txq_base + pos);

//       if (atx->type != 0) {
//         break;
//       }

//       avail += sizeof(*atx);
//       pos += sizeof(*atx);
//       if (pos >= len)
//         pos -= len;
//       i++;

//       MEM_BARRIER();
//     }

//     ctx->queues[q].txq_avail = avail;
//   }
// }

static void conns_bump(struct flextcp_context *ctx)
{
  struct flextcp_connection *c;
  struct flextcp_pl_atx atx;
  uint8_t flags;

  while ((c = ctx->bump_pending_first) != NULL) {
    assert(c->status == CONN_OPEN);

    struct rdma_queue* tx = ctx->queues[c->fn_core].tx;
    int rv;
    if ((rv = rq_try_reserve_flush(tx, sizeof(atx))) != 1) {
      assert(rv == 0);
      break;
    }

    flags = 0;

    if ((c->flags & CONN_FLAG_TXEOS_ALLOC) == CONN_FLAG_TXEOS_ALLOC) {
      flags |= FLEXTCP_PL_ATX_FLTXDONE;
    }

    int n_tx_bumped;
    if ((n_tx_bumped = rq_nflush(c->tx, c->txb_bump)) < 0) {
      fprintf(stderr, "conns_bump: failed flush\n");
      abort();
    }

    atx.msg.connupdate.rx_bump = c->rxb_bump;
    atx.msg.connupdate.tx_bump = n_tx_bumped;
    atx.msg.connupdate.flow_id = c->flow_id;
    atx.msg.connupdate.bump_seq = c->bump_seq++;
    atx.msg.connupdate.flags = flags;
    atx.type = FLEXTCP_PL_ATX_CONNUPDATE;

    c->txb_bump -= n_tx_bumped; 
    c->rxb_bump = 0;
    c->bump_pending = 0;

    if (c->bump_next == NULL) {
      ctx->bump_pending_last = NULL;
    }
    ctx->bump_pending_first = c->bump_next;

    rv = rq_try_enqueue(tx, &atx, sizeof(atx));
    assert(rv == 0);
    rv = rq_flush(tx);
    assert(rv == sizeof(atx));
    MEM_BARRIER();
    flextcp_flexnic_kick(ctx, c->fn_core);
  }
}

int flextcp_context_waitfd(struct flextcp_context *ctx)
{
  return ctx->evfd;
}

int flextcp_context_canwait(struct flextcp_context *ctx)
{
  /* At a high level this code implements a state machine that ensures that at
   * least POLL_CYCLE time has elapsed between two unsuccessfull poll calls.
   * This is a bit messier because we don't want to move any of the timestamp
   * code into the poll call and make it more expensive for apps that don't
   * block. Instead we use the timing of calls to canwait along with flags that
   * the poll call sets when it's called and when it finds events.
   */

  /* if blocking is disabled, we can never wait */
  if (tas_info->poll_cycle_app == UINT64_MAX) {
    return -1;
  }

  /* if there were events found in the last poll, it's back to square one. */
  if ((ctx->flags & CTX_FLAG_POLL_EVENTS) != 0) {
    ctx->flags &= ~(CTX_FLAG_POLL_EVENTS | CTX_FLAG_WANTWAIT |
        CTX_FLAG_LASTWAIT);

    return -1;
  }

  /* from here on we know that there are no events */
  if ((ctx->flags & CTX_FLAG_WANTWAIT) != 0) {
    /* in want wait state: just wait for grace period to be over */
    if ((util_rdtsc() - ctx->last_inev_ts) > tas_info->poll_cycle_app) {
      /* past grace period, move on to lastwait. clear polled flag, to make sure
       * it gets polled again before we clear lastwait. */
      ctx->flags &= ~(CTX_FLAG_POLL_CALLED | CTX_FLAG_WANTWAIT);
      ctx->flags |= CTX_FLAG_LASTWAIT;
    }
  } else if ((ctx->flags & CTX_FLAG_LASTWAIT) != 0) {
    /* in last wait state */
    if ((ctx->flags & CTX_FLAG_POLL_CALLED) != 0) {
      /* if we have polled once more after the grace period, we're good to go to
       * sleep */
      return 0;
    }
  } else if ((ctx->flags & CTX_FLAG_POLL_CALLED) != 0) {
    /* not currently getting ready to wait, so start */
    ctx->last_inev_ts = util_rdtsc();
    ctx->flags |= CTX_FLAG_WANTWAIT;
  }

  return -1;
}

void flextcp_context_waitclear(struct flextcp_context *ctx)
{
  ssize_t ret;
  uint64_t val;

  ret = read(ctx->evfd, &val, sizeof(uint64_t));
  if ((ret >= 0 && ret != sizeof(uint64_t)) ||
      (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
  {
    perror("flextcp_context_waitclear: read failed");
    abort();
  }

  ctx->flags &= ~(CTX_FLAG_WANTWAIT | CTX_FLAG_LASTWAIT | CTX_FLAG_POLL_CALLED);
}

int flextcp_context_wait(struct flextcp_context *ctx, int timeout_ms)
{
  struct pollfd pfd;
  int ret;

  if (flextcp_context_canwait(ctx) != 0) {
    return -1;
  }

  pfd.fd = ctx->evfd;
  pfd.events = POLLIN;
  pfd.revents = 0;
  printf("sleeping on fd %d\n", pfd.fd);
  ret = poll(&pfd, 1, timeout_ms);
  if (ret < 0) {
    perror("flextcp_context_wait: poll returned error");
    return -1;
  }
  printf("woke up\n");

  flextcp_context_waitclear(ctx);
  return 0;
}
