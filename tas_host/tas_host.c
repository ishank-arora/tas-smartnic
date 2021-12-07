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
    int nicif_fd;
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
    {
        struct epoll_event ev = {
            .events = EPOLLIN,
            .data.fd = k_evfd,
        };
        int r = epoll_ctl(epfd, EPOLL_CTL_ADD, k_evfd, &ev);
        assert(r == 0);
    }

    /* event sockets to wake up fast path */
    int* fp_evfds = malloc(tas_info->cores_num);
    for (int i = 0; i < tas_info->cores_num; i++) {
        fp_evfds[i] = eventfd(0, EFD_NONBLOCK);
        {
            struct epoll_event ev = {
                .events = EPOLLIN,
                .data.fd = fp_evfds[i],
            };
            int r = epoll_ctl(epfd, EPOLL_CTL_ADD, fp_evfds[i], &ev);
            assert(r == 0);
        }
    }

    /* poll loop */
    int n, i;
    struct epoll_event event[50];
    int* notify_fast = malloc(tas_info->cores_num);
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
                int cfd;
                if ((cfd = accept(fd, NULL, NULL)) < 0) {
                    fprintf(stderr, "accept failed\n");
                }

                struct iovec iov = {
                    .iov_base = &tas_info->cores_num,
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

                struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
                cmsg->cmsg_level = SOL_SOCKET;
                cmsg->cmsg_type = SCM_RIGHTS;
                cmsg->cmsg_len = CMSG_LEN(sizeof(int));
                int* pfd = (int*) CMSG_DATA(cmsg);
                *pfd = k_evfd;

                /* send out kernel notify fd */
                int tx;
                if((tx = sendmsg(cfd, &msg, 0)) != sizeof(uint32_t)) {
                    fprintf(stderr, "tx == %d\n", tx);
                    if(tx == -1) {
                    fprintf(stderr, "errno == %d\n", errno);
                    }
                }

                int off = 0;
                uint8_t b = 0;
                for (; off < tas_info->cores_num;) {
                    iov.iov_base = &b;
                    iov.iov_len = 1;

                    memset(&msg, 0, sizeof(msg));
                    msg.msg_iov = &iov;
                    msg.msg_iovlen = 1;
                    msg.msg_control = u.buf;
                    msg.msg_controllen = sizeof(u.buf);

                    n = (tas_info->cores_num - off >= 4 ? 4 : tas_info->cores_num - off);

                    cmsg->cmsg_level = SOL_SOCKET;
                    cmsg->cmsg_type = SCM_RIGHTS;
                    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * n);

                    pfd = (int *) CMSG_DATA(cmsg);
                    for (int j = 0; j < n; j++) {
                        pfd[j] = fp_evfds[j];
                    }

                    /* send out kernel notify fd */
                    if((tx = sendmsg(cfd, &msg, 0)) != 1) {
                        fprintf(stderr, "tx fd == %d\n", tx);
                        if(tx == -1) {
                            fprintf(stderr, "errno fd == %d\n", errno);
                        }
                        abort();
                    }
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
        }
    }
    res = -1;


error_exit:
    return res;

}