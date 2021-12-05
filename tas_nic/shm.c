/*
 * Copyright 2019 University of Washington, Max Planck Institute for
 * Software Systems, and The University of Texas at Austin
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <inttypes.h>

#include <utils.h>
#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_cycles.h>

#include <tas.h>
#include <tas_memif.h>

void *tas_shm = NULL;
struct flextcp_pl_mem *fp_state = NULL;
struct flexnic_info *tas_info = NULL;

/* destroy shared memory region */
static void destroy_shm(const char *name, size_t size, void *addr);
/* create shared memory region using huge pages */
static void *util_create_shmsiszed_huge(const char *name, size_t size,
    void *addr) __attribute__((used));
/* destroy shared huge page memory region */
static void destroy_shm_huge(const char *name, size_t size, void *addr)
    __attribute__((used));
/* convert microseconds to cycles */
static uint64_t us_to_cycles(uint32_t us);

/* Allocate DMA memory before DPDK grabs all huge pages */
int shm_preinit(void)
{
  /* create shm for dma memory */
  if (config.fp_hugepages) {
    tas_shm = util_create_shmsiszed_huge(config.hide ? NULL : FLEXNIC_NAME_DMA_MEM,
        config.shm_len, NULL);
  } else {
    tas_shm = util_create_shmsiszed(config.hide ? NULL : FLEXNIC_NAME_DMA_MEM, config.shm_len,
        NULL);
  }
  if (tas_shm == NULL) {
    fprintf(stderr, "mapping flexnic dma memory failed\n");
    return -1;
  }

  /* create shm for internal memory */
  if (config.fp_hugepages) {
    fp_state = util_create_shmsiszed_huge(config.hide ? NULL : FLEXNIC_NAME_INTERNAL_MEM,
        FLEXNIC_INTERNAL_MEM_SIZE, NULL);
  } else {
    fp_state = util_create_shmsiszed(config.hide ? NULL : FLEXNIC_NAME_INTERNAL_MEM,
        FLEXNIC_INTERNAL_MEM_SIZE, NULL);
  }
  if (fp_state == NULL) {
    fprintf(stderr, "mapping flexnic internal memory failed\n");
    shm_cleanup();
    return -1;
  }

  return 0;
}

int shm_init(unsigned num)
{
  umask(0);

  /* create shm for tas_info */
  tas_info = util_create_shmsiszed(config.hide ? NULL : FLEXNIC_NAME_INFO, FLEXNIC_INFO_BYTES, NULL);
  if (tas_info == NULL) {
    fprintf(stderr, "mapping flexnic tas_info failed\n");
    shm_cleanup();
    return -1;
  }

  tas_info->dma_mem_size = config.shm_len;
  tas_info->internal_mem_size = FLEXNIC_INTERNAL_MEM_SIZE;
  tas_info->qmq_num = FLEXNIC_NUM_QMQUEUES;
  tas_info->cores_num = num;
  tas_info->mac_address = 0;
  tas_info->poll_cycle_app = us_to_cycles(config.fp_poll_interval_app);
  tas_info->poll_cycle_tas = us_to_cycles(config.fp_poll_interval_tas);
  tas_info->nic_ip = 0;
  tas_info->nic_port = 0;
  tas_info->kin_len = config.app_kin_len;
  tas_info->kout_len = config.app_kout_len;
  tas_info->tcp_rxbuf_len = config.tcp_rxbuf_len;
  tas_info->tcp_txbuf_len = config.tcp_txbuf_len;
  strcpy(tas_info->magic, FLEXNIC_MAGIC_STR);
  printf("magic: %s\n", tas_info->magic);

  if (config.fp_hugepages)
    tas_info->flags |= FLEXNIC_FLAG_HUGEPAGES;

  return 0;
}

