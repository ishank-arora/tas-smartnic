#include <tas_host.h>
#include <netdb.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <assert.h>
#include <rdma.h>
#include <rdma_common.h>
#include <host_nic.h>
#include <sys/mman.h>
#include <unistd.h>

static struct rdma_cm_id* id = NULL;
static struct rdma_event_channel* event_chn = NULL;
static struct rdma_context* ctx;
struct rdma_connection* nic_conn;

static int on_rdma_client_event();
static int on_addr_resolved(struct rdma_cm_id* id);
static int on_route_resolved(struct rdma_cm_id* id);
static int on_connection(void* ctx);
static int on_disconnect(struct rdma_cm_id* id);

static int epfd;
const int TIMEOUT_IN_MS = 500;

int on_nicif_event() {
    struct epoll_event event[2];
    int n = epoll_wait(epfd, event, 2, 0);
    int i, rv;

    for (i = 0; i < n; i++) {
        int fd = event[i].data.fd;
        if (fd == event_chn->fd) {
            rv = on_rdma_client_event();
        } else if (ctx->comp_chn != NULL && fd == ctx->comp_chn->fd) {
            rv = rdma_common_on_completion(ctx->comp_chn);
        } else {
            rv = -1;
            fprintf(stderr, "on_nicif_event: got event from unknown file descriptor\n");
        }

        if (rv != 0) {
            nicif_cleanup();
            break;
        }
    }
    return rv;
}

static int on_rdma_client_event() {
    struct rdma_cm_event* event;
    struct rdma_cm_event event_copy;
    int rv;
    rdma_get_cm_event(event_chn, &event);
    memcpy(&event_copy, event, sizeof(struct rdma_cm_event));
    rdma_ack_cm_event(event);

    switch (event_copy.event) {
        case RDMA_CM_EVENT_ADDR_RESOLVED:
            rv = on_addr_resolved(event_copy.id);
            break;
        case RDMA_CM_EVENT_ROUTE_RESOLVED:
            rv = on_route_resolved(event_copy.id);
            break;
        case RDMA_CM_EVENT_ESTABLISHED:
            rv = on_connection(event_copy.id->context);
            break;
        case RDMA_CM_EVENT_DISCONNECTED:
            rv = on_disconnect(event_copy.id);
            // TODO: handle disconnection occurring with ongoing flows
            break;
        default:
            rv = -1;
            fprintf(stderr, "on_event: received %s\n", rdma_event_str(event_copy.event));
            break; 
    }
    return rv;
}

static int on_addr_resolved(struct rdma_cm_id *id) {
    int add_to_epoll = ctx == NULL;

    /* build connection*/
    struct rdma_connection* conn;
    if ((conn = rdma_common_build_connection(id, &ctx)) == NULL) {
        fprintf(stderr, "on_addr_resolved: failed to build connection\n");
        return -1;
    }
    nic_conn = conn;

    if (add_to_epoll) {
        {
            struct epoll_event ev = {
            .events = EPOLLIN,
            .data.fd = ctx->comp_chn->fd,
            };
            int r = epoll_ctl(epfd, EPOLL_CTL_ADD, ctx->comp_chn->fd, &ev);
            assert(r == 0);
        }
    }

    if (rdma_resolve_route(id, TIMEOUT_IN_MS) != 0) {
        perror("on_addr_resolved: failed to resolve route\n");
        return -1;
    }

    return 0;
}

static int on_route_resolved(struct rdma_cm_id* id) {
    struct rdma_conn_param cm_params;
    memset(&cm_params, 0, sizeof(cm_params));
    if (rdma_connect(id, &cm_params) != 0) {
        fprintf(stderr, "on_route_resolved: failed to find route to host\n");
        return -1;
    }

    return 0;
}

static int on_connection(void* context) {
    struct rdma_connection* conn = (struct rdma_connection*) context;

    if (rdma_common_send_bases(conn, ctx, config.shm_len) != 0) {
        fprintf(stderr, "on_connection: failed to post send\n");
        return -1;
    }
    return 0;
}

static int handle_recv_bases(struct rdma_connection* conn, const struct rdma_msg* msg) {
    assert(conn->context_type == RCONN_TYPE_INTERFACE);
    struct host_nic_context* hn_ctx = (struct host_nic_context*) conn->context;
    
    printf("received tas_info bases\n");
    hn_ctx->bases = msg->data.bases;
    hn_ctx->flags |= RC_FLAG_RECEIVED_BASES;
    tas_info->nic_ip = config.ip;
    tas_info->nic_port = config.port;
    return 0;
}

