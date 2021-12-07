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
#include <unistd.h>
#include <rte_config.h>
#include <stdlib.h>

#include <tas_memif.h>

#include "internal.h"
#include "fastemu.h"
#include "tcp_common.h"


static inline void inject_tcp_ts(void *buf, uint16_t len, uint32_t ts,
    struct network_buf_handle *nbh);

int fast_kernel_poll(struct dataplane_context *ctx,
    struct network_buf_handle *nbh, uint32_t ts)
{
  void *buf = network_buf_buf(nbh);
  struct flextcp_pl_appctx *kctx = &fp_state->kctx[ctx->id];
  struct flextcp_pl_ktx ktx;
  uint32_t flow_id, len;
  int ret = -1;

  /* stop if context is not in use */
  if (!kctx->enabled) {
    return -1;
  }

  int rv;
  if ((rv = rq_try_dequeue(kctx->tx, &ktx, sizeof(ktx))) != 0) {
    assert(rv > 0);
    return -1;
  }

  if (ktx.type == FLEXTCP_PL_KTX_PACKET) {
    len = ktx.msg.packet.len;

    /* Read transmit queue entry */
    dma_read(ktx.msg.packet.addr, len, buf);

    ret = 0;
    inject_tcp_ts(buf, len, ts, nbh);
    tx_send(ctx, nbh, 0, len);
  } else if (ktx.type == FLEXTCP_PL_KTX_PACKET_NOTS) {
    /* send packet without filling in timestamp */
    len = ktx.msg.packet.len;

    /* Read transmit queue entry */
    dma_read(ktx.msg.packet.addr, len, buf);

    ret = 0;
    tx_send(ctx, nbh, 0, len);
  } else if (ktx.type == FLEXTCP_PL_KTX_CONNRETRAN) {
    flow_id = ktx.msg.connretran.flow_id;
    if (flow_id >= FLEXNIC_PL_FLOWST_NUM) {
      fprintf(stderr, "fast_kernel_poll: invalid flow id=%u\n", flow_id);
      abort();
    }

    fast_flows_retransmit(ctx, flow_id);
    ret = 1;
  } else {
    fprintf(stderr, "fast_kernel_poll: unknown type: %u\n", ktx.type);
    abort();
  }
  return ret;
}

void fast_kernel_packet(struct dataplane_context *ctx,
    struct network_buf_handle *nbh)
{
  struct flextcp_pl_appctx *kctx = &fp_state->kctx[ctx->id];
  struct flextcp_pl_krx krx;
  uint16_t len;
  int rv;

  /* queue not initialized yet */
  if (!kctx->enabled) {
    return;
  }

  if ((rv = rq_try_reserve_flush(kctx->rx, sizeof(krx))) != 1) {
    assert(rv == 0);
    fprintf(stderr, "dropped packet\n");
    ctx->kernel_drop++;
    return;
  }

  len = network_buf_len(nbh);
  assert(kctx->rx->endpoints.tx.offset % sizeof(struct flextcp_pl_krx) == 0);
  struct flextcp_pl_krx* krx_ptr = rq_tx_tail(kctx->rx);
  krx.addr = krx_ptr->addr;
  dma_write(krx.addr, len, network_buf_bufoff(nbh));

  if (network_buf_flowgroup(nbh, &krx.msg.packet.flow_group)) {
    fprintf(stderr, "fast_kernel_packet: network_buf_flowgroup failed\n");
    abort();
  }

  krx.msg.packet.len = len;
  krx.msg.packet.fn_core = ctx->id;
  krx.type = FLEXTCP_PL_KRX_PACKET;

  MEM_BARRIER();

  if ((rv = rq_try_enqueue(kctx->rx, &krx, sizeof(krx))) != 0) {
    assert(rv < 0);
    abort();
  }

  if ((rv = rq_flush(kctx->rx)) < sizeof(krx)) {
    assert(rv < 0);
    abort();
  }

  MEM_BARRIER();

  notify_slowpath_core();
}

static inline void inject_tcp_ts(void *buf, uint16_t len, uint32_t ts,
    struct network_buf_handle *nbh)
{
  struct pkt_tcp *p = buf;
  struct tcp_opts opts;

  if (len < sizeof(*p) || f_beui16(p->eth.type) != ETH_TYPE_IP ||
      p->ip.proto != IP_PROTO_TCP)
  {
    return;
  }

  if (tcp_parse_options(buf, len, &opts) != 0) {
    fprintf(stderr, "inject_tcp_ts: parsing options failed\n");
    return;
  }

  if (opts.ts == NULL) {
    return;
  }

  opts.ts->ts_val = t_beui32(ts);

  fast_flows_kernelxsums(nbh, p);
}
