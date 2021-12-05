#include <tas.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <assert.h>
#include <rdma_queue.h>
#include <rdma.h>
#include <rdma_common.h>
#include <host_nic.h>
#include <pthread.h>
#include <fastpath.h>
#include "appif.h"

/* guide here: https://www.hpcadvisorycouncil.com/pdf/building-an-rdma-capable-application-with-ib-verbs.pdf*/
static int epfd;
static pthread_t pt_rdmaif;

static struct rdma_cm_id* listener = NULL;
static struct rdma_event_channel* event_chn = NULL;
struct rdma_context* tas_ctx;
struct rdma_connection* tas_host_conn;

struct ibv_mr* tas_shm_mr = NULL;
struct ibv_mr* tas_info_mr = NULL;

static int on_connect_request(struct rdma_cm_id* id);
static int on_connection(void* context);
static int on_disconnect(struct rdma_cm_id* id);

extern int kernel_notifyfd;

int on_rdma_event() {
    struct epoll_event event[2];
    int n = epoll_wait(epfd, event, 2, 0);
    int i, rv;
    
    for (i = 0; i < n; i++) {
        int fd = event[i].data.fd;
        if (fd == event_chn->fd) {
            struct rdma_cm_event *event = NULL;
            rdma_get_cm_event(event_chn, &event);
            struct rdma_cm_event event_copy = *event;
            rdma_ack_cm_event(event);
            switch (event_copy.event) {
                case RDMA_CM_EVENT_CONNECT_REQUEST:
                    printf("on connection request\n");
                    rv = on_connect_request(event_copy.id);
                    break;
                case RDMA_CM_EVENT_ESTABLISHED:
                    printf("on event established\n");
                    rv = on_connection(event_copy.id->context);
                    break;
                case RDMA_CM_EVENT_DISCONNECTED:
                    printf("on event disconnected\n");
                    rv = on_disconnect(event_copy.id);
                    break;
                default:
                    rv = -1;
                    fprintf(stderr, "on_rdma_event: received %s\n", rdma_event_str(event_copy.event));
                    break;
            }
        } else if (tas_ctx->comp_chn != NULL && fd == tas_ctx->comp_chn->fd) {
            rv = rdma_common_on_completion(tas_ctx->comp_chn);
        } else {
            rv = -1;
            fprintf(stderr, "on_rdma_event: got event from unknown file descriptor\n");
        } 

        if (rv != 0) {
            fprintf(stderr, "Got bad return value; cleaning up\n");
            rdma_cleanup();
            break;
        }
    }
    return rv;
}

static int on_connect_request(struct rdma_cm_id* id) {    
    int add_to_epoll = tas_ctx == NULL;

    /* build connection*/
    if (rdma_common_build_connection(id, &tas_ctx) == NULL) {
        fprintf(stderr, "on_connect_request: failed to build connection\n");
        return -1;
    }

    if (add_to_epoll) {
        {
            struct epoll_event ev = {
            .events = EPOLLIN,
            .data.fd = tas_ctx->comp_chn->fd,
            };
            int r = epoll_ctl(epfd, EPOLL_CTL_ADD, tas_ctx->comp_chn->fd, &ev);
            assert(r == 0);
        }
    }

    struct rdma_conn_param cm_params;
    memset(&cm_params, 0, sizeof(struct rdma_conn_param));
    if (rdma_accept(id, &cm_params) != 0) {
        fprintf(stderr, "on_connect_request: failed to accept connection\n");
        return -1;
    }

    return 0;
}

static int on_connection(void* context) {
    /* register tas_shm */    
    if ((tas_shm_mr = ibv_reg_mr(
            tas_ctx->prot_domain, 
            tas_shm,
            config.shm_len, 
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ)) == 0) {
        fprintf(stderr, "on_connection: failed to register tas_shm\n");
        return -1;
    }
    /* register tas_info */
    if ((tas_info_mr = ibv_reg_mr(
        tas_ctx->prot_domain,
        tas_info,
        FLEXNIC_INFO_BYTES,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
    )) == 0) {
        fprintf(stderr, "on_connection: failed to register tas_info\n");
        return -1;
    }
    return 0;
}

