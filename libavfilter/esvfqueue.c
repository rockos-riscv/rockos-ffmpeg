#define LOG_TAG "esqueue"
#include <sys/time.h>
#include "esvfqueue.h"
#include <libavutil/log.h>
#include "esvfcommon.h"

static int32_t get_clock_time_by_timeout(struct timespec *ts, int32_t timeout_ms) {
    int64_t reltime_sec;
    int64_t time_sec;

    if (!ts || timeout_ms <= 0) {
        av_log(NULL, AV_LOG_ERROR, "error  ts: %p, timeout_ms: %d", ts, timeout_ms);
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

ESFifoQueue *esvf_fifo_queue_create(size_t nmemb, int mem_size, const char *name) {
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

void esvf_fifo_queue_free(ESFifoQueue **queue) {
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

int esvf_fifo_queue_push(ESFifoQueue *queue, void *src, int size) {
    int ret = FAILURE;
    int fifo_size;
    if (!queue || !queue->fifo || !src) {
        av_log(NULL, AV_LOG_ERROR, "params error!!! queue: %p, src: %p\n", queue, src);
        return FAILURE;
    }

    if (queue->mem_size != size) {
        av_log(NULL, AV_LOG_ERROR,
               "params error!!! size: %d, mem_size: %d\n",
               size,
               queue->mem_size);
        return FAILURE;
    }

    pthread_mutex_lock(&queue->fifo_mutex);
    fifo_size = av_fifo_space(queue->fifo);
    if (fifo_size >= size) {
        av_fifo_generic_write(queue->fifo, src, size, NULL);
        ret = SUCCESS;
        pthread_cond_signal(&queue->fifo_cond);
    } else {
        av_log(NULL, AV_LOG_ERROR, "error !!! fifo_size: %d, size: %d\n", fifo_size, size);
    }
    pthread_mutex_unlock(&queue->fifo_mutex);

    return ret;
}
int esvf_fifo_queue_pop(ESFifoQueue *queue, void *dest, int dest_size) {
    int ret = SUCCESS;
    int fifo_size;
    if (!queue || !queue->fifo || !dest || queue->mem_size != dest_size) {
        av_log(NULL, AV_LOG_ERROR, "params error!!! dest_size: %d\n", dest_size);
        return FAILURE;
    }

    pthread_mutex_lock(&queue->fifo_mutex);
    if (!queue->abort_request) {
        fifo_size = av_fifo_size(queue->fifo);
        if (fifo_size < dest_size) {
            if (fifo_size != 0) {
                av_log(NULL, AV_LOG_ERROR,
                      "error fifo_size: %d, dest_size: %d\n",
                      fifo_size,
                      dest_size);
            }
            pthread_cond_wait(&queue->fifo_cond, &queue->fifo_mutex);
            fifo_size = av_fifo_size(queue->fifo);
            if (fifo_size < dest_size) {
                av_log(NULL, AV_LOG_WARNING,
                       "wait success but fifo_size: %d error dest_size: %d\n",
                       fifo_size,
                       dest_size);
                ret = FAILURE;
            }
        }

        if (ret == SUCCESS) {
            ret = av_fifo_generic_read(queue->fifo, dest, dest_size, NULL);
        }
    } else {
        ret = FAILURE;
        av_log(NULL, AV_LOG_INFO, "pop failed %s queue abort_request\n", queue->name);
    }
    pthread_mutex_unlock(&queue->fifo_mutex);

    return ret;
}

int esvf_fifo_queue_pop_ignore_abort(ESFifoQueue *queue, void *dest, int dest_size) {
    int ret = FAILURE;
    int fifo_size;
    if (!queue || !queue->fifo || !dest || queue->mem_size != dest_size) {
        av_log(NULL, AV_LOG_ERROR, "params error!!! dest_size: %d\n", dest_size);
        return FAILURE;
    }

    pthread_mutex_lock(&queue->fifo_mutex);
    fifo_size = av_fifo_size(queue->fifo);
    av_log(NULL, AV_LOG_INFO, "fifo_size: %d, dest_size: %d\n", fifo_size, dest_size);
    if (fifo_size >= dest_size) {
        ret = SUCCESS;
        av_fifo_generic_read(queue->fifo, dest, dest_size, NULL);
    }
    pthread_mutex_unlock(&queue->fifo_mutex);

    return ret;
}

int esvf_fifo_queue_pop_until_timeout(ESFifoQueue *queue, void *dest, int dest_size, int timeout_ms) {
    int ret = SUCCESS;
    int fifo_size;
    if (!queue || !queue->fifo || !dest || queue->mem_size != dest_size) {
        av_log(NULL, AV_LOG_ERROR, "params error!!! dest_size: %d\n", dest_size);
        return FAILURE;
    }

    pthread_mutex_lock(&queue->fifo_mutex);
    if (!queue->abort_request) {
        fifo_size = av_fifo_size(queue->fifo);
        if (fifo_size < dest_size) {
            if (fifo_size != 0) {
                av_log(NULL, AV_LOG_ERROR,
                      "error fifo_size: %d,dest_size: %d\n",
                      fifo_size,
                      dest_size);
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
                    av_log(NULL, AV_LOG_WARNING,
                           "wait success but fifo_size: %d error dest_size: %d\n",
                           fifo_size,
                           dest_size);
                    ret = FAILURE;
                }
            }
        }
        if (ret == SUCCESS) {
            ret = av_fifo_generic_read(queue->fifo, dest, dest_size, NULL);
        }
    } else {
        ret = FAILURE;
        av_log(NULL, AV_LOG_INFO, "pop failed %s queue abort_request\n", queue->name);
    }
    pthread_mutex_unlock(&queue->fifo_mutex);

    return ret;
}

void esvf_fifo_queue_abort(ESFifoQueue *queue) {
    if (!queue) {
        return;
    }
    pthread_mutex_lock(&queue->fifo_mutex);
    queue->abort_request = TRUE;
    pthread_cond_broadcast(&queue->fifo_cond);
    av_log(NULL, AV_LOG_INFO, "%s request abort\n", queue->name);
    pthread_mutex_unlock(&queue->fifo_mutex);
}
