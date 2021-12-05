#ifndef HOST_NIC_H
#define HOST_NIC_H

#include <rdma_queue.h>
#include <rdma.h>
#include <tas_memif.h>

#define RC_FLAG_RECEIVED_BASES (1 << 0)
#define RC_FLAG_RECEIVED_INFO (1 << 1)

struct host_nic_context {
    uint64_t queue_size;
    struct rdma_send_bases bases;
    struct ibv_mr* tas_info_mr;
    int flags;
};

struct host_nic_context* allocate_host_nic_context(
    struct ibv_pd* pd, 
    struct ibv_qp* qp, 
    struct flexnic_info* tas_info,
    size_t tas_info_len,
    size_t sz
);

int deallocate_host_nic_context(struct host_nic_context* ctx);

#endif