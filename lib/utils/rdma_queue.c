#include <rdma_queue.h>
#include <rdma.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <utils.h>

#define OFFSET(ptr, x) ((void*) ((uintptr_t) (ptr) + (x)))
#define RDMA_QUEUE_FULL ((1 << 0)) 
#define RDMA_QUEUE_PTR(queue, x) OFFSET(queue->buffer, x)
#define RDMA_QUEUE_EMPTY 0

int rq_try_enqueue(struct rdma_queue* queue, void* to_enqueue, size_t sz) {
    assert((uintptr_t) queue == queue->endpoints.tx.addr);
    
    if (sz > rq_nbytes_empty(queue)) {
        if (rq_flush(queue) < 0) {
            fprintf(stderr, "rq_try_enqueue: failed to flush queue\n");
            return -1;
        }
        return 1;
    }

    int b1, b2;
    b1 = queue->endpoints.tx.offset + sz > queue->buffer_size ? queue->buffer_size - queue->endpoints.tx.offset : sz;
    b2 = sz - b1;
    if (to_enqueue != NULL)
        memcpy(RDMA_QUEUE_PTR(queue, queue->endpoints.tx.offset), to_enqueue, b1);
    
    uint64_t new_tx_offset;
    if (b2 > 0) {
        if (to_enqueue != NULL)
            memcpy(RDMA_QUEUE_PTR(queue, 0), OFFSET(to_enqueue, b2), b2);
        new_tx_offset = b2;
    } else {
        assert(b1 == sz);
        new_tx_offset = queue->endpoints.tx.offset + b1;
        MEM_BARRIER();
    }
    MEM_BARRIER();
    queue->endpoints.tx.offset = new_tx_offset;
    return 0;
}

static inline int rq_try_possible_dequeue(struct rdma_queue* queue, void* buffer, size_t sz, int dequeue) {
    assert((uintptr_t) queue == queue->endpoints.rx.addr);
    
    int bytes = rq_nbytes_enqueued(queue);
    if (bytes < sz) {
        return 1;
    }

    int b1, b2;
    b1 = queue->endpoints.rx.offset + sz > queue->buffer_size ? queue->buffer_size - queue->endpoints.rx.offset : sz;
    b2 = sz - b1;
    uint64_t new_offset;
    if (buffer != NULL)
        memcpy(buffer, RDMA_QUEUE_PTR(queue, queue->endpoints.rx.offset), b1);
    if (b2 > 0) {
        if (buffer != NULL)
            memcpy(OFFSET(buffer, b1), RDMA_QUEUE_PTR(queue, 0), b2);
        new_offset = b2;
    } else {
        assert(sz == b1);
        new_offset = queue->endpoints.rx.offset + b1;
    }
    MEM_BARRIER();
    if (dequeue) {
        queue->endpoints.rx.offset = new_offset;
    }

    return 0;
}

int rq_try_peek(struct rdma_queue* queue, void* buffer, size_t sz) {
    return rq_try_possible_dequeue(queue, buffer, sz, 0);
}

int rq_try_dequeue(struct rdma_queue* queue, void* buffer, size_t sz) {
    return rq_try_possible_dequeue(queue, buffer, sz, 1);
}

struct rdma_queue* rq_allocate_in_place(
    void* bytes, 
    int nbytes, 
    struct ibv_qp* qp, 
    int lkey, 
    int rkey) 
{
    int buffer_size = nbytes - sizeof(struct rdma_queue);
    if (buffer_size <= 0) {
        return NULL;
    }

    struct rdma_queue* queue = (struct rdma_queue*) bytes;
    memset(queue, 0, sizeof(struct rdma_queue));

    queue->buffer_size = buffer_size;
    queue->qp = qp;
    queue->endpoints.tx.addr = (uintptr_t) queue;
    queue->endpoints.tx.rkey = rkey;
    queue->endpoints.tx.lkey = lkey;
    queue->endpoints.rx = queue->endpoints.tx;
    return queue;
}

int rq_pair_receiver(struct rdma_queue* queue, struct rdma_queue_endpoint rx_endpoint) {
    queue->endpoints.rx = rx_endpoint;
    queue->endpoints.paired = 1;
    queue->is_tx = 1;
    struct ibv_send_wr wr, *bad_wr = NULL;
    struct ibv_sge sge;
    
    memset(&wr, 0, sizeof(wr));
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.wr.rdma.remote_addr = rx_endpoint.addr + offsetof(struct rdma_queue, endpoints);
    wr.wr.rdma.rkey = queue->endpoints.rx.rkey;
    
    memset(&sge, 0, sizeof(struct ibv_sge));
    assert((uintptr_t) queue == queue->endpoints.tx.addr);
    sge.addr = (uintptr_t) (&queue->endpoints);
    sge.length = sizeof(struct rdma_queue_endpoints);
    sge.lkey = queue->endpoints.tx.lkey;

    if (ibv_post_send(queue->qp, &wr, &bad_wr) != 0) {
        queue->endpoints.paired = 0;
        return -1;
    }
    return 0;
}

