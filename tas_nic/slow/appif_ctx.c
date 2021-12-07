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

/**
 * @brief Handling application context kernel queues.
 * @file appif_ctx.c
 * @addtogroup tas-sp-appif
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <tas.h>
#include "internal.h"
#include "appif.h"

static int kin_conn_open(struct application *app, struct app_context *ctx,
    struct kernel_appout kin, struct kernel_appin *kout);
static int kin_conn_move(struct application *app, struct app_context *ctx,
    struct kernel_appout kin, struct kernel_appin *kout);
static int kin_conn_close(struct application *app, struct app_context *ctx,
    struct kernel_appout kin, struct kernel_appin *kout);
static int kin_listen_open(struct application *app, struct app_context *ctx,
    struct kernel_appout kin, struct kernel_appin *kout);
static int kin_accept_conn(struct application *app, struct app_context *ctx,
    struct kernel_appout kin, struct kernel_appin *kout);
static int kin_req_scale(struct application *app, struct app_context *ctx,
    struct kernel_appout kin, struct kernel_appin *kout);

static void appif_ctx_kick(struct app_context *ctx)
{
  notify_app_communicator(&ctx->ctx_comm, &ctx->last_ts);
}

static inline void enqueue_and_flush_kout(struct app_context* ctx, struct kernel_appin kout) {
  int rv;
  if ((rv = rq_try_enqueue(ctx->kout, &kout, sizeof(kout)) != 0)) {
    assert(rv < 0);
    fprintf(stderr, "enqueue_and_flush_kout: enqueue failed\n");
    abort();
  }

  if ((rv = rq_flush(ctx->kout)) < sizeof(kout)) {
    assert(rv < 0);
    fprintf(stderr, "enqueue_and_flush_kout: flush failed\n");
    abort();
  }
}

void appif_conn_opened(struct connection *c, int status)
{
  struct app_context *ctx = c->ctx;
  struct kernel_appin kout;
  int rv;
  if ((rv = rq_try_reserve_flush(ctx->kout, sizeof(kout))) != 1) {
    assert(rv == 0);
    fprintf(stderr, "appif_conn_opened: No space in kout queue (TODO)\n");
    return;
  }

  kout.data.conn_opened.opaque = c->opaque;
  kout.data.conn_opened.status = status;
  if (status == 0) {
    // TODO: send data on remote and local keys and receive data on remote and local keys
    assert((uintptr_t) c->rx == c->rx->endpoints.tx.addr);
    assert((uintptr_t) c->tx == c->tx->endpoints.rx.addr);
    kout.data.conn_opened.rx_off = (uintptr_t) c->rx - (uintptr_t) tas_shm;
    kout.data.conn_opened.tx_off = (uintptr_t) c->tx - (uintptr_t) tas_shm;

    kout.data.conn_opened.seq_rx = c->remote_seq;
    kout.data.conn_opened.seq_tx = c->local_seq;
    kout.data.conn_opened.local_ip = config.ip;
    kout.data.conn_opened.local_port = c->local_port;
    kout.data.conn_opened.flow_id = c->flow_id;
    kout.data.conn_opened.fn_core = c->fn_core;
  } else {
    tcp_destroy(c);
  }

  kout.type = KERNEL_APPIN_CONN_OPENED;

  /* make sure we have room for a response */
  enqueue_and_flush_kout(ctx, kout);

  MEM_BARRIER();

  printf("appif_conn_opened calls ctx_kick?\n");
  appif_ctx_kick(ctx);

}

