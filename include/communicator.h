#ifndef COMMUNICATOR_H_
#define COMMUNICATOR_H_

#include <unistd.h>

struct rdma_connection;

enum communicator_type {
  CT_LOCAL,
  CT_REMOTE,
};

struct communicator {
  enum communicator_type type;
  union {
    int fd;
    struct {
      int fd;
      struct rdma_connection* conn;
    } remote_fd;
  } method;
};

static inline int communicator_close(struct communicator* communicator) {
  switch (communicator->type) {
    case CT_LOCAL:
      return close(communicator->method.fd);
    case CT_REMOTE:
      // TODO: implement remote close
      break;
  }
  return -1;
}

#endif