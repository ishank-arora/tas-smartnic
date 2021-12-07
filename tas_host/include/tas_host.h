#ifndef TAS_HOST_H_
#define TAS_HOST_H_

#include <config.h>
#include <tas_memif.h>
#include <rdma.h>

extern void *tas_shm;
extern struct configuration config;
extern struct flexnic_info *tas_info;
extern struct rdma_connection* nic_conn;

/* Shared memory for DMA */
int shm_init(void);
void shm_cleanup(void);

/* Communication to NIC */
int nicif_init(int* fd);
void nicif_cleanup();
int on_nicif_event();

#endif