static int on_disconnect(struct rdma_cm_id* id) {
    struct rdma_connection *conn = (struct rdma_connection*) id->context;
    printf("peer disconnected.\n");
    rdma_destroy_qp(id);
    // TODO: why does deregistering memory seg fault?
    // ibv_dereg_mr(conn->receive_buffer_mr);
    // ibv_dereg_mr(conn->send_buffer_mr);
    // rdma_destroy_id(id);

    rdma_common_free_conn_context(conn);
    free(conn->receive_buffer);
    free(conn->send_buffer);
    free(conn);
    return 0;
}

static int handle_send_bases(struct rdma_connection* conn) {
    assert(conn->context_type == RCONN_TYPE_INTERFACE);
    struct host_nic_context* hn_ctx = (struct host_nic_context*) conn->context;
    tas_host_conn = conn;

    /* write tas info to host */
    printf("writing tas_info\n");
    assert(strcmp(tas_info->magic, FLEXNIC_MAGIC_STR) == 0);

    struct protected_address from;
    struct protected_address to;
    from.addr = (uintptr_t) tas_info;
    from.key = hn_ctx->tas_info_mr->lkey;
    to.addr = hn_ctx->bases.tas_info_opaque;
    to.key = hn_ctx->bases.tas_info_rkey;
    if ((rdma_write_with_imm(conn, RDMA_IMM_TAS_INFO, from, to, FLEXNIC_INFO_BYTES)) != 0) {
        fprintf(stderr, "handle_send_bases: failed to write tas info.\n");
        return -1;
    }

    return 0;
}

static int handle_recv_bases(struct rdma_connection* conn, const struct rdma_msg* msg) {    
    if (rdma_common_send_bases(conn, tas_ctx, config.shm_len) != 0) {
        fprintf(stderr, "handle_recv_bases: Failed to send bases.\n");
        return -1;
    }

    printf("received tas_shm and tas_info bases\n");
    struct host_nic_context* hn_ctx = (struct host_nic_context*) conn->context;
    hn_ctx->bases = msg->data.bases;
    hn_ctx->flags |= RC_FLAG_RECEIVED_BASES;

    return 0;
}

static int handle_recv_app_reg(struct rdma_connection* conn, const struct rdma_msg* msg) {
    assert(strcmp(msg->data.app_reg.magic, FLEXNIC_MAGIC_STR) == 0);
    struct communicator comm;
    comm.type = CT_REMOTE;
    comm.method.remote_fd.conn = conn;
    comm.method.remote_fd.fd = -1;
    struct application* app = allocate_application(comm);
    conn->context_type = RCONN_TYPE_APP;
    struct rdma_msg to_send;
    to_send.type = RDMA_MSG_ACK_APP_REG;
    if (app != NULL) {
        conn->context = app;
        app->rdma_conn = conn;
    }
    to_send.data.app_reg_ack.success = app != NULL;
    to_send.data.app_reg_ack.tas_shm_opaque = (uintptr_t) tas_shm;
    to_send.data.app_reg_ack.tas_shm_lkey = (uint32_t) tas_info_mr->lkey;
    to_send.data.app_reg_ack.tas_shm_rkey = (uint32_t) tas_info_mr->rkey;
    if (rdma_send_msg(conn, to_send) != 0) {
        fprintf(stderr, "handle_recv_app_reg: failed to send ack\n");
        abort();
    }
    return 0;
}

