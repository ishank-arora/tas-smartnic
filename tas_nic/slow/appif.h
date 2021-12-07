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

#ifndef APPIF_H_
#define APPIF_H_

#include <stdbool.h>
#include <stdint.h>

#include "internal.h"
#include <kernel_appif.h>

#include <rdma.h>
#include <rdma_queue.h>
#include <communicator.h>

struct app_doorbell {
  uint32_t id;
  /* only for freelist */
  struct app_doorbell *next;
};

struct app_context {
  struct application *app;
  struct rdma_queue* kin;
  struct rdma_queue* kout;

  struct app_doorbell *doorbell;

  int ready;
  struct communicator ctx_comm; // communicator to context
  uint64_t last_ts;
  struct app_context *next;

  union {
    struct {
      struct packetmem_handle *kin_handle;
      struct packetmem_handle *kout_handle;
      struct {
        struct packetmem_handle *rxq;
        struct packetmem_handle *txq;
      } handles[];
    } handles;
    int _dummy;
  };
};

struct application {
  struct rdma_connection* rdma_conn;
  struct communicator comm;
  struct nbqueue_el nqe;
  size_t req_rx;
  struct kernel_uxsock_request req;
  size_t resp_sz;
  union {
    struct kernel_uxsock_response *resp;
    struct {
      void* base;
      int app_out_len;
      int app_in_len;
      struct ibv_mr* mr;
    } queues;
  } buffer;

  struct app_context *contexts;
  struct application *next;
  struct app_context *need_reg_ctx;
  struct app_context *need_reg_ctx_done;

  struct connection *conns;
  struct listener   *listeners;

  struct nicif_completion comp;

  uint16_t id;
  volatile bool closed;
};

static inline struct rdma_queue* app_get_kin(struct application* app) {
  switch (app->comm.type) {
    case CT_LOCAL:
      return (struct rdma_queue*) ((uintptr_t) tas_shm + app->buffer.resp->app_in_off);
    case CT_REMOTE:
      return (struct rdma_queue*) app->buffer.queues.base;
  }
  fprintf(stderr, "app_get_kin: unreachable\n");
  abort();
  return NULL;
}

static inline struct rdma_queue* app_get_kout(struct application* app) {
  switch (app->comm.type) {
    case CT_LOCAL: {
        return (struct rdma_queue*) ((uintptr_t) tas_shm + app->buffer.resp->app_in_off);
    }
    case CT_REMOTE: {
      struct rdma_queue* kin = app_get_kin(app);
      return (struct rdma_queue*) ((uintptr_t) kin + sizeof(struct rdma_queue) + kin->buffer_size); 
    }
  }
  fprintf(stderr, "app_get_kout: unreachable\n");
  abort();
  return NULL;
}

static inline struct rdma_queue* app_get_flexnic_rq(struct application* app, int q) {
  switch (app->comm.type) {
    case CT_LOCAL: {
      return (struct rdma_queue*) ((uintptr_t) tas_shm + app->buffer.resp->flexnic_qs[q].rxq_off);
    }
    case CT_REMOTE: {
      struct rdma_queue* kout = app_get_kout(app);
      int skip_sz = 2 * sizeof(struct rdma_queue) + app->buffer.queues.app_in_len + app->buffer.queues.app_out_len;
      return (struct rdma_queue*) ((uintptr_t) kout + sizeof(struct rdma_queue) + kout->buffer_size + skip_sz * q);
    }
  }
  fprintf(stderr, "app_get_flexnic_rq: unreachable\n");
  abort();
  return NULL;
}

static inline struct rdma_queue* app_get_flexnic_tq(struct application* app, int q) {
  switch (app->comm.type) {
    case CT_LOCAL: {
      return (struct rdma_queue*) ((uintptr_t) tas_shm + app->buffer.resp->flexnic_qs[q].txq_off);
    }
    case CT_REMOTE: {
      struct rdma_queue* rq = app_get_flexnic_rq(app, q);
      int offset = sizeof(struct rdma_queue) + rq->buffer_size;
      return (struct rdma_queue*) ((uintptr_t) rq + offset);
    }
  }
  fprintf(stderr, "app_get_flexnic_tq: unreachable\n");
  abort();
  return NULL;
}

/**
 * Poll kernel->app context queue.
 *
 * @param app Application to poll
 * @param ctx Context to poll
 */
unsigned appif_ctx_poll(struct application *app, struct app_context *ctx);

struct application* allocate_application(struct communicator comm);
struct app_context* allocate_remote_app_context(
  struct application *app, 
  struct communicator ctx_comm,
  void* base,
  size_t app_in_len,
  size_t app_out_len,
  struct ibv_mr* mr
);

#endif /* ndef APPIF_H_ */
