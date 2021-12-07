#include <rdma_common.h>
#include <tas_memif.h>
#include <stdlib.h>
#include <stdio.h>
#include <host_nic.h>
#include <sys/mman.h>

extern void* tas_shm;
extern struct flexnic_info* tas_info;

int rdma_common_send_bases(
    struct rdma_connection* conn, 
    struct rdma_context* ctx,
    size_t shm_len) {
    printf("sending tas_shm and tas_info bases\n");
    struct host_nic_context** hn_ctx_ptr = (struct host_nic_context**) &conn->context;
    size_t queue_size = getpagesize();
    if ((*hn_ctx_ptr = allocate_host_nic_context(
        ctx->prot_domain, 
        conn->qp,
        tas_info,
        FLEXNIC_INFO_BYTES,
        queue_size)) == NULL) {
        fprintf(stderr, "rdma_common_send_bases: failed to allocate host nic context\n");
        return -1;
    }
    struct host_nic_context* hn_ctx = *hn_ctx_ptr;

    struct rdma_msg msg;
    msg.type = RDMA_MSG_SEND_BASES;
    struct rdma_send_bases bases;
    bases.tas_info_opaque = (uint64_t) tas_info;
    bases.tas_info_rkey = hn_ctx->tas_info_mr->rkey;
    msg.data.bases = bases;
    conn->context_type = RCONN_TYPE_INTERFACE;    

    if (rdma_send_msg(conn, msg) != 0) {
        fprintf(stderr, "rdma_common_send_bases: failed to post send\n");
        return -1;
    }
    return 0;
}

int rdma_common_on_completion(
    struct ibv_comp_channel* comp_chn
) {

    struct ibv_cq* cq;
    struct ibv_wc wc;
    void* ctx;

    if (ibv_get_cq_event(comp_chn, &cq, &ctx) != 0) {
        fprintf(stderr, "rdma_common_on_completion: Failed to get completion event\n");
        return -1;
    }

    ibv_ack_cq_events(cq, 1);

    if (ibv_req_notify_cq(cq, 0) != 0) {
        fprintf(stderr, "rdma_common_on_completion: Could not rearm completion queue\n");
        return -1;
    }

    while (ibv_poll_cq(cq, 1, &wc)) {
        if (wc.status != IBV_WC_SUCCESS) {
            int is_recv = (wc.opcode & IBV_WC_RECV) != 0;
            int is_send = wc.opcode == IBV_WC_SEND;
            fprintf(stderr, "rdma_common_on_completion: status (%s) is not IBV_WC_SUCCESS (send: %d, recv: %d).\n",
                    ibv_wc_status_str(wc.status), is_send, is_recv);
            return -1;
        }


        struct rdma_buffer_addrs* addrs = (struct rdma_buffer_addrs*) wc.wr_id;
        int rv;
        if (wc.opcode & IBV_WC_RECV) {
            struct rdma_msg msg = *addrs->buffer;
            printf("rdma_common_on_completion: successful message: %d\n", msg.type);
            MEM_BARRIER();
            if (rdma_common_post_receive_buffer(addrs->conn, addrs->buffer) != 0) {
                fprintf(stderr, "rdma_common_on_completion: failed to post receive buffer\n");
                return -1;
            }

            if (wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
                int imm = ntohl(wc.imm_data);
                if (imm >= RDMA_IMM_LAST_VALUE) {
                    fprintf(stderr, "rdma_common_on_completion: received out of bounds immediate\n");
                    return -1;
                } else {
                    rdma_imm_handler handler = rdma_imm_handlers[imm];
                    if (handler != NULL) {
                        rv = (*handler)(addrs->conn);
                        if (rv != 0) {
                            return -1;
                        }
                    }
                }
            } else {
                int msg_type = (int) msg.type;
                if (msg_type >= RDMA_MSG_LAST_VALUE) {
                    fprintf(stderr, "rdma_common_on_completion: received out of bounds msg type\n");
                    return -1; 
                }
                rdma_recv_handler handler = rdma_recv_handlers[msg_type];
                if (handler != NULL) {
                    rv = (*handler)(addrs->conn, &msg);
                    if (rv != 0) {
                        return rv;
                    }
                }
            }
        }

        if (wc.opcode == IBV_WC_SEND) {
            int msg_type = (int) addrs->buffer->type;
            MEM_BARRIER();
            addrs->conn->n_to_send--;
            if (msg_type >= RDMA_MSG_LAST_VALUE) {
                fprintf(stderr, "rdma_common_on_completion: received out of bounds msg type\n");
                return -1;
            }
            rdma_send_handler handler = rdma_send_handlers[msg_type];
            if (handler != NULL) {
                rv = (*handler)(addrs->conn);
                if (rv != 0) {
                    return rv;
                }
            }
        }
        free(addrs);
    }
    return 0;
}