static int handle_imm_tas_info(struct rdma_connection* conn) {
    printf("tas_info written locally\n");
    if (strcmp(tas_info->magic, FLEXNIC_MAGIC_STR) != 0) {
        fprintf(stderr, "on_completion: tas_info does not contain correct magic str (str: %s)\n", tas_info->magic);
        return -1;
    }
    return 0;
}

static int handle_wakeup(struct rdma_connection* conn, const struct rdma_msg* msg) {
    assert(conn->context_type == RCONN_TYPE_INTERFACE);
    int fd = msg->data.wakeup_app_info.fd;
    uint64_t val = 1;
    if (write(fd, &val, sizeof(uint64_t)) != sizeof(uint64_t)) {
        perror("handle_wakeup: write failed");
        return -1;
    }
    return 0;
}

static int on_disconnect(struct rdma_cm_id* id) {
    struct rdma_connection *conn = (struct rdma_connection*) id->context;
    assert(nic_conn == conn);
    printf("peer disconnected.\n");
    rdma_destroy_qp(id);
    ibv_dereg_mr(conn->receive_buffer_mr);
    ibv_dereg_mr(conn->send_buffer_mr);
    
    rdma_common_free_conn_context(conn);
    free(conn);
    rdma_destroy_id(id);
    return 1;
}

int nicif_init(int* fd) {
    epfd = epoll_create1(0);
    assert(epfd != -1);

    struct addrinfo* addr = NULL;
    uint32_t ip32 = ntohl(config.ip);
    char ip[INET_ADDRSTRLEN];
    char port[6];
    snprintf(port, 6, "%d", config.port);
    const void* iprv;
    iprv = inet_ntop(AF_INET, &ip32, ip, INET_ADDRSTRLEN);
    if (iprv == NULL) {
        fprintf(stderr, "nicif_init: parsing ip failed\n");
        goto error_exit;
    }

    printf("Connecting to %s:%d\n", ip, config.port); 
    if (getaddrinfo(ip, port, NULL, &addr) != 0) {
        fprintf(stderr, "nicif_init: getaddrinfo failed\n");
        goto error_exit;
    }

    
    if ((event_chn = rdma_create_event_channel()) == 0) {
        fprintf(stderr, "nicif_init: could not create event channel\n");
        goto error_exit;
    }

    if (rdma_create_id(event_chn, &id, NULL, RDMA_PS_TCP) != 0) {
        fprintf(stderr, "nicif_init: could not create communication identifier\n");
        goto error_exit;
    }

    if (rdma_resolve_addr(id, NULL, addr->ai_addr, TIMEOUT_IN_MS) != 0) {
        fprintf(stderr, "nicif_init: could not resolve address\n");
        goto error_exit;
    }

    freeaddrinfo(addr);
    addr = NULL;
    {
        struct epoll_event ev = {
            .events = EPOLLIN,
            .data.fd = event_chn->fd,
        };
        int r = epoll_ctl(epfd, EPOLL_CTL_ADD, event_chn->fd, &ev);
        assert(r == 0);
    }

    int rv;
    rv = rdma_common_init();
    assert(rv == 0);
    rv = add_recv_handler(RDMA_MSG_SEND_BASES, handle_recv_bases);
    assert(rv == 0);
    rv = add_imm_handler(RDMA_IMM_TAS_INFO, handle_imm_tas_info);
    assert(rv == 0);
    rv = add_recv_handler(RDMA_MSG_WAKEUP_APP, handle_wakeup);
    assert(rv == 0);

    /* wait for connection made with host */
    while (1) {
        if (on_nicif_event() != 0) {
            fprintf(stderr, "nicif_init: unable to handle nicif event\n");
            goto error_exit;
        }

        if (nic_conn != NULL &&
                nic_conn->context != NULL && 
                ((struct host_nic_context*) nic_conn->context)->flags & RC_FLAG_RECEIVED_BASES) {
            break;
        }
    }

    *fd = epfd;
    return 0;

error_exit:
    if (addr != NULL) {
        freeaddrinfo(addr);
    }
    nicif_cleanup();
    return -1;
}

void nicif_cleanup() {
    if (event_chn != NULL) {
        rdma_destroy_event_channel(event_chn);
    }

    if (id != NULL) {
        rdma_destroy_id(id);
    }

    event_chn = NULL;
    id = NULL;
}