void appif_conn_closed(struct connection *c, int status)
{
  struct app_context *ctx = c->ctx;
  struct application *app = ctx->app;
  struct connection *c_i;
  struct kernel_appin kout;

  int rv;
  if ((rv = rq_try_reserve_flush(ctx->kout, sizeof(kout))) != 1) {
    assert(rv == 0);
    fprintf(stderr, "appif_conn_closed: No space in kout queue (TODO)\n");
    return;
  }

  kout.data.status.opaque = c->opaque;
  kout.data.status.status = status;
  kout.type = KERNEL_APPIN_STATUS_CONN_CLOSE;
  /* make sure we have room for a response */
  enqueue_and_flush_kout(ctx, kout);
  MEM_BARRIER();

  printf("appif_conn_closed calls ctx_kick?\n");
  appif_ctx_kick(ctx);

  /* remove from app connection list */
  if (app->conns == c) {
    app->conns = c->app_next;
  } else {
    for (c_i = app->conns; c_i != NULL && c_i->app_next != c;
        c_i = c_i->app_next);
    if (c_i == NULL) {
      fprintf(stderr, "appif_conn_closed: connection not found\n");
      abort();
    }
    c_i->app_next = c->app_next;
  }
}

void appif_listen_newconn(struct listener *l, uint32_t remote_ip,
    uint16_t remote_port)
{
  struct app_context *ctx = l->ctx;
  struct kernel_appin kout;

  int rv;
  if ((rv = rq_try_reserve_flush(ctx->kout, sizeof(kout))) != 1) {
    assert(rv == 0);
    fprintf(stderr, "appif_listen_newconn: No space in kout queue (TODO)\n");
    return;
  }

  kout.data.listen_newconn.opaque = l->opaque;
  kout.data.listen_newconn.remote_ip = remote_ip;
  kout.data.listen_newconn.remote_port = remote_port;
  kout.type = KERNEL_APPIN_LISTEN_NEWCONN;
  enqueue_and_flush_kout(ctx, kout);
  MEM_BARRIER();

  printf("appif_listen_newconn calls ctx_kick?\n");
  appif_ctx_kick(ctx);
}

void appif_accept_conn(struct connection *c, int status)
{
  struct app_context *ctx = c->ctx;
  struct application *app = ctx->app;
  struct kernel_appin kout;

  int rv;
  if ((rv = rq_try_reserve_flush(ctx->kout, sizeof(kout))) != 1) {
    assert(rv == 0);
    fprintf(stderr, "appif_accept_conn: No space in kout queue (TODO)\n");
  }

  kout.data.accept_connection.opaque = c->opaque;
  kout.data.accept_connection.status = status;
  if (status == 0) {
    assert((uintptr_t) c->rx == c->rx->endpoints.tx.addr);
    assert((uintptr_t) c->tx == c->tx->endpoints.rx.addr);
    kout.data.accept_connection.rx_off = (uintptr_t) c->rx - (uintptr_t) tas_shm;
    kout.data.accept_connection.tx_off = (uintptr_t) c->tx - (uintptr_t) tas_shm;

    kout.data.accept_connection.seq_rx = c->remote_seq;
    kout.data.accept_connection.seq_tx = c->local_seq;
    kout.data.accept_connection.local_ip = config.ip;
    kout.data.accept_connection.remote_ip = c->remote_ip;
    kout.data.accept_connection.remote_port = c->remote_port;
    kout.data.accept_connection.flow_id = c->flow_id;
    kout.data.accept_connection.fn_core = c->fn_core;

    c->app_next = app->conns;
    app->conns = c;
  } else {
    tcp_destroy(c);
  }

  kout.type = KERNEL_APPIN_ACCEPTED_CONN;
  enqueue_and_flush_kout(ctx, kout);
  MEM_BARRIER();

  printf("appif_accept_conn calls ctx_kick?\n");
  appif_ctx_kick(ctx);
}


