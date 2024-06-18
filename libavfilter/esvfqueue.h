#ifndef AVCODEC_ESQUEUE_H_
#define AVCODEC_ESQUEUE_H_
#include "libavutil/fifo.h"
#include "libavutil/thread.h"

typedef struct ESFifoQueue {
    int abort_request;
    int mem_size;
    int nmemb;
    AVFifoBuffer *fifo;
    char *name;
    pthread_mutex_t fifo_mutex;
    pthread_cond_t fifo_cond;
} ESFifoQueue;

ESFifoQueue *esvf_fifo_queue_create(size_t nmemb, int mem_size, const char *name);
void esvf_fifo_queue_free(ESFifoQueue **queue);
int esvf_fifo_queue_push(ESFifoQueue *queue, void *src, int src_size);
int esvf_fifo_queue_pop(ESFifoQueue *queue, void *dest, int dest_size);
int esvf_fifo_queue_pop_ignore_abort(ESFifoQueue *queue, void *dest, int dest_size);
void esvf_fifo_queue_abort(ESFifoQueue *queue);
void esvf_fifo_queue_deinit(ESFifoQueue **queue);
int esvf_fifo_queue_pop_until_timeout(ESFifoQueue *queue,
                                      void *dest,
                                      int dest_size,
                                      int timeout_ms);

#endif