static inline int rq_nbytes_max_flush(struct rdma_queue* queue) {
    return rq_nbytes(queue, queue->flush_offset, queue->endpoints.rx.offset, 0);
}

ssize_t rq_nflush(struct rdma_queue* queue, size_t to_flush) {
    assert((uintptr_t) queue == queue->endpoints.tx.addr);
    
    int total_flushed = 0;
    
    size_t bytes_remaining_to_flush = rq_nbytes_to_flush(queue);
    size_t max_bytes_to_flush = bytes_remaining_to_flush > to_flush ? to_flush : bytes_remaining_to_flush;
    if (!queue->endpoints.paired) {
        uint64_t new_flush_offset = queue->flush_offset + max_bytes_to_flush;
        if (new_flush_offset > queue->buffer_size) {
            new_flush_offset -= queue->buffer_size;
        }
        queue->flush_offset = new_flush_offset;
        total_flushed = max_bytes_to_flush;
    } else {
        struct protected_address from;
        struct protected_address to;
        from.key = queue->endpoints.tx.lkey;
        to.key = queue->endpoints.rx.rkey;
        for (int i = 0; i < 2; i++) {
            uint64_t bytes_to_flush;
            uint64_t max_bytes_can_flush = rq_nbytes_max_flush(queue);
            bytes_to_flush = max_bytes_to_flush > max_bytes_can_flush ? max_bytes_can_flush : max_bytes_to_flush;

            if (bytes_to_flush > 0) {
                int b1, b2;
                
                b1 = queue->flush_offset + bytes_to_flush > queue->buffer_size ? queue->buffer_size - queue->flush_offset : bytes_to_flush;
                b2 = bytes_to_flush - b1;
                
                from.addr = (uintptr_t) RDMA_QUEUE_PTR(queue, queue->flush_offset);
                to.addr = queue->endpoints.rx.addr + offsetof(struct rdma_queue, buffer) + queue->flush_offset;
                
                if (rdma_write(queue->qp, from, to, b1) < 0) {
                    fprintf(stderr, "rq_flush: failed to flush first half (%d bytes) to remote\n", b1);
                    return -1;
                }

                if (b2 > 0) {
                    from.addr = (uintptr_t) RDMA_QUEUE_PTR(queue, 0);
                    to.addr = queue->endpoints.rx.addr + offsetof(struct rdma_queue, buffer);
                    if (rdma_write(queue->qp, from, to, b2) < 0) {
                        fprintf(stderr, "rq_flush: failed to flush second half (%d bytes) to remote\n", b1);
                        return -1;
                    }
                    queue->flush_offset = b2;
                } else {
                    queue->flush_offset += b1;
                }
            }

            total_flushed += bytes_to_flush;
            if (total_flushed == max_bytes_to_flush) {
                break;
            } else if (i == 0) {
                from.addr = (uintptr_t) &queue->endpoints.rx.offset;
                to.addr = queue->endpoints.rx.addr + offsetof(struct rdma_queue, endpoints.rx.offset);
                if (rdma_read(queue->qp, from, to, sizeof(queue->endpoints.rx.offset)) != 0) {
                    fprintf(stderr, "rq_flush: failed to read rx.offset from remote\n");
                    return -1;
                }
            }
        }

        /* update receiver */
        if (total_flushed > 0) {
            from.addr = (uintptr_t) &queue->endpoints.tx.offset;
            to.addr = queue->endpoints.rx.addr + offsetof(struct rdma_queue, endpoints.tx.offset);
            if (rdma_write(queue->qp, from, to, sizeof(queue->endpoints.tx.offset)) < 0) {
                fprintf(stderr, "rq_flush: failed to write new offset to remote\n");
            }
        }
    }
    return total_flushed;
}

ssize_t rq_flush(struct rdma_queue* queue) {
    size_t bytes_remaining_to_flush = rq_nbytes_to_flush(queue);
    return rq_nflush(queue, bytes_remaining_to_flush);
}

int rq_try_reserve_flush(struct rdma_queue* queue, size_t to_flush) {
    assert(queue->endpoints.tx.addr == (uintptr_t) queue);
    for (int i = 0; i < 2; i++) {
        uint64_t max_bytes_can_flush = rq_nbytes_max_flush(queue);
        uint64_t bytes_to_flush = rq_nbytes_to_flush(queue);
        if (max_bytes_can_flush < bytes_to_flush + to_flush) {
            if (!queue->endpoints.paired) {
                break;
            }

            if (i == 0) {
                struct protected_address from;
                struct protected_address to;
                from.key = queue->endpoints.tx.lkey;
                to.key = queue->endpoints.rx.rkey;
                from.addr = (uintptr_t) &queue->endpoints.rx.offset;
                to.addr = queue->endpoints.rx.addr + offsetof(struct rdma_queue, endpoints.rx.offset);
                if (rdma_read(queue->qp, from, to, sizeof(queue->endpoints.rx.offset)) != 0) {
                    fprintf(stderr, "rq_flush: failed to read rx.offset from remote\n");
                    return -1;
                }
            }
        } else {
            return 1;
        }
    }
    return 0;
}