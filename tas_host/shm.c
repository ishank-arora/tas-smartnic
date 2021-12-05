#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include <tas_host.h>
#include <tas_memif.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* create shared memory region */
static void *util_create_shmsiszed(const char *name, size_t size, void *addr);
/* destroy shared memory region */
static void destroy_shm(const char *name, size_t size, void *addr);
/* create shared memory region using huge pages */
static void *util_create_shmsiszed_huge(const char *name, size_t size,
    void *addr) __attribute__((used));
/* destroy shared huge page memory region */
static void destroy_shm_huge(const char *name, size_t size, void *addr)
    __attribute__((used));

int shm_init(void)
{
  umask(0);

  /* create shm for tas_info */
  tas_info = util_create_shmsiszed(FLEXNIC_NAME_INFO, FLEXNIC_INFO_BYTES, NULL);
  if (tas_info == NULL) {
    fprintf(stderr, "mapping flexnic tas_info failed\n");
    shm_cleanup();
    return -1;
  }

  return 0;
}

void shm_cleanup(void)
{
  /* cleanup dma memory region */
  if (tas_shm != NULL) {
    if (config.fp_hugepages) {
      destroy_shm_huge(FLEXNIC_NAME_DMA_MEM, config.shm_len, tas_shm);
    } else {
      destroy_shm(FLEXNIC_NAME_DMA_MEM, config.shm_len, tas_shm);
    }
  }

  /* cleanup tas_info memory region */
  if (tas_info != NULL) {
    destroy_shm(FLEXNIC_NAME_INFO, FLEXNIC_INFO_BYTES, tas_info);
  }
}

static void *util_create_shmsiszed(const char *name, size_t size, void *addr)
{
  int fd;
  void *p;

  if ((fd = shm_open(name, O_CREAT | O_RDWR, 0666)) == -1) {
    perror("shm_open failed");
    goto error_out;
  }
  if (ftruncate(fd, size) != 0) {
    perror("ftruncate failed");
    goto error_remove;
  }

  if ((p = mmap(addr, size, PROT_READ | PROT_WRITE,
      MAP_SHARED | (addr == NULL ? 0 : MAP_FIXED) | MAP_POPULATE, fd, 0)) ==
      (void *) -1)
  {
    perror("mmap failed");
    goto error_remove;
  }

  memset(p, 0, size);

  close(fd);
  return p;

error_remove:
  close(fd);
  shm_unlink(name);
error_out:
  return NULL;
}

static void destroy_shm(const char *name, size_t size, void *addr)
{
  if (munmap(addr, size) != 0) {
    fprintf(stderr, "Warning: munmap failed (%s)\n", strerror(errno));
  }
  shm_unlink(name);
}

static void *util_create_shmsiszed_huge(const char *name, size_t size,
    void *addr)
{
  int fd;
  void *p;
  char path[128];

  snprintf(path, sizeof(path), "%s/%s", FLEXNIC_HUGE_PREFIX, name);

  if ((fd = open(path, O_CREAT | O_RDWR, 0666)) == -1) {
    perror("util_create_shmsiszed: open failed");
    goto error_out;
  }
  if (ftruncate(fd, size) != 0) {
    perror("util_create_shmsiszed: ftruncate failed");
    goto error_remove;
  }

  if ((p = mmap(addr, size, PROT_READ | PROT_WRITE,
      MAP_SHARED | (addr == NULL ? 0 : MAP_FIXED) | MAP_POPULATE, fd, 0)) ==
      (void *) -1)
  {
    perror("util_create_shmsiszed: mmap failed");
    goto error_remove;
  }

  memset(p, 0, size);

  close(fd);
  return p;

error_remove:
  close(fd);
  shm_unlink(name);
error_out:
  return NULL;
}

static void destroy_shm_huge(const char *name, size_t size, void *addr)
{
  char path[128];

  snprintf(path, sizeof(path), "%s/%s", FLEXNIC_HUGE_PREFIX, name);

  if (munmap(addr, size) != 0) {
    fprintf(stderr, "Warning: munmap failed (%s)\n", strerror(errno));
  }
  unlink(path);
}