static int handle_recv_wakeup(struct rdma_connection* conn, const struct rdma_msg* msg) {
    assert(conn->context_type == RCONN_TYPE_INTERFACE);
    if (msg->data.wakeup_tas_info.wakeup_slow) {
        uint64_t val = 1;
        int r = write(kernel_notifyfd, &val, sizeof(uint64_t));
        if (r != 0) {
            perror("handle_recv_wakeup: failed to notify kernel");
            return -1;
        }
    }

    uint64_t wakeup_fast = msg->data.wakeup_tas_info.wakeup_fast;
    int to_wakeup = 0;
    while (wakeup_fast > 0) {
        if (to_wakeup > tas_info->cores_num) {
            fprintf(stderr, "handle_imm_signal: attempting to wakeup nonexistent core\n");
            break;
        }

        if ((wakeup_fast & 0b1) != 0) {
            uint64_t val = 1;
            int r = write(ctxs[to_wakeup]->evfd, &val, sizeof(uint64_t));
            if (r != 0) {
                fprintf(stderr, "handle_recv_wakeup: failed to notify fp core %d: %s\n", 
                    to_wakeup, strerror(errno));
                return -1;
            }
        }
        to_wakeup++;
        wakeup_fast >>= 1;
    }
    return 0;
}

static int handle_imm_signal(struct rdma_connection* conn) {
    assert(conn->context_type == RCONN_TYPE_INTERFACE);
    // TODO: impl for applications
    return 0;
}

static int handle_recv_reg_ctx(struct rdma_connection* conn, const struct rdma_msg* msg) {
    assert(conn->context_type == RCONN_TYPE_APP);
    /* allocate memory queues */
    size_t kin_qsize = config.app_kin_len + sizeof(struct rdma_queue*);
    size_t kout_qsize = config.app_kout_len + sizeof(struct rdma_queue*);
    uint64_t nic_rx_len = msg->data.ctx_reg.rx_len + sizeof(struct rdma_queue);
    uint64_t nic_tx_len = msg->data.ctx_reg.tx_len + sizeof(struct rdma_queue);
    uint64_t bytes_needed = kin_qsize + kout_qsize + (nic_rx_len + nic_tx_len) * tas_info->cores_num;
    void* ctx_buffer = malloc(bytes_needed);
    uint64_t app_base_opaque = msg->data.ctx_reg.memq_base;
    /* register ctx buffer */
    struct ibv_mr* ctx_buffer_mr;
    if ((ctx_buffer_mr = ibv_reg_mr(
        tas_ctx->prot_domain,
        ctx_buffer,
        bytes_needed,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ
    )) == 0) {
        fprintf(stderr, "handle_recv_reg_ctx: failed to register ctx buffer\n");
        return -1;
    }
    /* allocate buffers */
    int offset = 0;
    int rv;
    struct rdma_queue* q;
    uintptr_t ctx_buffer_opaque = (uintptr_t) ctx_buffer;
    struct rdma_queue_endpoint rx_endpoint;
    rx_endpoint.addr = app_base_opaque;
    rx_endpoint.offset = 0;
    rx_endpoint.lkey = ctx_buffer_mr->lkey;
    rx_endpoint.rkey = ctx_buffer_mr->rkey;

    q = rq_allocate_in_place((void*) (ctx_buffer_opaque + offset), kin_qsize, conn->qp, ctx_buffer_mr->lkey, ctx_buffer_mr->rkey);
    rv = rq_pair_receiver(q, rx_endpoint);
    assert(rv == 0);
    offset += kin_qsize;
    rx_endpoint.addr += kin_qsize;

    q = rq_allocate_in_place((void*) (ctx_buffer_opaque + offset), kout_qsize, conn->qp, ctx_buffer_mr->lkey, ctx_buffer_mr->rkey);
    offset += kout_qsize;
    rx_endpoint.addr += kout_qsize;
    for (int i = 0; i < tas_info->cores_num; i++) {
      q = rq_allocate_in_place((void*) (ctx_buffer_opaque + offset), nic_rx_len, conn->qp, ctx_buffer_mr->lkey, ctx_buffer_mr->rkey);
      rv = rq_pair_receiver(q, rx_endpoint);
      offset += nic_rx_len;
      rx_endpoint.addr += nic_rx_len;
      
      q = rq_allocate_in_place((void*) (ctx_buffer_opaque + offset), nic_tx_len, conn->qp, ctx_buffer_mr->lkey, ctx_buffer_mr->rkey);
      offset += nic_tx_len;
      rx_endpoint.addr += nic_tx_len;
    }
    assert(offset == bytes_needed);

    struct application* app = (struct application*) conn->context;
    struct communicator comm;
    comm.type = CT_REMOTE;
    comm.method.remote_fd.fd = msg->data.ctx_reg.ctx_evfd;
    comm.method.remote_fd.conn = conn;
    struct app_context* app_ctx = allocate_remote_app_context(app, comm, ctx_buffer, config.app_kin_len, config.app_kout_len);
    if (app_ctx != NULL) {
        struct rdma_msg msg;
        msg.type = RDMA_MSG_ACK_CTX_REG;
        struct rdma_ack_ctx_register ctx_reg_ack;
        ctx_reg_ack.memq_base = ctx_buffer_opaque;
        ctx_reg_ack.lkey = ctx_buffer_mr->lkey;
        ctx_reg_ack.rkey = ctx_buffer_mr->rkey;
        msg.data.ctx_reg_ack = ctx_reg_ack;
        if (rdma_send_msg(conn, msg) != 0) {
            fprintf(stderr, "handle_recv_reg_ctx: failed to send ack\n");
            abort();
        }
    }
    return 0;
}

