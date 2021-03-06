/*
 * Copyright 2020 University of Washington, Max Planck Institute for
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
#include <assert.h>
#include <unistd.h>

#include <tas.h>
#include <communicator.h>
#include <rdma.h>

extern int kernel_notifyfd;
extern struct rdma_connection* tas_host_conn;

static inline void notify_communicator(struct communicator* comm, uint64_t *last_ts, uint64_t tsc,
    uint64_t delta) 
{
  uint64_t val;
  /* blocking is disabled */
  if (delta == UINT64_MAX) {
    return;
  }

  if(tsc - *last_ts > delta) {
    switch (comm->type) {
      case CT_LOCAL: {
        val = 1;
        if (write(comm->method.fd, &val, sizeof(uint64_t)) != sizeof(uint64_t)) {
          perror("notify_communicator: write failed");
          abort();
        }
        break;
      }
      case CT_REMOTE: {
        // TODO: implement
        struct rdma_msg msg;
        msg.type = RDMA_MSG_WAKEUP_APP;
        msg.data.wakeup_app_info.fd = comm->method.remote_fd.fd;
        int rv = rdma_send_msg(tas_host_conn, msg);
        assert(rv == 0);
        break;
      }
    }
  }

  *last_ts = tsc;
}

static void notify_core(int cfd, uint64_t *last_ts, uint64_t tsc,
    uint64_t delta)
{
  struct communicator comm;
  comm.type = CT_LOCAL;
  comm.method.fd = cfd;
  notify_communicator(&comm, last_ts, tsc, delta);
}

void notify_fastpath_core(unsigned core)
{
  printf("notify_fp_core called\n");
  notify_communicator(&fp_state->kctx[core].ctx_comm, &fp_state->kctx[core].last_ts,
      util_rdtsc(), tas_info->poll_cycle_tas);
}

void notify_app_communicator(struct communicator* comm, uint64_t *last_ts)
{
  printf("notify_app_communicator called\n");
  notify_communicator(comm, last_ts, util_rdtsc(), tas_info->poll_cycle_app);
}

void notify_appctx(struct flextcp_pl_appctx *ctx, uint64_t tsc)
{
  printf("notify_appctx called\n");
  notify_communicator(&ctx->ctx_comm, &ctx->last_ts, tsc, tas_info->poll_cycle_app);
}

void notify_slowpath_core(void)
{
  static uint64_t __thread last_ts = 0;
  notify_core(kernel_notifyfd, &last_ts, util_rdtsc(),
      tas_info->poll_cycle_tas);
}

int notify_canblock(struct notify_blockstate *nbs, int had_data, uint64_t tsc)
{
  if (tas_info->poll_cycle_tas == UINT64_MAX) {
    return 0;
  }

  if (had_data) {
    /* not idle this round, reset everything */
    nbs->can_block = nbs->second_bar = 0;
    nbs->last_active_ts = tsc;
  } else if (nbs->second_bar) {
    /* we can block now, reset afterwards */
    nbs->can_block = nbs->second_bar = 0;
    nbs->last_active_ts = tsc;
    return 1;
  } else if (nbs->can_block &&
      tsc - nbs->last_active_ts > tas_info->poll_cycle_tas)
  {
    /* we've reached the poll cycle interval, so just poll once more */
    nbs->second_bar = 1;
  } else {
    /* waiting for poll cycle interval */
    nbs->can_block = 1;
  }

  return 0;
}

void notify_canblock_reset(struct notify_blockstate *nbs)
{
  nbs->can_block = nbs->second_bar = 0;
}