int rdma_common_post_receive_buffer(struct rdma_connection* conn, struct rdma_msg* to_post) {
    /* post receives */
    struct ibv_recv_wr wr;
    struct ibv_recv_wr* bad_wr = NULL;
    struct ibv_sge sge;

    struct rdma_buffer_addrs* addrs;
    addrs = malloc(sizeof(*addrs));
    addrs->buffer = to_post;
    addrs->conn = conn;
    wr.wr_id = (uintptr_t) addrs;
    wr.next = NULL;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    // printf("posting address %lx\n", (uintptr_t) to_post);
    sge.addr = (uintptr_t) to_post;
    sge.length = sizeof(struct rdma_msg);
    sge.lkey = conn->receive_buffer_mr->lkey;

    if (ibv_post_recv(conn->qp, &wr, &bad_wr) != 0) {
        perror("post_receive_buffer: failed to post receive");
        return -1;
    }

    return 0;
}

static int handle_recv_invalid(struct rdma_connection* conn, const struct rdma_msg* msg) {
    fprintf(stderr, "handle_recv_invalid: received invalid message\n");
    return -1;
}

static int handle_imm_invalid(struct rdma_connection* conn) {
    fprintf(stderr, "handle_imm_invalid: invalid immediate\n");
    return -1;
}

static int handle_send_invalid(struct rdma_connection* conn) {
    fprintf(stderr, "handle_send_invalid: sent invalid message\n");
    return -1;
}

int rdma_common_init() {
    rdma_recv_handlers = calloc(sizeof(rdma_recv_handler), RDMA_MSG_LAST_VALUE);
    rdma_imm_handlers = calloc(sizeof(rdma_imm_handler), RDMA_IMM_LAST_VALUE);
    rdma_send_handlers = calloc(sizeof(rdma_send_handler), RDMA_MSG_LAST_VALUE);

    if (rdma_recv_handlers == 0 || rdma_imm_handlers == 0) {
        fprintf(stderr, "rdmaif_init: failed to allocate handler tables\n");
        return -1;
    }
    if (add_recv_handler(RDMA_MSG_INVALID, handle_recv_invalid) != 0) {
        fprintf(stderr, "rdmaif_init: failed to register invalid recv rdma msg handler\n");
        return -1;
    }
    if (add_imm_handler(RDMA_IMM_INVALID, handle_imm_invalid) != 0) {
        fprintf(stderr, "rdmaif_init: failed to register invalid rdma imm handler\n");
        return -1;
    }
    if (add_send_handler(RDMA_IMM_INVALID, handle_send_invalid) != 0) {
        fprintf(stderr, "rdmaif_init: failed to register invalid send rdma msg handler\n");
    }
    return 0;
}

int rdma_common_create_context(struct rdma_context* context, struct ibv_context* verbs) {
    context->ctx = verbs;
    if ((context->prot_domain = ibv_alloc_pd(verbs)) == NULL) {
        fprintf(stderr, "rdma_common_create_context: Failed to allocate protection domain.\n");
        return -1;
    }
    if ((context->comp_chn = ibv_create_comp_channel(verbs)) == NULL) {
        fprintf(stderr, "rdma_common_create_context: Failed to create completion channel.\n");
        return -1;
    }
    if ((context->comp_queue = ibv_create_cq(verbs, 10, NULL, context->comp_chn, 0)) == NULL) {
        fprintf(stderr, "rdma_common_create_context: Failed to create completion queue.\n");
        return -1;
    }

    if (ibv_req_notify_cq(context->comp_queue, 0) != 0) {
        fprintf(stderr, "rdma_common_create_context: Failed to rearm comppletion queue\n");
        return -1;
    }

    return 0;
}

int rdma_common_create_qp(struct rdma_cm_id* id, struct rdma_context* ctx) {
    struct ibv_qp_init_attr qp_attr;
    memset(&qp_attr, 0, sizeof(struct ibv_qp_init_attr));
    qp_attr.send_cq = ctx->comp_queue;
    qp_attr.recv_cq = ctx->comp_queue;
    qp_attr.qp_type = IBV_QPT_RC;

    qp_attr.cap.max_send_wr = 10 + RC_MAX_OUTGOING;
    qp_attr.cap.max_recv_wr = 10 + RC_MAX_INGOING;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    /* create queue pair */
    if (rdma_create_qp(id, ctx->prot_domain, &qp_attr)) {
        fprintf(stderr, "rdma_common_create_qp: failed to create queue pair\n.");
        return -1;
    }

    return 0;
}