int rdma_init() {
    return 0;
}

static void* rdmaif_thread(void* arg) {
    while (on_rdma_event() == 0);
    abort();
    return NULL;
}

int rdma_server_init(int* server_fd) {
    epfd = epoll_create1(0);
    assert(epfd != -1);

    struct sockaddr_in addr;
    if((event_chn = rdma_create_event_channel()) == NULL) {
        fprintf(stderr, "rdma_server_init: failed to create event channel\n");
        return -1;
    }

    if (rdma_create_id(event_chn, &listener, NULL, RDMA_PS_TCP) != 0) {
        fprintf(stderr, "rdma_server_init: failed to create id\n");
        return -1;
    }

    int rv;
    rv = rdma_common_init();
    assert(rv == 0);
    rv = add_recv_handler(RDMA_MSG_SEND_BASES, handle_recv_bases);
    assert(rv == 0);
    rv = add_send_handler(RDMA_MSG_SEND_BASES, handle_send_bases);
    assert(rv == 0);
    rv = add_imm_handler(RDMA_IMM_SIGNAL, handle_imm_signal);
    assert(rv == 0);
    rv = add_recv_handler(RDMA_MSG_APP_REG, handle_recv_app_reg);
    assert(rv == 0);
    rv = add_recv_handler(RDMA_MSG_REGISTER_CTX, handle_recv_reg_ctx);
    assert(rv == 0);
    rv = add_recv_handler(RDMA_MSG_WAKEUP_TAS, handle_recv_wakeup);
    assert(rv == 0);

    memset((char*) &addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config.nic_port);
    addr.sin_addr.s_addr = 0; // TODO: bind to specific address

    if (rdma_bind_addr(listener, (struct sockaddr *)&addr) != 0) {
        fprintf(stderr, "rdma_server_init: failed to bind to addr\n");
        return -1;
    }

    if (rdma_listen(listener, 10) != 0) {
        fprintf(stderr, "rdma_server_init: failed to listen\n");
        return -1;
    }
    fprintf(stdout, "rdma_server_init: listening for rdma connections on port %d.\n", ntohs(rdma_get_src_port(listener)));

    {
        struct epoll_event ev = {
            .events = EPOLLIN,
            .data.fd = event_chn->fd,
        };
        int r = epoll_ctl(epfd, EPOLL_CTL_ADD, event_chn->fd, &ev);
        assert(r == 0);
    }
    
    if (pthread_create(&pt_rdmaif, NULL, rdmaif_thread, NULL) != 0) {
        fprintf(stdout, "rdma_server_init: failed to create rdma thread\n");
        return -1;
    }
    return 0;
}

void rdma_cleanup() {
    fprintf(stderr, "cleaning up\n");
    if (event_chn != NULL) {
        rdma_destroy_event_channel(event_chn);
    }

    if (listener != NULL) {
        rdma_destroy_id(listener);
    }

    event_chn = NULL;
    listener = NULL;
}