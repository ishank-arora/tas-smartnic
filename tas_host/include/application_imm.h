#ifndef APPLICATION_IMM_H_
#define APPLICATION_IMM_H_

#include <kernel_appif.h>
#include <packetmem.h>
#include <rdma_queue.h>

struct application_imm {
    int fd;
    size_t req_rx;
    struct kernel_uxsock_request req;
    struct application_imm* next;
};

struct app_ctx_imm {
    struct application_imm* app;
    struct packetmem_handle* kin_handle;
    struct rdma_queue* kin;

    struct packetmem_handle *kout_handle;
    struct rdma_queue* kout;

    struct app_ctx_imm *next;

    struct {
        struct packetmem_handle *rxq;
        struct packetmem_handle *txq;
    } handles[]; 
};

#endif