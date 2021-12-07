#ifndef RDMA_H_
#define RDMA_H_ 

#include <stdint.h>
#include <rdma/rdma_cma.h>
#include <rdma_queue.h>

struct rdma_context {
    struct ibv_context* ctx;
    struct ibv_pd* prot_domain;
    struct ibv_cq* comp_queue;
    struct ibv_comp_channel* comp_chn;
};

enum rdma_msg_type {
    RDMA_MSG_INVALID = 0,
    RDMA_MSG_SEND_BASES = 1,
    RDMA_MSG_APP_REG = 2,
    RDMA_MSG_ACK_APP_REG = 3,
    RDMA_MSG_REGISTER_CTX = 4,
    RDMA_MSG_ACK_CTX_REG = 5,
    RDMA_MSG_WAKEUP_TAS = 6,
    RDMA_MSG_WAKEUP_APP = 7,
    RDMA_MSG_LAST_VALUE = 8,
};

enum rdma_write_imm {
    RDMA_IMM_INVALID = 0,
    RDMA_IMM_TAS_INFO = 1,
    RDMA_IMM_SIGNAL = 2,
    RDMA_IMM_LAST_VALUE = 3,
};

struct rdma_app_register {
    char magic[4];
};

struct rdma_send_bases {
    uint64_t tas_info_opaque;
    uint32_t tas_info_rkey;
};

struct rdma_ack_app_register {
    uint64_t tas_shm_opaque;
    uint32_t tas_shm_rkey;
    uint32_t tas_shm_lkey;
    int success;
};

struct rdma_register_ctx {
    uintptr_t memq_base;
    uint32_t lkey;
    uint32_t rkey;
    uint64_t tx_len;
    uint64_t rx_len;
    int ctx_evfd;
};

struct rdma_ack_ctx_register {
    uint64_t memq_base; 
    uint32_t lkey;
    uint32_t rkey;
    uint16_t flexnic_db_id;
    uint16_t flexnic_qs_num;
};

struct rdma_wakeup_tas {
    int wakeup_slow;
    uint64_t wakeup_fast;
};

struct rdma_wakeup_app {
    int fd;
};

struct rdma_msg {
    union {
        struct rdma_send_bases bases;
        struct rdma_app_register app_reg;
        struct rdma_ack_app_register app_reg_ack;
        struct rdma_register_ctx ctx_reg;
        struct rdma_ack_ctx_register ctx_reg_ack;
        struct rdma_wakeup_tas wakeup_tas_info;
        struct rdma_wakeup_app wakeup_app_info;
    } data;
    int type;
};

struct rdma_buffer_addrs {
    struct rdma_connection* conn;
    struct rdma_msg* buffer;
};

enum rconn_type {
    RCONN_TYPE_INTERFACE,
    RCONN_TYPE_APP,
};

struct rdma_conn_mr {
    struct ibv_mr* mr;
    struct rdma_conn_mr* next;
};

#define RC_MAX_OUTGOING 100
#define RC_MAX_INGOING 100

struct rdma_connection {
    struct ibv_qp* qp;
    struct ibv_mr* receive_buffer_mr;
    struct ibv_mr* send_buffer_mr;
    struct rdma_msg* receive_buffer;
    size_t n_to_send;
    size_t send_off;
    struct rdma_msg* send_buffer;
    int context_type;  
    void* context;
};

typedef int (*rdma_recv_handler)(struct rdma_connection*, const struct rdma_msg*);
typedef int (*rdma_imm_handler)(struct rdma_connection*);
typedef int (*rdma_send_handler)(struct rdma_connection*);
extern rdma_recv_handler* rdma_recv_handlers;
extern rdma_imm_handler* rdma_imm_handlers;
extern rdma_send_handler* rdma_send_handlers;

int add_recv_handler(enum rdma_msg_type type, rdma_recv_handler handler);
int add_imm_handler(enum rdma_write_imm imm, rdma_imm_handler handler);
int add_send_handler(enum rdma_msg_type type, rdma_send_handler handler);

int rdma_send_msg(struct rdma_connection* conn, struct rdma_msg msg);
struct protected_address {
    int key;
    uintptr_t addr;
};
int rdma_write(struct ibv_qp* qp, struct protected_address from, struct protected_address to, size_t sz);
int rdma_read(struct ibv_qp* qp, struct protected_address from, struct protected_address to, size_t sz);
int rdma_write_with_imm(struct rdma_connection* conn, 
    enum rdma_write_imm, 
    struct protected_address from, 
    struct protected_address to, 
    size_t sz);
#endif