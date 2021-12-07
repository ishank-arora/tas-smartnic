#include <host_nic.h>
#include <sys/mman.h>
#include <rdma_common.h>
#include <stdlib.h>
#include <stdio.h>
#include <rdma/rdma_cma.h>

struct host_nic_context* allocate_host_nic_context(
    struct ibv_pd* pd, 
    struct ibv_qp* qp, 
    struct flexnic_info* tas_info,
    size_t tas_info_len,
    size_t sz
) {
    struct host_nic_context* hn_ctx = (struct host_nic_context*) calloc(1, sizeof(struct host_nic_context));

    /* register tas_info */
    if ((hn_ctx->tas_info_mr = ibv_reg_mr(
        pd,
        tas_info,
        tas_info_len,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
    )) == 0) {
        fprintf(stderr, "allocate_host_nic_context: failed to register tas_info\n");
        return NULL;
    }

    return hn_ctx;
}

int deallocate_host_nic_context(struct host_nic_context* ctx) {
    ibv_dereg_mr(ctx->tas_info_mr);
    // Figure out why this leads to segfault
    return 0;
}