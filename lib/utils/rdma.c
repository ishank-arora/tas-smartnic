#include <rdma.h>
#include <stdlib.h>
#include <stdio.h>

rdma_recv_handler* rdma_recv_handlers;
rdma_imm_handler* rdma_imm_handlers;
rdma_send_handler* rdma_send_handlers;

int add_recv_handler(enum rdma_msg_type type, rdma_recv_handler handler) {
    if (type >= RDMA_MSG_LAST_VALUE) {
        return -1;
    }
    rdma_recv_handler* handler_addr = &rdma_recv_handlers[(int) type];
    if (*handler_addr != NULL) {
        return -1;
    }
    *handler_addr = handler;
    return 0;
}

int add_imm_handler(enum rdma_write_imm imm, rdma_imm_handler handler) {
    if (imm >= RDMA_IMM_LAST_VALUE) {
        return -1;
    }

    rdma_imm_handler* handler_addr = &rdma_imm_handlers[(int) imm];
    if (*handler_addr != 0) {
        return -1;
    }
    *handler_addr = handler;
    return 0;
}

int add_send_handler(enum rdma_msg_type type, rdma_send_handler handler) {
    if (type >= RDMA_MSG_LAST_VALUE) {
        return -1;
    }
    rdma_send_handler* handler_addr = &rdma_send_handlers[(int) type];
    if (*handler_addr != NULL) {
        return -1;
    }
    *handler_addr = handler;
    return 0;
}

int rdma_send_msg(struct rdma_connection* conn, struct rdma_msg msg) {
    struct ibv_send_wr wr, *bad_wr = NULL;
    struct ibv_sge sge;

    /* Send message of memory locations */
    sge.length = sizeof(struct rdma_msg);
    sge.lkey = conn->send_buffer_mr->lkey;
    struct rdma_msg* send_buffer;
    if (conn->n_to_send >= RC_MAX_OUTGOING) {
        return -1;
    }
    assert(conn->send_off < RC_MAX_OUTGOING);
    send_buffer = &conn->send_buffer[conn->send_off];
    *send_buffer = msg;
    conn->send_off++;
    if (conn->send_off >= RC_MAX_OUTGOING) {
        conn->send_off -= RC_MAX_OUTGOING;
    }
    conn->n_to_send++;
    assert((uintptr_t) conn->send_buffer_mr->addr <= (uintptr_t) send_buffer);
    assert((uintptr_t) conn->send_buffer_mr->addr + conn->send_buffer_mr->length >= (uintptr_t) send_buffer);
    printf("sending buffer at addr %lx, idx: %ld\n", (uintptr_t) send_buffer, conn->send_off);
    sge.addr = (uintptr_t) send_buffer;

    struct rdma_buffer_addrs* addrs;
    addrs = malloc(sizeof(*addrs));
    addrs->conn = conn;
    addrs->buffer = send_buffer; 

    memset(&wr, 0, sizeof(struct ibv_send_wr));
    wr.wr_id = (uintptr_t) addrs;
    wr.opcode = IBV_WR_SEND;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED;
    if (ibv_post_send(conn->qp, &wr, &bad_wr) != 0) {
        return -1;
    }

    return 0;
}

int rdma_write_with_imm(struct rdma_connection* conn, 
    enum rdma_write_imm imm, 
    struct protected_address from, 
    struct protected_address to, 
    size_t sz) 
{
    struct ibv_sge sge;
    struct ibv_send_wr wr;
    struct ibv_send_wr* bad_wr;

    struct rdma_buffer_addrs* addrs;
    addrs = malloc(sizeof(*addrs));
    addrs->conn = conn;
    addrs->buffer = NULL; 

    memset(&wr, 0, sizeof(wr));
    wr.wr_id = (uintptr_t) addrs;
    wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = to.addr;
    wr.wr.rdma.rkey = to.key;
    wr.imm_data = htonl(imm);

    sge.addr = from.addr;
    sge.length = sz;
    sge.lkey = from.key;

    if (ibv_post_send(conn->qp, &wr, &bad_wr) != 0) {
        fprintf(stderr, "rdma_write_with_imm: failed to write to address\n");
        return -1;
    }
    return 0;
}

int rdma_write(struct ibv_qp* qp, struct protected_address from, struct protected_address to, size_t sz) {
    struct ibv_sge sge;
    struct ibv_send_wr wr;
    struct ibv_send_wr* bad_wr;

    memset(&wr, 0, sizeof(wr));
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = to.addr;
    wr.wr.rdma.rkey = to.key;

    sge.addr = from.addr;
    sge.length = sz;
    sge.lkey = from.key;

    int rv;
    if ((rv = ibv_post_send(qp, &wr, &bad_wr)) != 0) {
        fprintf(stderr, "rdma_write: failed to write to address (%s)\n", strerror(rv));
        return rv;
    }
    return 0;
}

int rdma_read(struct ibv_qp* qp, struct protected_address from, struct protected_address to, size_t sz) {
    struct ibv_sge sge;
    struct ibv_send_wr wr;
    struct ibv_send_wr* bad_wr;

    memset(&wr, 0, sizeof(wr));
    wr.opcode = IBV_WR_RDMA_READ;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = to.addr;
    wr.wr.rdma.rkey = to.key;

    sge.addr = from.addr;
    sge.length = sz;
    sge.lkey = from.key;

    if (ibv_post_send(qp, &wr, &bad_wr) != 0) {
        fprintf(stderr, "rdma_read: failed to read from address\n");
        return -1;
    }
    return 0;
}
