#ifndef PACKETMEM_H_
#define PACKETMEM_H_

/*****************************************************************************/
/**
 * @addtogroup tas-sp-packetmem
 * @brief Packet Memory Manager.
 * @ingroup tas-sp
 *
 * Manages memory region that can be used by FlexNIC for DMA.
 * @{ */

struct packetmem_handle;

/** Initialize packet memory interface */
int packetmem_init(void);

/**
 * Allocate packet memory of specified length.
 *
 * @param length  Required number of bytes
 * @param off     Pointer to location where offset in DMA region should be
 *                stored
 * @param handle  Pointer to location where handle for memory region should be
 *                stored
 *
 * @return 0 on success, <0 else
 */
int packetmem_alloc(size_t length, uintptr_t *off,
    struct packetmem_handle **handle);

/**
 * Free packet memory region.
 *
 * @param handle  Handle for memory region to be freed
 *
 * @return 0 on success, <0 else
 */
void packetmem_free(struct packetmem_handle *handle);

/** @} */

#endif