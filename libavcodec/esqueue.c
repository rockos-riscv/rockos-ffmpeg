#define LOG_TAG "esqueue"
#include <sys/time.h>
#include "esqueue.h"
#include "eslog.h"
#include "es_common.h"

int32_t get_clock_time_by_timeout(struct timespec *ts, int32_t timeout_ms) {
    int64_t reltime_sec;
    int64_t time_sec;

    if (!ts || timeout_ms <= 0) {
        log_error(NULL, "error  ts: %p, timeout_ms: %d", ts, timeout_ms);
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, ts);

    reltime_sec = timeout_ms / 1000;
    ts->tv_nsec += (long)(timeout_ms % 1000 * 1000000);
    if (reltime_sec < INT64_MAX && ts->tv_nsec >= 1000000000) {
        ts->tv_nsec -= 1000000000;
        ++reltime_sec;
    }

    time_sec = ts->tv_sec;
    if (time_sec > INT64_MAX - reltime_sec) {
        time_sec = INT64_MAX;
    } else {
        time_sec += reltime_sec;
    }

    ts->tv_sec = (time_sec > LONG_MAX) ? LONG_MAX : (long)(time_sec);

    return 0;
}

void es_queue_init(ESQueue *q) {
    if (q == NULL) {
        return;
    }
    q->head = q->tail = NULL;
    q->length = 0;
}

ESQueue *es_queue_create() {
    ESQueue *q = (ESQueue *)malloc(sizeof(ESQueue));
    if (q) {
        es_queue_init(q);
    }

    return q;
}

int es_queue_destroy(ESQueue *q) {
    if (!q) return -1;

    free(q);
    q = NULL;
    return 0;
}

void *es_queue_peek_head(ESQueue *q) {
    void *data = NULL;
    if (q == NULL) {
        return NULL;
    }
    if (q->head) {
        data = q->head->data;
    }

    return data;
}

void *es_queue_pop_head(ESQueue *q) {
    void *data = NULL;
    if (q == NULL) {
        return NULL;
    }

    if (q->head) {
        List *node = q->head;
        data = node->data;
        q->head = node->next;
        if (q->head) {
            q->head->prev = NULL;
        } else {
            q->tail = NULL;
        }
        q->length--;
        free(node);
    }

    return data;
}

static List *es_list_append(List *list, void *data) {
    List *new_list;
    new_list = (List *)malloc(sizeof(List));
    if (new_list == NULL) {
        return NULL;
    }

    new_list->data = data;
    new_list->next = NULL;
    if (list) {
        list->next = new_list;
        new_list->prev = list;
    } else {
        new_list->prev = NULL;
    }
    return new_list;
}

void *es_queue_peek_tail(ESQueue *q) {
    void *data = NULL;
    if (q == NULL) {
        return NULL;
    }
    if (q->tail) {
        data = q->tail->data;
    }

    return data;
}

int es_queue_push_tail(ESQueue *q, void *data) {
    if (q == NULL) {
        return -1;
    }

    q->tail = es_list_append(q->tail, data);
    if (q->tail == NULL) {
        return -1;
    }

    if (!q->tail->prev) {
        q->head = q->tail;
    }
    q->length++;

    return 0;
}

int es_queue_push_front(ESQueue *q, void *data) {
    List *new_list;
    if (!q) {
        return -1;
    }

    new_list = (List *)malloc(sizeof(List));
    if (!new_list) {
        return -1;
    }

    new_list->data = data;
    new_list->next = NULL;
    new_list->prev = NULL;

    if (!q->head) {
        q->tail = q->head = new_list;
    } else {
        new_list->next = q->head;
        q->head->prev = new_list;
        q->head = new_list;
    }
    q->length++;

    return 0;
}

int es_queue_is_empty(ESQueue *q) {
    int ret = -1;

    if (q == NULL || q->head == NULL) {
        ret = 0;
    }

    return ret;
}

int es_queue_get_length(ESQueue *q) {
    int length = 0;
    if (q) {
        length = q->length;
    }

    return length;
}

