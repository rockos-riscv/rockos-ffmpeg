#ifndef AVCODEC_ESQUEUE_H_
#define AVCODEC_ESQUEUE_H_
#include "libavutil/fifo.h"
#include "libavutil/thread.h"

typedef struct _List List;
typedef struct ESQueue ESQueue;

struct _List {
    void *data;
    List *next;
    List *prev;
};

struct ESQueue {
    List *head;
    List *tail;
    int length;
};

ESQueue *es_queue_create(void);
int es_queue_destroy(ESQueue *q);
void es_queue_init(ESQueue *q);
void *es_queue_peek_head(ESQueue *q);
void *es_queue_pop_head(ESQueue *q);
int es_queue_is_empty(ESQueue *q);
int es_queue_get_length(ESQueue *q);
void *es_queue_peek_tail(ESQueue *q);
int es_queue_push_tail(ESQueue *q, void *data);
int es_queue_push_front(ESQueue *q, void *data);
int es_queue_delete_data(ESQueue *q, void *data);

/*************************************************************************************
 *  fifo queue
 *************************************************************************************/
typedef struct ESFifoQueue {
    int abort_request;
    int mem_size;
    int nmemb;
    AVFifoBuffer *fifo;
    char *name;
    pthread_mutex_t fifo_mutex;
    pthread_cond_t fifo_cond;
} ESFifoQueue;

ESFifoQueue *es_fifo_queue_create(size_t nmemb, int mem_size, const char *name);
int es_fifo_queue_enlarge(ESFifoQueue *queue, size_t nmemb, int mem_size);
void es_fifo_queue_free(ESFifoQueue **queue);
int es_fifo_queue_push(ESFifoQueue *queue, void *src, int src_size);
int es_fifo_queue_pop(ESFifoQueue *queue, void *dest, int dest_size);
int es_fifo_queue_pop_ignore_abort(ESFifoQueue *queue, void *dest, int dest_size);
void es_fifo_queue_abort(ESFifoQueue *queue);
void es_fifo_queue_start(ESFifoQueue *queue);
void es_fifo_queue_deinit(ESFifoQueue **queue);
int es_fifo_queue_pop_until_timeout(ESFifoQueue *queue, void *dest, int dest_size, int timeout_ms);

int32_t get_clock_time_by_timeout(struct timespec *ts, int32_t timeout_ms);

#endif