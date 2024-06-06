/*
 * Copyright (C) 2019  VeriSilicon
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include "esenc_common.h"
#include "codec_internal.h"

static int esenc_fifo_init(FifoQueue *fifo_queue, unsigned nmemb, unsigned task_size) {
    if (!fifo_queue || !nmemb || !task_size) return -1;

    fifo_queue->fifo = av_fifo_alloc_array(nmemb, task_size);
    if (!fifo_queue->fifo) return -1;

    fifo_queue->cur_index = 0;
    fifo_queue->depth = nmemb;
    fifo_queue->task_size = task_size;

    pthread_mutex_init(&fifo_queue->fifo_mutex, NULL);
    pthread_cond_init(&fifo_queue->fifo_cond, NULL);
    pthread_cond_init(&fifo_queue->fifo_writable_cond, NULL);

    return 0;
}

static int esenc_fifo_deinit(FifoQueue *fifo_queue) {
    if (!fifo_queue) return -1;
    pthread_mutex_destroy(&fifo_queue->fifo_mutex);
    pthread_cond_destroy(&fifo_queue->fifo_cond);
    pthread_cond_destroy(&fifo_queue->fifo_writable_cond);
    av_fifo_freep(&fifo_queue->fifo);
    fifo_queue->fifo = NULL;

    return 0;
}

static int esenc_fifo_push(FifoQueue *fifo_queue, FifoTask *task) {
    if (!fifo_queue || !task) return -1;

    pthread_mutex_lock(&fifo_queue->fifo_mutex);
    if (!av_fifo_space(fifo_queue->fifo)) {
        pthread_cond_wait(&fifo_queue->fifo_writable_cond, &fifo_queue->fifo_mutex);
    }
    av_fifo_generic_write(fifo_queue->fifo, task, sizeof(FifoTask), NULL);
    pthread_mutex_unlock(&fifo_queue->fifo_mutex);
    pthread_cond_signal(&fifo_queue->fifo_cond);

    return 0;
}

static int esenc_fifo_pop(FifoQueue *fifo_queue, FifoTask *task) {
    if (!fifo_queue || !task) return -1;

    pthread_mutex_lock(&fifo_queue->fifo_mutex);
    av_fifo_generic_read(fifo_queue->fifo, task, sizeof(FifoTask), NULL);
    pthread_mutex_unlock(&fifo_queue->fifo_mutex);
    pthread_cond_signal(&fifo_queue->fifo_writable_cond);

    return 0;
}

static int esenc_fifo_size(FifoQueue *fifo_queue) {
    int ret = 0;
    if (!fifo_queue) return -1;

    pthread_mutex_lock(&fifo_queue->fifo_mutex);
    ret = av_fifo_size(fifo_queue->fifo);
    pthread_mutex_unlock(&fifo_queue->fifo_mutex);

    return ret;
}

static int esenc_fifo_wait(FifoQueue *fifo_queue) {
    int ret = 0;
    if (!fifo_queue) return -1;

    pthread_mutex_lock(&fifo_queue->fifo_mutex);
    ret = pthread_cond_wait(&fifo_queue->fifo_cond, &fifo_queue->fifo_mutex);
    pthread_mutex_unlock(&fifo_queue->fifo_mutex);

    return ret;
}

static int esenc_fifo_signal(FifoQueue *fifo_queue) {
    int ret = 0;
    if (!fifo_queue) return -1;

    pthread_mutex_lock(&fifo_queue->fifo_mutex);
    ret = pthread_cond_signal(&fifo_queue->fifo_cond);
    pthread_mutex_unlock(&fifo_queue->fifo_mutex);

    return ret;
}

static int esenc_fifo_broadcast(FifoQueue *fifo_queue) {
    int ret = 0;
    if (!fifo_queue) return -1;

    pthread_mutex_lock(&fifo_queue->fifo_mutex);
    ret = pthread_cond_broadcast(&fifo_queue->fifo_cond);
    pthread_mutex_unlock(&fifo_queue->fifo_mutex);

    return ret;
}

static void esenc_thread_free(void *tc);

static void *attribute_align_arg esenc_thread_worker(void *v) {
    ThreadContext *c = (ThreadContext *)v;
    AVPacket *pkt = NULL;
    AVCodecContext *avctx = c->avctx;

    av_log(NULL, AV_LOG_INFO, "encoder worker in\n");

    while (!atomic_load(&c->exit)) {
        int got_packet, ret;
        AVFrame *frame;
        FifoTask task;
        FifoTask out_task;
        int in_frame_fifo_size = 0;

        while ((in_frame_fifo_size = esenc_fifo_size(&c->in_frame_queue)) <= 0 && !atomic_load(&c->exit)) {
            if (in_frame_fifo_size < 0) return NULL;
            esenc_fifo_wait(&c->in_frame_queue);
        }

        // av_log(NULL, AV_LOG_INFO, "encoder worker fifo size: %d\n", in_frame_fifo_size);
        if (in_frame_fifo_size > 0) {
            // streaming packet
            if (!pkt) pkt = av_packet_alloc();
            if (!pkt) continue;
            av_init_packet(pkt);

            esenc_fifo_pop(&c->in_frame_queue, &task);
            frame = (AVFrame *)task.buf;
            av_log(avctx,
                   AV_LOG_DEBUG,
                   "worker, get one frame: %p, pts = %" PRId64 ", dts = %" PRId64 " from input queue\n",
                   frame,
                   frame ? frame->pts : AV_NOPTS_VALUE,
                   frame ? frame->pkt_dts : AV_NOPTS_VALUE);

            // FFmpeg5.1.2 fixed:
            ret = c->es_encode_func(avctx, pkt, frame, &got_packet);
            // ret = avctx->codec->encode2(avctx, pkt, frame, &got_packet);
            if (got_packet) {
                av_packet_make_refcounted(pkt);
                // put into out_packet_queue
                out_task.buf = (void *)pkt;
                out_task.index = c->out_packet_queue.cur_index;
                esenc_fifo_push(&c->out_packet_queue, &out_task);
                c->out_packet_queue.cur_index = (c->out_packet_queue.cur_index + 1) % OUT_PACKET_FIFO_DEPTH;
                av_log(NULL, AV_LOG_DEBUG, "worker, push one packet to output queue\n");
            } else {
                av_packet_free(&pkt);
            }
            pkt = NULL;

            // av_frame_unref(frame);
            av_frame_free(&frame);

            if (ret == AVERROR_EOF) {
                c->is_eof = 1;
                av_log(NULL, AV_LOG_INFO, "encoder worker is_eof = 1\n");
                esenc_fifo_broadcast(&c->out_packet_queue);
            } else {
                c->is_eof = 0;
            }
        }
    }

    av_log(NULL, AV_LOG_INFO, "encoder worker exit\n");
    return NULL;
}

static int esenc_thread_init(
    void **tc,
    AVCodecContext *avctx,
    int (*es_encode_func)(struct AVCodecContext *, struct AVPacket *, const struct AVFrame *, int *)) {
    ThreadContext *c = (ThreadContext *)av_mallocz(sizeof(ThreadContext));
    *tc = c;

    if (!c) return AVERROR(ENOMEM);

    c->avctx = avctx;
    c->es_encode_func = es_encode_func;

    if (esenc_fifo_init(&c->in_frame_queue, IN_FRMAE_FIFO_DEPTH, sizeof(FifoTask))) {
        goto fail;
    }

    if (esenc_fifo_init(&c->out_packet_queue, OUT_PACKET_FIFO_DEPTH, sizeof(FifoTask))) {
        goto fail;
    }

    atomic_init(&c->exit, 0);

    if (pthread_create(&c->worker, NULL, esenc_thread_worker, (void *)c)) {
        goto fail;
    }

    return 0;

fail:
    av_log(NULL, AV_LOG_ERROR, "%s failed\n", __FUNCTION__);
    return -1;
}

static void esenc_thread_free(void *tc) {
    ThreadContext *c = (ThreadContext *)tc;

    atomic_store(&c->exit, 1);
    esenc_fifo_broadcast(&c->in_frame_queue);
    esenc_fifo_broadcast(&c->out_packet_queue);
    pthread_join(c->worker, NULL);

    while (esenc_fifo_size(&c->in_frame_queue) > 0) {
        FifoTask task;
        AVFrame *frame;
        esenc_fifo_pop(&c->in_frame_queue, &task);
        frame = (AVFrame *)task.buf;
        av_frame_free(&frame);
        task.buf = NULL;
    }

    while (esenc_fifo_size(&c->out_packet_queue) > 0) {
        FifoTask task;
        AVPacket *pkt;
        esenc_fifo_pop(&c->out_packet_queue, &task);
        pkt = (AVPacket *)task.buf;
        av_packet_free(&pkt);
        task.buf = NULL;
    }

    esenc_fifo_deinit(&c->in_frame_queue);
    esenc_fifo_deinit(&c->out_packet_queue);
    av_freep(&tc);
}

int esenc_send_frame(void *tc, const AVFrame *frame) {
    ThreadContext *c = (ThreadContext *)tc;
    FifoTask task;
    AVFrame *new_frame = NULL;;

    if (frame && !c->enc_flushing) {
        new_frame = av_frame_alloc();
        if (!new_frame) {
            av_log(NULL, AV_LOG_ERROR, "%s av_frame_alloc failed\n", __FUNCTION__);
            return AVERROR(ENOMEM);
        }
        av_frame_move_ref(new_frame, frame);
    } else {
        if (!c->enc_flushing) c->enc_flushing = 1;
    }

    task.index = c->in_frame_queue.cur_index;
    task.buf = (void *)new_frame;
    esenc_fifo_push(&c->in_frame_queue, &task);
    c->in_frame_queue.cur_index = (c->in_frame_queue.cur_index + 1) % IN_FRMAE_FIFO_DEPTH;

    return 0;
}

int esenc_receive_packet(void *tc, AVPacket *pkt) {
    ThreadContext *c = (ThreadContext *)tc;
    FifoTask task;
    int size = 0;

    while ((size = esenc_fifo_size(&c->out_packet_queue)) <= 0 && !atomic_load(&c->exit)) {
        if (size < 0) {
            return AVERROR(EINVAL);
        } else if (!size) {
            if (c->is_eof) {
                av_log(NULL, AV_LOG_INFO, "receive_packet is_eof\n");
                return AVERROR_EOF;
            } else if (!c->avctx->internal->draining) {
                return AVERROR(EAGAIN);
            } else {
                av_log(NULL, AV_LOG_DEBUG, "receive pkt, wait\n");
                if (!c->is_eof) esenc_fifo_wait(&c->out_packet_queue);
            }
        }
    }

    // get streaming form queue
    if (esenc_fifo_pop(&c->out_packet_queue, &task) < 0) {
        return AVERROR(EINVAL);
    }

    if (pkt) {
        *pkt = *(AVPacket *)(task.buf);
    }

    if (pkt->size) av_log(NULL, AV_LOG_DEBUG, "%s, size: %d out\n", __FUNCTION__, pkt->size);

    return 0;
}

int esenc_init(void **tc,
               AVCodecContext *avctx,
               int (*es_encode_func)(struct AVCodecContext *, struct AVPacket *, const struct AVFrame *, int *)) {
    int ret;

    ret = esenc_thread_init(tc, avctx, es_encode_func);

    return ret;
}

void esenc_close(void *tc) {
    esenc_thread_free(tc);
}