unsigned appif_ctx_poll(struct application *app, struct app_context *ctx)
{
  struct kernel_appout kin;
  struct kernel_appin kout;
  uint8_t type;
  int kout_inc = 0;
  int rv;

  if (rq_nbytes_empty(ctx->kout) < sizeof(kout)) {
    fprintf(stderr, "kin_poll: No space in kout queue (TODO)\n");
    return 0;
  }

  if ((rv = rq_try_dequeue(ctx->kin, &kin, sizeof(kin))) != 0) {
    assert(rv > 0);
    return 0;
  }

  type = kin.type;
  MEM_BARRIER();

  switch (type) {
    case KERNEL_APPOUT_CONN_OPEN:
      /* connection request */
      kout_inc = kin_conn_open(app, ctx, kin, &kout);
      break;

    case KERNEL_APPOUT_CONN_MOVE:
      /* connection move request */
      kout_inc = kin_conn_move(app, ctx, kin, &kout);
      break;

    case KERNEL_APPOUT_CONN_CLOSE:
      /* connection close request */
      kout_inc = kin_conn_close(app, ctx, kin, &kout);
      break;

    case KERNEL_APPOUT_LISTEN_OPEN:
      /* listen request */
      kout_inc = kin_listen_open(app, ctx, kin, &kout);
      break;

    case KERNEL_APPOUT_ACCEPT_CONN:
      /* accept request */
      kout_inc = kin_accept_conn(app, ctx, kin, &kout);
      break;

    case KERNEL_APPOUT_REQ_SCALE:
      /* scaling request */
      kout_inc = kin_req_scale(app, ctx, kin, &kout);
      break;

    case KERNEL_APPOUT_LISTEN_CLOSE:
    default:
      fprintf(stderr, "kin_poll: unsupported request type %u\n", kin.type);
      break;
  }

  /* update kout queue position if the entry was used */
  if (kout_inc != 0) {
    int rv;
    if ((rv = rq_try_reserve_flush(ctx->kout, sizeof(kout))) != 1) {
      assert(rv == 0);
      fprintf(stderr, "appif_listen_newconn: No space in kout queue (TODO)\n");
      abort();
    }
    enqueue_and_flush_kout(ctx, kout);
    MEM_BARRIER();

    printf("appif_ctx_poll calls ctx_kick?\n");
    appif_ctx_kick(ctx);
  }

  return 1;
}

static int kin_conn_open(struct application *app, struct app_context *ctx,
    struct kernel_appout kin, struct kernel_appin *kout)
{
  struct connection *conn;

  if (tcp_open(ctx, kin.data.conn_open, ctx->doorbell->id, &conn) != 0)
  {
    fprintf(stderr, "kin_conn_open: tcp_open failed\n");
    goto error;
  }

  conn->app_next = app->conns;
  app->conns = conn;

  return 0;

error:
  kout->data.conn_opened.opaque = kin.data.conn_open.opaque;
  kout->data.conn_opened.status = -1;
  kout->type = KERNEL_APPIN_CONN_OPENED;
  return 1;
}

static int kin_conn_move(struct application *app, struct app_context *ctx,
    struct kernel_appout kin, struct kernel_appin *kout)
{
  struct connection *conn;
  struct app_context *new_ctx;

  for (conn = app->conns; conn != NULL; conn = conn->app_next) {
    if (conn->local_ip == kin.data.conn_move.local_ip &&
        conn->remote_ip == kin.data.conn_move.remote_ip &&
        conn->local_port == kin.data.conn_move.local_port &&
        conn->remote_port == kin.data.conn_move.remote_port &&
        conn->opaque == kin.data.conn_move.opaque)
    {
      break;
    }
  }
  if (conn == NULL) {
    fprintf(stderr, "kin_conn_move: connection not found\n");
    goto error;
  }

  for (new_ctx = app->contexts; new_ctx != NULL; new_ctx = new_ctx->next) {
    if (new_ctx->doorbell->id == kin.data.conn_move.db_id) {
      break;
    }
  }
  if (new_ctx == NULL) {
    fprintf(stderr, "kin_conn_move: destination context not found\n");
    goto error;
  }

  if (conn->status != CONN_OPEN) {
    fprintf(stderr, "kin_conn_move: connection not open\n");
    goto error;
  }

  if (nicif_connection_move(new_ctx->doorbell->id, conn->flow_id) != 0) {
    fprintf(stderr, "kin_conn_move: nicif_connection_move failed\n");
    goto error;
  }