int rdma_common_reg_mem(struct rdma_connection* conn, struct rdma_context* ctx) {
    /* register receive buffer */
    assert(conn->receive_buffer != NULL);
    if ((conn->receive_buffer_mr = ibv_reg_mr(
        ctx->prot_domain,
        conn->receive_buffer,
        RC_MAX_INGOING * sizeof(struct rdma_msg),
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
    )) == 0) {
        fprintf(stderr, "rdma_common_reg_mem: failed to register receive buffer\n");
        return -1;
    }

    /* register send buffer */
    assert(conn->send_buffer != NULL);
    if ((conn->send_buffer_mr = ibv_reg_mr(
        ctx->prot_domain,
        conn->send_buffer,
        RC_MAX_OUTGOING * sizeof(struct rdma_msg),
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
    )) == 0) {
        fprintf(stderr, "rdma_common_reg_mem: failed to register send buffer\n");
        return -1;
    }
    return 0;
}

int rdma_common_build_context(struct rdma_cm_id* id, struct rdma_context** ctx) {
    /* build context */
    struct ibv_context* verbs = id->verbs;
    if (*ctx != NULL) {
        if ((*ctx)->ctx != verbs) {
            fprintf(stderr, "rdma_common_build_context: Events from multiple contexts unsupported. Limit to one device.\n");
            return -1;
        }
    } else {
        *ctx = malloc(sizeof(struct rdma_context));
        if (rdma_common_create_context(*ctx, verbs) != 0) {
            fprintf(stderr, "rdma_common_build_context: Failed to create context");
            return -1;
        }
    }
    return 0;
}

struct rdma_connection* rdma_common_build_connection(struct rdma_cm_id* id, struct rdma_context** ctx) {
    /* build context */
    int rv;
    if ((rv = rdma_common_build_context(id, ctx)) != 0) {
        return NULL;
    }

    /* create queue pair */
    if (rdma_common_create_qp(id, *ctx)) {
        fprintf(stderr, "on_connect_request: Failed to create queue pair\n.");
        return NULL;
    }

    /* fill out rdma connection */
    struct rdma_connection* conn;
    id->context = conn = (struct rdma_connection*)calloc(1, sizeof(struct rdma_connection));
    conn->qp = id->qp;
    conn->n_to_send = 0;
    conn->send_buffer = malloc(sizeof(struct rdma_msg) * RC_MAX_OUTGOING);
    conn->receive_buffer = malloc(sizeof(struct rdma_msg) * RC_MAX_INGOING);
    assert(conn->send_buffer != NULL);
    assert(conn->receive_buffer != NULL);

    /* register memory */
    if (rdma_common_reg_mem(conn, *ctx) != 0) {
        fprintf(stderr, "on_connect_request: Failed to register memory");
        return NULL;
    }

    /* post receives */
    for (int bi = 0; bi < RC_MAX_INGOING; bi++) {
        assert((uintptr_t) &conn->receive_buffer[bi] - (uintptr_t) conn->receive_buffer < RC_MAX_OUTGOING * sizeof(struct rdma_msg));
        if (rdma_common_post_receive_buffer(conn, &conn->receive_buffer[bi]) != 0) {
            fprintf(stderr, "on_connect_request: failed to post receive buffer\n");
            return NULL;
        }
    }

    // TODO: handle freeing resources on error

    return conn;
}

int rdma_common_allocate_queue(struct rdma_queue** queue, struct ibv_mr** mr, struct ibv_pd* pd, struct ibv_qp* qp, size_t sz) {
    void* queue_mem = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (queue_mem == NULL) {
        perror("rdma_common_allocate_queue: failed to allocate queue memory");
        return -1;
    }
    if (mr != NULL) {
        if ((*mr = ibv_reg_mr(
                pd, 
                queue_mem, 
                sz, 
                IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
            )) == NULL) 
        {
            fprintf(stderr, "rdma_common_allocate_queue: failed to register protected memory region\n");
            return -1;
        }
        if ((*queue = rq_allocate_in_place(
                queue_mem, 
                sz, 
                qp, 
                (*mr)->lkey, 
                (*mr)->rkey
            )) == NULL) 
        {
            fprintf(stderr, "rdma_common_allocate_queue: failed to allocate queue in place\n");
            return -1;
        }
    }

    return 0; 
}
int rdma_common_free_conn_context(struct rdma_connection* conn) {
    int rv = 0;
    if (conn->context != NULL) {
        switch(conn->context_type) {
            case RCONN_TYPE_INTERFACE:
                rv = deallocate_host_nic_context(conn->context);
                free(conn->context);
                break;
        }
    }
    return rv;
}