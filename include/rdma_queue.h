#ifndef RDMA_QUEUE_H_
#define RDMA_QUEUE_H_

#include <rdma/rdma_cma.h>
#include <inttypes.h>
#include <assert.h>

struct rdma_queue_endpoint {
    uint64_t addr;
    uint64_t offset;
    uint32_t lkey;
    uint32_t rkey;
};

struct rdma_queue_endpoints {
    struct rdma_queue_endpoint rx;
    struct rdma_queue_endpoint tx;
    int paired;
};

/**
 * @brief Single producer-single consumer queue
 * 
 * Notes:
 *   A dummy sequence of bytes is used to differentiate a filled and empty buffer. 
 */
struct rdma_queue {
    struct ibv_qp* qp;
    uint64_t buffer_size;
    uint64_t flush_offset;
    struct rdma_queue_endpoints endpoints;
    int is_tx;
    uint8_t buffer[];
};

#define RDMA_QUEUE_PAD 8
#define RDMA_QUEUE_SZ (sizeof(struct rdma_queue) + RDMA_QUEUE_PAD)

static inline void* rq_rx_head(struct rdma_queue* queue) {
    return &((uint8_t*)queue->buffer)[queue->endpoints.rx.offset];
}

static inline void* rq_tx_tail(struct rdma_queue* queue) {
    return &((uint8_t*)queue->buffer)[queue->endpoints.tx.offset];
}

static inline size_t rq_nbytes(struct rdma_queue* queue, uintptr_t from, uintptr_t to, int inner) {
    ssize_t rm;
    if (from < to) {
        rm = to - from;
    } else if (from > to) {
        rm = queue->buffer_size - from + to;
    } else {
        rm = inner ? 0 : queue->buffer_size - RDMA_QUEUE_PAD;
    }
    assert(rm >= 0);
    return (size_t) rm;
}

/**
 * @brief The number of bytes to flush; should only be called by transmitter
 * 
 * @param queue 
 * @return size_t the number of bytes
 */
static inline size_t rq_nbytes_to_flush(struct rdma_queue* queue) {
    assert(queue->endpoints.tx.addr == (uintptr_t) queue);
    return rq_nbytes(queue, queue->flush_offset, queue->endpoints.tx.offset, 1);
}

/**
 * @brief The number of bytes that are enqueued; should only be called by receiver
 * 
 * @param queue 
 * @return size_t the number of bytes
 */
static inline size_t rq_nbytes_enqueued(struct rdma_queue* queue) {
    assert(queue->endpoints.rx.addr == (uintptr_t) queue);
    return queue->endpoints.paired ? 
        rq_nbytes(queue, queue->endpoints.rx.offset, queue->endpoints.tx.offset, 1) :
        rq_nbytes(queue, queue->endpoints.rx.offset, queue->flush_offset, 1);
}

/**
 * @brief The number of bytes available for enqueuing
 * 
 * @param queue 
 * @return size_t the number of bytes
 */
static inline size_t rq_nbytes_empty(struct rdma_queue* queue) {
    int bytes;
    if (queue->endpoints.paired) {
        if (queue->is_tx) {
            bytes = rq_nbytes_to_flush(queue);
        } else {
            bytes = rq_nbytes_enqueued(queue);
        }
    } else {
        bytes = rq_nbytes(queue, queue->endpoints.rx.offset, queue->endpoints.tx.offset, 1);
    }
    return queue->buffer_size - bytes - RDMA_QUEUE_PAD;
}

/**
 * @brief Tries to enqueue a sequence of bytes into the queue. Will
 * attempt to flush the queue on error
 * 
 * @param queue the queue to try to enqueue into
 * @param to_enqueue the buffer to write the bytes from
 * @param sz  the number of bytes to write
 * @return int 0 on success, negative on error, positive on failure
 */
int rq_try_enqueue(struct rdma_queue* queue, void* to_enqueue, size_t sz);

/**
 * @brief the queue to try to dequeue from
 * 
 * @param queue the queue to read the bytes from
 * @param buffer the buffer to read the bytes into
 * @param sz the number of bytes to read
 * @return int 0 on success, negative on error, positive on failure
 */
int rq_try_dequeue(struct rdma_queue* queue, void* buffer, size_t sz);

/**
 * @brief the queue to try to peek into
 * 
 * @param queue the queue to read the bytes from
 * @param buffer the buffer to read the bytes into
 * @param sz the number of bytes to read
 * @return int 0 on success, negative on error, positive on failure
 */
int rq_try_peek(struct rdma_queue* queue, void* buffer, size_t sz);

/**
 * @brief Allocate a rdma_queue from an array of nbytes bytes
 * 
 * @param bytes the array of bytes to turn into a rdma_queue
 * @param nbytes the number of bytes in `bytes`; the size of the rdma_queue in bytes, including the metadata
 * @param qp the queue pair used for RDMA
 * @param lkey the local memory protection region key
 * @param rkey the remote memory protection region key
 * @return struct rdma_queue* `bytes` as a rdma_queue
 */
struct rdma_queue* rq_allocate_in_place(void* bytes, int nbytes, struct ibv_qp* qp, int lkey, int rkey);

/**
 * @brief Pair the queue with the receiver
 * 
 * @param queue the queue to pair up with
 * @param rx_endpoint details of the remote endpoint
 * @return int 0 on success
 */
int rq_pair_receiver(struct rdma_queue* queue, struct rdma_queue_endpoint rx_endpoint);

/**
 * @brief Tries to reserve `to_flush` additional bytes of space to enqueue then flush
 * 
 * @param queue the queue to reserve
 * @param to_flush the amount of bytes to reserve
 * @return int 1 on success, 0 on failure, -1 on error
 */
int rq_try_reserve_flush(struct rdma_queue* queue, size_t to_flush);

/**
 * @brief Flushes n bytes to the receiver
 * 
 * @param queue the queue to flush
 * @param to_flush the number of bytes to flush
 * @return ssize_t negative on failure, else number of bytes flushed
 */
ssize_t rq_nflush(struct rdma_queue* queue, size_t to_flush);

/**
 * @brief Flushes all tx bytes to the receiver
 * 
 * @param queue the queue to flush
 * @return ssize_t negative on failure, else number of bytes flushed 
 */
ssize_t rq_flush(struct rdma_queue* queue);
#endif