static void es_queue_delete_node(ESQueue *q, List *node) {
    if (!q || !node) {
        return;
    }

    if (node->prev) {
        node->prev->next = node->next;
    } else {
        q->head = node->next;
    }

    if (node->next) {
        node->next->prev = node->prev;
    } else {
        q->tail = node->prev;
    }

    free(node);
}

int es_queue_delete_data(ESQueue *q, void *data) {
    int ret = -1;
    List *node = NULL;
    if (q == NULL || q->head == NULL) {
        return -1;
    }

    node = q->head;
    while (node) {
        if (node->data == data) {
            ret = 0;
            es_queue_delete_node(q, node);
            break;
        } else {
            node = node->next;
        }
    }

    return ret;
}

/*************************************************************************************
 *  fifo queue
 *************************************************************************************/

ESFifoQueue *es_fifo_queue_create(size_t nmemb, int mem_size, const char *name) {
    pthread_condattr_t attr;
    ESFifoQueue *queue;
    if (nmemb <= 0 || mem_size < 0) {
        return NULL;
    }

    queue = av_mallocz(sizeof(*queue));
    if (!queue) {
        return NULL;
    }

    queue->fifo = av_fifo_alloc_array(nmemb, mem_size);
    if (!queue->fifo) {
        av_freep(&queue);
        return NULL;
    }
    queue->mem_size = mem_size;
    queue->nmemb = nmemb;
    queue->name = av_strdup(name);

    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&queue->fifo_cond, &attr);
    pthread_mutex_init(&queue->fifo_mutex, NULL);

    return queue;
}

int es_fifo_queue_enlarge(ESFifoQueue *queue, size_t nmemb, int mem_size) {
    pthread_condattr_t attr;
    int ret = FAILURE;
    int rv = 0;

    if (!queue || !queue->fifo) {
        return ret;
    }

    if (nmemb < 0) {
        return ret;
    }

    pthread_mutex_lock(&queue->fifo_mutex);
    rv = av_fifo_realloc2(queue->fifo, nmemb*mem_size);
    if (rv >= 0) {
        ret = SUCCESS;
        pthread_cond_signal(&queue->fifo_cond);
    } else {
        log_error(NULL, "error !!! enlarge fifo space, nmemb: %d\n", nmemb);
    }
    pthread_mutex_unlock(&queue->fifo_mutex);

    return ret;
}

void es_fifo_queue_free(ESFifoQueue **queue) {
    ESFifoQueue *q;
    if (!queue || !*queue) {
        return;
    }

    q = *queue;
    av_free(q->name);
    pthread_mutex_destroy(&q->fifo_mutex);
    pthread_cond_destroy(&q->fifo_cond);
    av_fifo_freep(&q->fifo);
    av_freep(queue);
}

int es_fifo_queue_push(ESFifoQueue *queue, void *src, int size) {
    int ret = FAILURE;
    int fifo_size;
    if (!queue || !queue->fifo || !src) {
        log_error(NULL, "params error!!! queue: %p, src: %p\n", queue, src);
        return FAILURE;
    }

    if (queue->mem_size != size) {
        log_error(NULL, "params error!!! size: %d, mem_size: %d\n", size, queue->mem_size);
        return FAILURE;
    }

    pthread_mutex_lock(&queue->fifo_mutex);
    fifo_size = av_fifo_space(queue->fifo);
    if (fifo_size >= size) {
        av_fifo_generic_write(queue->fifo, src, size, NULL);
        ret = SUCCESS;
        pthread_cond_signal(&queue->fifo_cond);
    } else {
        log_error(NULL, "error !!! fifo_size: %d, size: %d\n", fifo_size, size);
    }
    pthread_mutex_unlock(&queue->fifo_mutex);

    return ret;
}
int es_fifo_queue_pop(ESFifoQueue *queue, void *dest, int dest_size) {
    int ret = SUCCESS;
    int fifo_size;
    if (!queue || !queue->fifo || !dest || queue->mem_size != dest_size) {
        log_error(NULL, "params error!!! dest_size: %d\n", dest_size);
        return FAILURE;
    }

    pthread_mutex_lock(&queue->fifo_mutex);
    if (!queue->abort_request) {
        fifo_size = av_fifo_size(queue->fifo);
        if (fifo_size < dest_size) {
            if (fifo_size != 0) {
                log_error(NULL, "error fifo_size: %d, dest_size: %d\n", fifo_size, dest_size);
            }
            pthread_cond_wait(&queue->fifo_cond, &queue->fifo_mutex);
            fifo_size = av_fifo_size(queue->fifo);
            if (fifo_size < dest_size) {
                log_warn(NULL, "wait success but fifo_size: %d error dest_size: %d\n", fifo_size, dest_size);
                ret = FAILURE;
            }
        }

        if (ret == SUCCESS) {
            ret = av_fifo_generic_read(queue->fifo, dest, dest_size, NULL);
        }
    } else {
        ret = FAILURE;
        log_info(NULL, "pop failed %s queue abort_request\n", queue->name);
    }
    pthread_mutex_unlock(&queue->fifo_mutex);

    return ret;
}

