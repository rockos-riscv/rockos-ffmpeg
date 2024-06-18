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

#ifndef AVCODEC_ESJDECAPI_H
#define AVCODEC_ESJDECAPI_H

#include "deccfg.h"
#include "dectypes.h"
#include "avcodec.h"
#include "hwconfig.h"
#include "internal.h"
#include "decode.h"
#include "libavutil/opt.h"
#include "libavutil/imgutils.h"
#include "es_common.h"
#include <stdlib.h>
#include <linux/limits.h>
#include "es_common.h"
#include "esdec_common.h"
#include "es_codec_private.h"
#include "libavutil/hwcontext_es.h"
#include "eslog.h"

#ifndef NEXT_MULTIPLE
#define NEXT_MULTIPLE(value, n) (((value) + (n) - 1) & ~((n) - 1))
#endif

#ifndef ALIGN
#define ALIGN(a) (1 << (a))
#endif


enum ThumbMode {
    Only_Decode_Pic = 0,
    Only_Decode_Thumb,
    Decode_Pic_Thumb
};

enum ThumbMemState {
    No_Use = 0,
    Using,
    Used
};

typedef struct _ESThumbOutputMemory {
    int flag;
    int target_pp;
    enum ThumbMemState state;
    int fd[ES_VID_DEC_MAX_OUT_COUNT];
    uint32_t *virtual_address;
    struct DWLLinearMem mem;
    struct DecPicturePpu picture;
} ESThumbOutputMemory;

typedef struct JDECContext JDECContext;

typedef int (*StoreReorderPktFunction)(JDECContext *dec_ctx, struct ReorderPkt *reorder_pkt);
typedef int (*GetReorderPktFunction)(JDECContext *dec_ctx, int pic_id, struct ReorderPkt *out_pkt);

struct JDECContext{
    const AVClass *class;
    AVPacket avpkt;
    AVFrame *frame;

    char *pp_setting[ES_VID_DEC_MAX_OUT_COUNT];
    char *scale_set;
    char * crop_set[ES_VID_DEC_MAX_OUT_COUNT];
    uint32_t stride_align;

    uint32_t packet_dump;
    char *dump_path;
    uint32_t packet_dump_time;
    DumpHandle *frame_dump_handle[ES_VID_DEC_MAX_OUT_COUNT];
    DumpHandle *pkt_dump_handle;
    uint32_t frame_dump[ES_VID_DEC_MAX_OUT_COUNT];
    uint32_t fdump;
    uint32_t frame_dump_time[ES_VID_DEC_MAX_OUT_COUNT];
    uint32_t drop_frame_interval;

    uint32_t thumb_exist;
    uint32_t thumb_done;
    uint32_t thumb_out;//whether to output thumbnails 0-output only original image 1-output only thumbnails
    uint32_t thumb_mode;
    uint32_t need_out_buf_for_thumb;
    ESThumbOutputMemory thumb_mem;

    AVCodecContext *avctx;
    AVBufferRef *hwdevice;
    AVBufferRef *hwframe;
    int extra_hw_frames;
    ESDecState state;

    DecPicAlignment align;  /* default: 64 bytes alignment */
    uint32_t prev_width;
    uint32_t prev_height;

    uint32_t output_format[ES_VID_DEC_MAX_OUT_COUNT];

    uint32_t pp_enabled;
    int32_t pp_fmt[ES_VID_DEC_MAX_OUT_COUNT];
    uint32_t cfg_pp_enabled[ES_VID_DEC_MAX_OUT_COUNT];
    uint32_t pp_out;//control output PP0 or PP
    uint32_t pp_count;

    ESJDecInst dec_inst;
    struct DWLInitParam dwl_init;
    void *dwl_inst;
    struct AVBufferRef *dwl_ref;
    int dev_mem_fd;   // for dev fd split

    uint32_t pic_display_number;
    uint32_t pic_decode_number;
    uint32_t pic_output_number;
    uint32_t got_package_number;
    uint32_t got_inputbuf_number;
    uint32_t drop_pkt_number;
    uint32_t decode_mode;
    uint32_t low_latency;
    uint32_t low_latency_sim;

    struct ESInputPort *input_port;
    struct ESOutputPort *output_port;
    struct AVBufferRef *input_port_ref;
    struct AVBufferRef *output_port_ref;

    struct ReorderPkt *reorder_pkt;
    struct ESQueue *reorder_queue;
    StoreReorderPktFunction store_reorder_pkt;
    GetReorderPktFunction get_reorder_pkt_by_pic_id;
    uint32_t pic_id;
    int64_t reordered_opaque;

    int32_t input_buf_num;
    int32_t output_buf_num;
    ESDecCodec codec;

    struct DecPicturePpu pic_out;
    struct DecPicturePpu *picture;

    struct DecConfig jdec_config;
    struct JpegDecConfig *dec_cfg;
    struct DecSequenceInfo sequence_info;

    pthread_t tid;
};

int ff_jdec_init_hwctx(AVCodecContext *avctx);

int ff_jdec_set_dec_config(JDECContext *dec_ctx);

int ff_jdec_jpegdec_init(JDECContext *dec_ctx);

int ff_jdec_decode_start(JDECContext *dec_ctx);

int ff_jdec_decode_close(JDECContext *dec_ctx);

int ff_jdec_get_frame(JDECContext *dec_ctx, AVFrame *frame, int timeout_ms);

int ff_jdec_send_packet(JDECContext *dec_ctx, AVPacket *pkt, int timeout);

int ff_jdec_send_packet_receive_frame(JDECContext *dec_ctx, AVPacket *pkt, AVFrame *frame);

#endif
