#include <stdlib.h>
#include <tas_host.h>
#include <sys/epoll.h>
#include <assert.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <sys/un.h>
#include <kernel_appif.h>

struct configuration config;
static int epfd;
void* tas_shm;
struct flexnic_info* tas_info;

int main(int argc, char *argv[]) {
    int res = EXIT_SUCCESS;

    /* parse command line options */
    if (config_parse(&config, argc, argv) != 0) {
        res = EXIT_FAILURE;
        goto error_exit;
    }

    if (shm_init() != 0) {
        res = EXIT_FAILURE;
        goto error_exit;
    }

    /* create epoll obj */
    epfd = epoll_create1(0);
    assert(epfd != -1);

    /* connect to nic */
    int nicif_fd = -1;
    if (nicif_init(&nicif_fd) != 0) {
        res = EXIT_FAILURE;
        goto error_exit;
    }
    {
        struct epoll_event ev = {
            .events = EPOLLIN,
            .data.fd = nicif_fd,
        };
        int r = epoll_ctl(epfd, EPOLL_CTL_ADD, nicif_fd, &ev);
        assert(r == 0);
    }

    /* socket to send fds*/
    int mfd;
    if ((mfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket failed");
        abort();
    }

    struct sockaddr_un saun;
    memset(&saun, 0, sizeof(saun));
    saun.sun_family = AF_UNIX;
    memcpy(saun.sun_path, KERNEL_SOCKET_PATH, sizeof(KERNEL_SOCKET_PATH));

    if (bind(mfd, (struct sockaddr*) &saun, sizeof(saun))) {
        perror("bind failed");
        abort();
    }

    if (listen(mfd, 5)) {
        perror("listen failed");
        abort();
    }

    {
        struct epoll_event ev = {
            .events = EPOLLIN,
            .data.fd = mfd,
        };
        int r = epoll_ctl(epfd, EPOLL_CTL_ADD, mfd, &ev);
        assert(r == 0);
    }

    /* event sockets to wake up kernel */
    int k_evfd = eventfd(0, EFD_NONBLOCK);
    if (k_evfd < 0) {
        perror("Failed to create kernel wakeup eventfd:");
        abort();
    }
    {
        struct epoll_event ev = {
            .events = EPOLLIN,
            .data.fd = k_evfd,
        };
        int r = epoll_ctl(epfd, EPOLL_CTL_ADD, k_evfd, &ev);
        assert(r == 0);
    }

    /* event sockets to wake up fast path */
    int n_cores = ((volatile struct flexnic_info*) tas_info)->cores_num;
    int* fp_evfds = malloc(n_cores * sizeof(*fp_evfds));
    printf("allocating %d fp file descriptors\n", n_cores);
    if (n_cores > 0) {
        printf("file descriptors:");
    }
    for (int i = 0; i < n_cores; i++) {
        fp_evfds[i] = eventfd(0, EFD_NONBLOCK);
        if (fp_evfds[i] < 0) {
            perror("Failed to create fast path wakeup eventfd:");
            abort();
        }   
        {
            struct epoll_event ev = {
                .events = EPOLLIN,
                .data.fd = fp_evfds[i],
            };
            int r = epoll_ctl(epfd, EPOLL_CTL_ADD, fp_evfds[i], &ev);
            assert(r == 0);
        }
        printf(" %d", fp_evfds[i]);
    }
    printf("\n");

    /* handle connections with TAS */
    int app_epfd = epoll_create1(0);
    {
        struct epoll_event ev = {
            .events = EPOLLIN,
            .data.fd = app_epfd,
        };
        int r = epoll_ctl(epfd, EPOLL_CTL_ADD, app_epfd, &ev);
        assert(r == 0);
    }

    /* poll loop */
    int n, i;
    struct epoll_event event[50];
    int* notify_fast = malloc(tas_info->cores_num * sizeof(*notify_fast));
    while ((n = epoll_wait(epfd, event, 50, -1)) != -1) {
        int notify_slow = 0;
        int needs_notify = 0;
        memset(notify_fast, 0, tas_info->cores_num * sizeof(*notify_fast));
        for (i = 0; i < n; i++) {
            int fd = event[i].data.fd;
            if (fd == nicif_fd) {
                if (on_nicif_event() != 0) {
                    fprintf(stderr, "unable to handle nicif event\n");
                    return -1;
                }
            } else if (fd == mfd) {
                int cfd = -1;
                if ((cfd = accept(mfd, NULL, NULL)) < 0) {
                    fprintf(stderr, "accept failed\n");
                    abort();
                }
                printf("accepted connection to application (fd: %d)\n", cfd);

                struct iovec iov = {
                    .iov_base = &n_cores,
                    .iov_len = sizeof(uint32_t),
                };
                union {
                    char buf[CMSG_SPACE(sizeof(int) * 4)];
                    struct cmsghdr align;
                } u;
                struct msghdr msg = {
                    .msg_name = NULL,
                    .msg_namelen = 0,
                    .msg_iov = &iov,
                    .msg_iovlen = 1,
                    .msg_control = u.buf,
                    .msg_controllen = sizeof(u.buf),
                    .msg_flags = 0,
                };

                memset(u.buf, 0, sizeof(u.buf));
                struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
                cmsg->cmsg_level = SOL_SOCKET;
                cmsg->cmsg_type = SCM_RIGHTS;
                cmsg->cmsg_len = CMSG_LEN(sizeof(uint32_t));
                int* pfd = (int*) CMSG_DATA(cmsg);
                *pfd = k_evfd;

                /* send out kernel notify fd */
                printf("sent kernel notify\n");
                int tx;
                if((tx = sendmsg(cfd, &msg, 0)) != sizeof(uint32_t)) {
                    fprintf(stderr, "tx == %d\n", tx);
                    if(tx == -1) {
                        perror("Socket received error");
                    }
                }

                printf("sending %d fast path fds\n", n_cores);
                uint8_t b = 0;
                for (int off = 0; off < n_cores;) {
                    iov.iov_base = &b;
                    iov.iov_len = 1;

                    memset(&msg, 0, sizeof(msg));
                    msg.msg_iov = &iov;
                    msg.msg_iovlen = 1;
                    msg.msg_control = u.buf;
                    msg.msg_controllen = sizeof(u.buf);

                    n = (n_cores - off >= 4 ? 4 : n_cores - off);

                    cmsg->cmsg_level = SOL_SOCKET;
                    cmsg->cmsg_type = SCM_RIGHTS;
                    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * n);

                    printf("sending file descriptors:");
                    pfd = (int *) CMSG_DATA(cmsg);
                    for (int j = 0; j < n; j++) {
                        pfd[j] = fp_evfds[off++];
                        printf(" %d", pfd[j]);
                    }
                    printf("\n");

                    /* send out kernel notify fd */
                    if((tx = sendmsg(cfd, &msg, 0)) != 1) {
                        fprintf(stderr, "tx fd == %d, off == %d\n", tx, off);
                        if(tx == -1) {
                            perror("errno:");
                        }
                        abort();
                    }
                }
                printf("sent fastpath fds\n");
                {
                    struct epoll_event ev = {
                        .events = EPOLLIN,
                        .data.fd = cfd,
                    };
                    int r = epoll_ctl(app_epfd, EPOLL_CTL_ADD, cfd, &ev);
                    assert(r == 0);
                }
            } else if (fd == k_evfd) {
                uint64_t val;
                int ret = read(k_evfd, &val, sizeof(uint64_t));
                if ((ret > 0 && ret != sizeof(uint64_t)) ||
                    (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) 
                {
                    perror("slow read failed");
                    abort();
                }
                notify_slow = 1;
                needs_notify = 1;
                printf("notifying tas slow path\n");
            } else if (fd == app_epfd) {
                struct epoll_event event[50];
                int n = epoll_wait(app_epfd, event, 50, -1);
                for (int i = 0; i < n; i++) {
                    int app_fd = event[i].data.fd;
                    /* receive data to hopefully complete request */
                    struct kernel_uxsock_request req;
                    struct iovec iov = {
                        .iov_base = &req,
                        .iov_len = sizeof(req),
                    };
                    union {
                        char buf[CMSG_SPACE(sizeof(int))];
                        struct cmsghdr align;
                    } u;
                    struct msghdr msg = {
                        .msg_name = NULL,
                        .msg_namelen = 0,
                        .msg_iov = &iov,
                        .msg_iovlen = 1,
                        .msg_control = &u.buf,
                        .msg_controllen = sizeof(u.buf),
                        .msg_flags = 0,
                    };
                    ssize_t r;
                    if ((r = recvmsg(app_fd, &msg, 0)) != sizeof(req)) {
                        fprintf(stderr, "failed to receive ctx fd " 
                                "(fd: %d msg size: %ld error: %s)\n", 
                                app_fd, r, strerror(errno));
                        abort();
                    }
                    printf("received ctx fd\n");

                    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
                    if (msg.msg_controllen <= 0 || cmsg->cmsg_len != CMSG_LEN(sizeof(int))) {
                        fprintf(stderr, "accessing ancillary data failed\n");
                        abort();
                    }
                    int ctx_fd = *CMSG_DATA(cmsg);

                    // remove from epoll
                    r = epoll_ctl(app_epfd, EPOLL_CTL_DEL, app_fd, NULL);
                    assert(r == 0);
                    
                    /* reply back with host's fd */
                    if (send(app_fd, &ctx_fd, sizeof(ctx_fd), 0) != sizeof(ctx_fd)) {
                        fprintf(stderr, "failed to return fd\n");
                        abort();
                    }
                    printf("sent host's view of ctx fd (%d)\n", ctx_fd);
                }
            } else {
                for (int fp_idx = 0; fp_idx < tas_info->cores_num; fp_idx++) {
                    if (fd == fp_evfds[fp_idx]) {
                        uint64_t val;
                        int ret = read(fp_evfds[fp_idx], &val, sizeof(uint64_t));
                        if ((ret > 0 && ret != sizeof(uint64_t)) ||
                            (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) 
                        {
                            perror("fp read failed");
                            abort();
                        }
                        notify_fast[fp_idx] = 1;
                    }
                }
                printf("notifying tas fast path\n");
                needs_notify = 1;
            }
        }

        if (needs_notify) {
            struct rdma_msg notify_msg;
            notify_msg.type = RDMA_MSG_WAKEUP_TAS;
            notify_msg.data.wakeup_tas_info.wakeup_fast = notify_slow;
            int fp_int_flags = 0;
            for (int fp_idx = 0; fp_idx < tas_info->cores_num; fp_idx++) {
                if (notify_fast[fp_idx]) {
                    fp_int_flags |= (1 << i);
                }
            }
            notify_msg.data.wakeup_tas_info.wakeup_slow = fp_int_flags;
            int rv = rdma_send_msg(nic_conn, notify_msg);
            assert(rv == 0);
            printf("notify kernel: %d\tnotify fp flags: %x\n", notify_slow, fp_int_flags);
        }
    }
    res = -1;


error_exit:
    return res;

}