void shm_cleanup(void)
{
  /* cleanup internal memory region */
  if (fp_state != NULL) {
    if (config.fp_hugepages) {
      destroy_shm_huge(config.hide ? NULL : FLEXNIC_NAME_INTERNAL_MEM, FLEXNIC_INTERNAL_MEM_SIZE,
          fp_state);
    } else {
      destroy_shm(config.hide ? NULL : FLEXNIC_NAME_INTERNAL_MEM, FLEXNIC_INTERNAL_MEM_SIZE,
          fp_state);
    }
  }

  /* cleanup dma memory region */
  if (tas_shm != NULL) {
    if (config.fp_hugepages) {
      destroy_shm_huge(config.hide ? NULL : FLEXNIC_NAME_DMA_MEM, config.shm_len, tas_shm);
    } else {
      destroy_shm(config.hide ? NULL : FLEXNIC_NAME_DMA_MEM, config.shm_len, tas_shm);
    }
  }

  /* cleanup tas_info memory region */
  if (tas_info != NULL) {
    destroy_shm(config.hide ? NULL : FLEXNIC_NAME_INFO, FLEXNIC_INFO_BYTES, tas_info);
  }
}

void shm_set_ready(void)
{
  tas_info->flags |= FLEXNIC_FLAG_READY;
}

void *util_create_shmsiszed(const char *name, size_t size, void *addr)
{
  int fd = -1;
  void *p;

  if (name != NULL) {
    if ((fd = shm_open(name, O_CREAT | O_RDWR, 0666)) == -1) {
      perror("shm_open failed");
      goto error_out;
    }
    if (ftruncate(fd, size) != 0) {
      perror("ftruncate failed");
      goto error_remove;
    }
  }

  int prot_arg;
  int map_arg = addr == NULL ? 0 : MAP_FIXED;
  if (name == NULL) {
    prot_arg = PROT_READ | PROT_WRITE;
    map_arg |= MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE;
  } else {
    prot_arg = PROT_READ | PROT_WRITE;
    map_arg |= MAP_SHARED | MAP_POPULATE; 
  }

  if ((p = mmap(addr, size, prot_arg, map_arg, fd, 0)) == (void *) -1) {
    perror("mmap failed");
    goto error_remove;
  }

  memset(p, 0, size);

  if (name != NULL) {
    close(fd);
  }
  return p;

error_remove:
  if (name != NULL) {
    close(fd);
    shm_unlink(name);
  }
error_out:
  return NULL;
}

static void destroy_shm(const char *name, size_t size, void *addr)
{
  if (munmap(addr, size) != 0) {
    fprintf(stderr, "Warning: munmap failed (%s)\n", strerror(errno));
  }
  if (name != NULL) {
    shm_unlink(name);
  }
}

static void *util_create_shmsiszed_huge(const char *name, size_t size,
    void *addr)
{
  int fd = -1;
  void *p;
  char path[128];

  if (name != NULL) {
    snprintf(path, sizeof(path), "%s/%s", FLEXNIC_HUGE_PREFIX, name);

    if ((fd = open(path, O_CREAT | O_RDWR, 0666)) == -1) {
      perror("util_create_shmsiszed: open failed");
      goto error_out;
    }
    if (ftruncate(fd, size) != 0) {
      perror("util_create_shmsiszed: ftruncate failed");
      goto error_remove;
    }
  }

  int prot_arg;
  int map_arg = addr == NULL ? 0 : MAP_FIXED;
  if (name == NULL) {
    prot_arg = PROT_READ | PROT_WRITE;
    map_arg |= MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE;
  } else {
    prot_arg = PROT_READ | PROT_WRITE;
    map_arg |= MAP_SHARED | MAP_POPULATE; 
  }

  if ((p = mmap(addr, size, prot_arg, map_arg, fd, 0)) == (void *) -1) {
    perror("util_create_shmsiszed: mmap failed");
    goto error_remove;
  }

  memset(p, 0, size);

  if (name != NULL) {
    close(fd);
  }
  return p;

error_remove:
  if (name != NULL) {
    close(fd);
    shm_unlink(name);
  }
error_out:
  return NULL;
}

static void destroy_shm_huge(const char *name, size_t size, void *addr)
{
  char path[128];

  if (munmap(addr, size) != 0) {
    fprintf(stderr, "Warning: munmap failed (%s)\n", strerror(errno));
  }
  if (name != NULL) {
    snprintf(path, sizeof(path), "%s/%s", FLEXNIC_HUGE_PREFIX, name);
    unlink(path);
  }
}

static uint64_t us_to_cycles(uint32_t us)
{
  if (us == UINT32_MAX) {
    return UINT64_MAX;
  }

  return (rte_get_tsc_hz() * us) / 1000000;
}

