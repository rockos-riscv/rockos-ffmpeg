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

#ifndef AVCODEC_ESENC_COMMON_H
#define AVCODEC_ESENC_COMMON_H

#include <stdatomic.h>
#include "libavutil/fifo.h"
#include "libavutil/avassert.h"
#include "libavutil/imgutils.h"
#include "libavutil/thread.h"
#include "libavutil/eval.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "libavutil/stereo3d.h"
#include "libavutil/intreadwrite.h"
#include "avcodec.h"
#include "internal.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_es.h"

#define IN_FRMAE_FIFO_DEPTH (15)
#define OUT_PACKET_FIFO_DEPTH IN_FRMAE_FIFO_DEPTH

typedef struct {
    void *buf;
    unsigned index;
} FifoTask;

typedef struct {
    unsigned cur_index;
    unsigned depth;
    unsigned task_size;
    AVFifoBuffer *fifo;
    pthread_mutex_t fifo_mutex;
    pthread_cond_t fifo_cond;
    pthread_cond_t fifo_writable_cond;
} FifoQueue;

typedef struct {
    AVCodecContext *avctx;
    // input frame queue
    FifoQueue in_frame_queue;
    // output packet queue
    FifoQueue out_packet_queue;

    pthread_t worker;
    atomic_int exit;
    uint8_t enc_flushing;
    uint8_t is_eof;

    /**
     * Encode data to an AVPacket.
     *
     * @param      avctx          codec context
     * @param      avpkt          output AVPacket
     * @param[in]  frame          AVFrame containing the raw data to be encoded
     * @param[out] got_packet_ptr encoder sets to 0 or 1 to indicate that a
     *                            non-empty packet was returned in avpkt.
     * @return 0 on success, negative error code on failure
     */
    int (*es_encode_func)(struct AVCodecContext *avctx,
                          struct AVPacket *avpkt,
                          const struct AVFrame *frame,
                          int *got_packet_ptr);
} ThreadContext;

int esenc_send_frame(void *tc, const AVFrame *frame);

int esenc_receive_packet(void *tc, AVPacket *avpkt);

int esenc_init(void **tc,
               AVCodecContext *avctx,
               int (*es_encode_func)(struct AVCodecContext *, struct AVPacket *, const struct AVFrame *, int *));

void esenc_close(void *tc);
#endif