int es_fifo_queue_pop_ignore_abort(ESFifoQueue *queue, void *dest, int dest_size) {
    int ret = FAILURE;
    int fifo_size;
    if (!queue || !queue->fifo || !dest || queue->mem_size != dest_size) {
        log_error(NULL, "params error!!! dest_size: %d\n", dest_size);
        return FAILURE;
    }

    pthread_mutex_lock(&queue->fifo_mutex);
    fifo_size = av_fifo_size(queue->fifo);
    log_info(NULL, "fifo_size: %d, dest_size: %d\n", fifo_size, dest_size);
    if (fifo_size >= dest_size) {
        ret = SUCCESS;
        av_fifo_generic_read(queue->fifo, dest, dest_size, NULL);
    }
    pthread_mutex_unlock(&queue->fifo_mutex);

    return ret;
}

int es_fifo_queue_pop_until_timeout(ESFifoQueue *queue, void *dest, int dest_size, int timeout_ms) {
    int ret = SUCCESS;
    int fifo_size;
    if (!queue || !queue->fifo || !dest || queue->mem_size != dest_size) {
        log_error(NULL, "params error!!! dest_size: %d\n", dest_size);
        return FAILURE;
    }

    pthread_mutex_lock(&queue->fifo_mutex);
    if (!queue->abort_request) {
        fifo_size = av_fifo_size(queue->fifo);
        if (fifo_size < dest_size) {
            if (fifo_size != 0) {
                log_error(NULL, "error fifo_size: %d, dest_size: %d\n", fifo_size, dest_size);
            }
            if (timeout_ms == -1) {
                pthread_cond_wait(&queue->fifo_cond, &queue->fifo_mutex);
            } else if (timeout_ms > 0) {
                struct timespec ts;
                get_clock_time_by_timeout(&ts, timeout_ms);
                ret = pthread_cond_timedwait(&queue->fifo_cond, &queue->fifo_mutex, &ts);
            } else {
                ret = FAILURE;
            }

            if (ret == SUCCESS) {
                fifo_size = av_fifo_size(queue->fifo);
                if (fifo_size < dest_size) {
                    log_warn(NULL, "wait success but fifo_size: %d error dest_size: %d\n", fifo_size, dest_size);
                    ret = FAILURE;
                }
            }
        }
        if (ret == SUCCESS) {
            ret = av_fifo_generic_read(queue->fifo, dest, dest_size, NULL);
        }
    } else {
        ret = FAILURE;
        log_info(NULL, "pop %s queue abort_request\n", queue->name);
    }
    pthread_mutex_unlock(&queue->fifo_mutex);

    return ret;
}

void es_fifo_queue_abort(ESFifoQueue *queue) {
    if (!queue) {
        return;
    }
    pthread_mutex_lock(&queue->fifo_mutex);
    queue->abort_request = TRUE;
    pthread_cond_broadcast(&queue->fifo_cond);
    log_info(NULL, "%s request abort\n", queue->name);
    pthread_mutex_unlock(&queue->fifo_mutex);
}

void es_fifo_queue_start(ESFifoQueue *queue) {
    if (!queue) {
        return;
    }
    pthread_mutex_lock(&queue->fifo_mutex);
    queue->abort_request = FALSE;
    pthread_cond_broadcast(&queue->fifo_cond);
    log_info(NULL, "%s request start\n", queue->name);
    pthread_mutex_unlock(&queue->fifo_mutex);
}
