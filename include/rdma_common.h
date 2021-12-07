#ifndef RDMA_COMMON_H
#define RDMA_COMMON_H

#include <rdma.h>
#include <rdma_queue.h>

int rdma_common_send_bases(
    struct rdma_connection* conn, 
    struct rdma_context* ctx,
    size_t shm_len);
int rdma_common_init();
int rdma_common_post_receive_buffer(struct rdma_connection* conn, struct rdma_msg* to_post);
int rdma_common_on_completion(struct ibv_comp_channel* comp_chn);
int rdma_common_create_context(struct rdma_context* context, struct ibv_context* verbs);
int rdma_common_create_qp(struct rdma_cm_id* id, struct rdma_context* context);
int rdma_common_reg_mem(struct rdma_connection* conn, struct rdma_context* context);
struct rdma_connection* rdma_common_build_connection(struct rdma_cm_id* id, struct rdma_context** context);
int rdma_common_allocate_queue(struct rdma_queue** queue, struct ibv_mr** mr, struct ibv_pd* pd, struct ibv_qp* qp, size_t sz);
int rdma_common_free_conn_context(struct rdma_connection* conn);

#endif