  kout->data.status.opaque = kin.data.conn_move.opaque;
  kout->data.status.status = 0;
  kout->type = KERNEL_APPIN_STATUS_CONN_MOVE;
  return 1;

error:
  kout->data.status.opaque = kin.data.conn_move.opaque;
  kout->data.status.status = -1;
  kout->type = KERNEL_APPIN_STATUS_CONN_MOVE;
  return 1;
}

static int kin_conn_close(struct application *app, struct app_context *ctx,
    struct kernel_appout kin, struct kernel_appin *kout)
{
  struct connection *conn;

  for (conn = app->conns; conn != NULL; conn = conn->app_next) {
    if (conn->local_ip == kin.data.conn_close.local_ip &&
        conn->remote_ip == kin.data.conn_close.remote_ip &&
        conn->local_port == kin.data.conn_close.local_port &&
        conn->remote_port == kin.data.conn_close.remote_port &&
        conn->opaque == kin.data.conn_close.opaque)
    {
      break;
    }
  }
  if (conn == NULL) {
    fprintf(stderr, "kin_conn_close: connection not found\n");
    goto error;
  }

  if (tcp_close(conn) != 0) {
    fprintf(stderr, "kin_conn_close: tcp_close failed\n");
    goto error;
  }

  return 0;

error:
  kout->data.status.opaque = kin.data.conn_close.opaque;
  kout->data.status.status = -1;
  kout->type = KERNEL_APPIN_STATUS_CONN_CLOSE;
  return 1;
}

static int kin_listen_open(struct application *app, struct app_context *ctx,
    struct kernel_appout kin, struct kernel_appin *kout)
{
  struct listener *listen;

  if (tcp_listen(ctx, kin.data.listen_open.opaque,
        kin.data.listen_open.local_port, kin.data.listen_open.backlog,
        !!(kin.data.listen_open.flags & KERNEL_APPOUT_LISTEN_REUSEPORT),
        &listen) != 0)
  {
    fprintf(stderr, "kin_listen_open: tcp_listen failed\n");
    goto error;
  }

  listen->app_next = app->listeners;
  app->listeners = listen;

  kout->data.status.opaque = kin.data.listen_open.opaque;
  kout->data.status.status = 0;
  kout->type = KERNEL_APPIN_STATUS_LISTEN_OPEN;
  return 1;

error:
  kout->data.status.opaque = kin.data.listen_open.opaque;
  kout->data.status.status = -1;
  kout->type = KERNEL_APPIN_STATUS_LISTEN_OPEN;
  return 1;
}

static int kin_accept_conn(struct application *app, struct app_context *ctx,
    struct kernel_appout kin, struct kernel_appin *kout)
{
  struct listener *listen;

  /* look for listen struct */
  for (listen = app->listeners; listen != NULL; listen = listen->app_next) {
    if (listen->port == kin.data.accept_conn.local_port &&
        listen->opaque == kin.data.accept_conn.listen_opaque)
    {
      break;
    }
  }

  if (tcp_accept(ctx, kin.data.accept_conn, listen,
        ctx->doorbell->id) != 0)
  {
    fprintf(stderr, "kin_accept_conn\n");
    goto error;
  }

  return 0;

error:
  kout->data.accept_connection.opaque = kin.data.accept_conn.conn_opaque;
  kout->data.accept_connection.status = -1;
  MEM_BARRIER();
  kout->type = KERNEL_APPIN_ACCEPTED_CONN;

  printf("kin_accept_connect calls ctx_kick?\n");
  appif_ctx_kick(ctx);
  return 1;
}

extern int flexnic_scale_to(uint32_t cores);

static int kin_req_scale(struct application *app, struct app_context *ctx,
    struct kernel_appout kin, struct kernel_appin *kout)
{
  uint32_t num_cores = kin.data.req_scale.num_cores;

  flexnic_scale_to(num_cores);

  return 0;
}
