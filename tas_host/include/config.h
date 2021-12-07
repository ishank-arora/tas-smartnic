#ifndef CONFIG_H_
#define CONFIG_H_
#include <cli.h>

struct configuration {
  /* shared memory size */
  uint64_t shm_len;
  /** IP address to NIC */
  uint32_t ip;
  /** Port to NIC */
  uint16_t port;
  /** List of routes */
  struct config_route *routes;
  /** FP: use huge pages for internal and buffer memory */
  uint32_t fp_hugepages;
};

int config_parse(struct configuration *c, int argc, char *argv[]);

#endif