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

#include "esjdecapi.h"
#include "jpegdecapi.h"
#include "encode.h"
#include "codec_internal.h"

#define NEXT_MULTIPLE(value, n) (((value) + (n) - 1) & ~((n) - 1))
#define ALIGN(a) (1 << (a))

static av_cold int ff_es_jpeg_decode_close(AVCodecContext *avctx)
{
    JDECContext *dec_ctx = avctx->priv_data;
    int i;

    log_info(avctx, "Es jpeg decode close.....\n");

    if (dec_ctx->state != ESDEC_STATE_CLOSED) {
        dec_ctx->state = ESDEC_STATE_CLOSED;
        ff_jdec_decode_close(dec_ctx);
    }

    if (dec_ctx->pkt_dump_handle)
        ff_codec_dump_file_close(&dec_ctx->pkt_dump_handle);

    for ( int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if(dec_ctx->frame_dump_handle[i]) {
            ff_codec_dump_file_close(&dec_ctx->frame_dump_handle[i]);
        }
    }

    return 0;
}

static av_cold int ff_es_jpeg_decode_init(AVCodecContext *avctx)
{
    JDECContext *dec_ctx = avctx->priv_data;
    JpegDecApiVersion dec_api;
    enum DecRet rv;
    int ret = 0;
    dec_ctx->avctx = avctx;

    dec_api = JpegGetAPIVersion();
    log_info(avctx, "Jpeg Decoder API v%d.%d\n", dec_api.major, dec_api.minor);

    ret = ff_jdec_init_hwctx(avctx);
    if (ret < 0) {
        log_error(dec_ctx, "esjdec_init_hwctx failed\n");
        return ret;
    } else {
        log_info(avctx, "esjdec_init_hwctx ok\n");
    }

    ret = ff_jdec_set_dec_config(dec_ctx);
    if (ret < 0){
        log_error(dec_ctx, "esjdec_set_dec_config failed\n");
        return ret;
    } else {
        log_info(avctx, "esjdec_set_dec_config success\n");
    }

    rv = ff_jdec_jpegdec_init(dec_ctx);
    if (rv < 0) {
        log_error(dec_ctx, "esjdec_jpegdec_init failed!\n");
         return FAILURE;
    } else {
        log_info(avctx, "esjdec_jpegdec_init success!\n");
    }

    rv = ff_jdec_decode_start(dec_ctx);
    if (rv < 0) {
        log_error(dec_ctx, "esjdec_decode_start failed!\n");
         return FAILURE;
    } else {
        dec_ctx->state = ESDEC_STATE_STARTED;
        avctx->apply_cropping = 0;//for RGB format can output
        log_info(avctx, "esjdec_decode_start success!\n");
    }

    return SUCCESS;
}

static int ff_es_jpeg_eof_proc(JDECContext *dec_ctx, AVFrame *frame) {
    int ret;
    if (!dec_ctx) {
        return AVERROR(EINVAL);
    }

    log_info(dec_ctx, "dec_ctx->state: %d\n", dec_ctx->state);
    if (dec_ctx->state == ESDEC_STATE_STARTED) {
        ret = ff_jdec_send_packet(dec_ctx, NULL, -1 /*always waiting*/);
        if (ret == SUCCESS) {
            dec_ctx->state = ESDEC_STATE_STOPPING;
            log_info(dec_ctx, "send eos packet success enter stopping\n");
        } else {
            dec_ctx->state = ESDEC_STATE_STOPPED;
            log_info(dec_ctx, "dec_ctx->state: %d\n", dec_ctx->state);
        }
    }

    if (dec_ctx->state == ESDEC_STATE_STOPPING) {
        ret = ff_jdec_get_frame(dec_ctx, frame, -1 /* always waiting */);
        if (ret == AVERROR_EOF || ret == AVERROR_EXIT) {
            dec_ctx->state = ESDEC_STATE_STOPPED;
            log_info(dec_ctx, "recv eof decode enter stopped\n");
        } else {
            log_info(dec_ctx, "decode state stopping get frame ret: %d\n", ret);
        }
    }

    return ret;
}

static int ff_es_jpeg_decode_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    int ret = SUCCESS;
    JDECContext *dec_ctx;
    AVPacket *avpkt;
    AVFrame *tmp_frame;

    if (!avctx || !avctx->priv_data || !frame) {
        log_error(dec_ctx, "avctx or private or frame is null avctx: %p, frame: %p\n", avctx, frame);
        return AVERROR(EINVAL);
    }

    dec_ctx = (JDECContext *)avctx->priv_data;
    avpkt = &dec_ctx->avpkt;
    tmp_frame = dec_ctx->frame;

    if (dec_ctx->state == ESDEC_STATE_UNINIT) {
        log_error(dec_ctx, "dec_ctx state: %d error\n", dec_ctx->state);
        return AVERROR(EINVAL);
    } else if (dec_ctx->state == ESDEC_STATE_STOPPED || dec_ctx->state == ESDEC_STATE_CLOSED) {
        log_warn(dec_ctx, "dec_ctx state: %d closed\n", dec_ctx->state);
        return AVERROR_EOF;
    }

    do {
        if (dec_ctx->state == ESDEC_STATE_STOPPING) {
            ret = ff_es_jpeg_eof_proc(dec_ctx, frame);
            break;
        }

        if (!avpkt->data || avpkt->size <= 0) {
            ret = ff_decode_get_packet(avctx, avpkt);
            log_debug(dec_ctx, "ff_decode_get_packet ret: 0x%x, size: %d\n", ret, avpkt->size);
            if (ret == SUCCESS) {
                dec_ctx->got_package_number ++;
                dec_ctx->reordered_opaque = avctx->reordered_opaque;
                log_debug(dec_ctx, "ff_decode_get_packet success\n");
            }
        } else {
            log_debug(dec_ctx, "use last pkt size: %d\n", avpkt->size);
        }

        if (ret == AVERROR(EAGAIN)) {
            ret = ff_jdec_get_frame(dec_ctx, frame, 0 /*without waiting*/);
            if (ret == FAILURE) {
                ret = AVERROR(EAGAIN);
                log_debug(dec_ctx, "EAGAIN ret: %d\n", ret);
            }
            break;
        } else if (ret == AVERROR_EOF) {
            ret = ff_es_jpeg_eof_proc(dec_ctx, frame);
            break;
        } else if (ret < 0) {
            log_error(dec_ctx, "ff_decode_get_packet failed ret: 0x%x\n", ret);
            break;
        }

        ret = ff_jdec_send_packet_receive_frame(dec_ctx, avpkt, frame);
    } while (0);

    if (ret == SUCCESS) {
        if (avctx->width != frame->width || avctx->height != frame->height) {
            avctx->width = frame->width;
            avctx->height = frame->height;
        }

        if (avctx->pix_fmt == AV_PIX_FMT_ES || frame->flags & AV_FRAME_FLAG_DISCARD) {
            frame->format = AV_PIX_FMT_ES;
            frame->hw_frames_ctx = av_buffer_ref(dec_ctx->hwframe);
        } else {
            av_frame_move_ref(tmp_frame, frame);
            ret = ff_get_buffer(avctx, frame, 0);
            if (ret < 0) {
                log_error(dec_ctx, "ff_get_buffer failed\n");
            } else {
                frame->pts = tmp_frame->pts;
                tmp_frame->hw_frames_ctx = av_buffer_ref(dec_ctx->hwframe);
                ret = av_hwframe_transfer_data(frame, tmp_frame, 0);
                if (ret) {
                    log_error(dec_ctx, "av_hwframe_transfer_data failed\n");
                }
            }
            av_frame_unref(tmp_frame);
        }
    }

    return ret;
}

static void ff_es_jpeg_decode_flush(AVCodecContext *avctx)
{
}

#define OFFSET(x) offsetof(JDECContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "stride_align", "set out_stride", OFFSET(stride_align),
        AV_OPT_TYPE_INT,  { .i64 = 64 }, 1, 2048, VD },
    { "pp0_enabled", "set pp0 output enable", OFFSET(cfg_pp_enabled[0]),
        AV_OPT_TYPE_INT,  { .i64 = 1 }, 0, 1, VD },
    { "pp0_format", "set ppu0 output format", OFFSET(output_format[0]),
        AV_OPT_TYPE_INT,  { .i64 = 23 }, 0, INT_MAX, VD },
    { "pp0_set", "set pp configure", OFFSET(pp_setting[0]),
        AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, VD }, //pp0 not support scale
    { "pp1_enabled", "set pp0 output enable", OFFSET(cfg_pp_enabled[1]),
        AV_OPT_TYPE_INT,  { .i64 = 0 }, 0, 1, VD },
    { "pp1_format", "set ppu1 output format", OFFSET(output_format[1]),
        AV_OPT_TYPE_INT,  { .i64 = 23 }, 0, INT_MAX, VD },
    { "pp1_set", "set pp configure", OFFSET(pp_setting[1]),
        AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, VD },
    { "packet_dump", "set dump packet", OFFSET(packet_dump),
        AV_OPT_TYPE_INT,  { .i64 = 0 }, 0, 1, VD },
    { "dump_path", "set dump packet path", OFFSET(dump_path),
        AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, VD },
    { "packet_dump_time", "set dump packet time", OFFSET(packet_dump_time),
        AV_OPT_TYPE_INT,  { .i64 = 0 }, 0, INT_MAX, VD },
    { "pp0_frame_dump", "set pp0 frame dump", OFFSET(frame_dump[0]),
        AV_OPT_TYPE_INT,  { .i64 = 0 }, 0, 60, VD },
    { "pp1_frame_dump", "set pp1 frame dump", OFFSET(frame_dump[1]),
        AV_OPT_TYPE_INT,  { .i64 = 0 }, 0, 60, VD },
    { "pp0_frame_dump_time", "set dump pp0 frame time", OFFSET(frame_dump_time[0]),
        AV_OPT_TYPE_INT,  { .i64 = 0 }, 0, INT_MAX, VD },
    { "pp1_frame_dump_time", "set dump pp1 frame time", OFFSET(frame_dump_time[1]),
        AV_OPT_TYPE_INT,  { .i64 = 0 }, 0, INT_MAX, VD },
    { "drop_frame_interval", "set drop frame interval", OFFSET(drop_frame_interval),
        AV_OPT_TYPE_INT,  {.i64 = 0 }, 0, INT_MAX, VD },
    { "thumb_mode", "0: only decode picture, 1: only decode thumbnail, 2: decode thumbnail and picture.",
        OFFSET(thumb_mode), AV_OPT_TYPE_INT,  {.i64 = Decode_Pic_Thumb }, 0, 2, VD, "thumbmode"},
    { "decode_pic", "0: only decode picture",  0,
        AV_OPT_TYPE_CONST, { .i64 = Only_Decode_Pic } , 0, 2, VD, "thumbmode"},
    { "decode_thumb", "1: only decode thumbnail",  0,
        AV_OPT_TYPE_CONST, { .i64 = Only_Decode_Thumb } , 0, 2, VD, "thumbmode"},
    { "decode_both", "2: decode thumbnail and picture",  0,
        AV_OPT_TYPE_CONST, { .i64 = Decode_Pic_Thumb } , 0, 2, VD, "thumbmode"},
    { "decode_mode", "decode mode: dec_normal, dec_low_latency",  OFFSET(decode_mode),
        AV_OPT_TYPE_INT, { .i64 =  DEC_NORMAL} , 0, 8, VD, "decode_mode"},
    { "dec_normal", "decode mode: dec_normal",  0,
        AV_OPT_TYPE_CONST, { .i64 =  DEC_NORMAL} , 0, 8, VD, "decode_mode"},
    { "dec_low_latency", "decode mode: dec_low_latency",  0,
        AV_OPT_TYPE_CONST, { .i64 =  DEC_LOW_LATENCY} , 0, 8, VD, "decode_mode"},
    { "input_buf_num", "inputbuffer queue size",  0,
        AV_OPT_TYPE_CONST, { .i64 =  0} , 0, 40, VD, "input_buf_num"},
    { "output_buf_num", "outputbuffer queue size",  0,
        AV_OPT_TYPE_CONST, { .i64 =  0} , 0, 40, VD, "output_buf_num"},

    // ffmpeg set opts
    { "down_scale", "width:height or ratio_x:ratio_y", OFFSET(scale_set),
        AV_OPT_TYPE_STRING, { .str="0:0" }, 0, 0, VD },
    { "pp0_crop", "crop (top)x(bottom)x(left)x(right)", OFFSET(crop_set[0]),
        AV_OPT_TYPE_STRING, { .str=NULL }, 0, 0, VD },
    { "pp1_crop", "crop (top)x(bottom)x(left)x(right)", OFFSET(crop_set[1]),
        AV_OPT_TYPE_STRING, { .str=NULL }, 0, 0, VD },
    { "frame_dump", "frame dump enable, next set filename better", OFFSET(fdump),
        AV_OPT_TYPE_INT,  { .i64 = 0 }, 0, 1, VD },
    { "pp0", "PP0 output enable",  OFFSET(cfg_pp_enabled[0]),
        AV_OPT_TYPE_INT, { .i64 = 1 }, 0, 1, VD, "enable pp"},
    { "pp1", "PP1 output enable",  OFFSET(cfg_pp_enabled[1]),
        AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VD, "enable pp"},
    { "pp_out", "set pp output enable", OFFSET(pp_out),
        AV_OPT_TYPE_INT,  { .i64 = 0 }, 0, 1, VD },
    { "enable", "enable pp",  0,
        AV_OPT_TYPE_CONST, { .i64 = 1 } , 0, 1, VD, "enable pp"},
    { "disable", "disable pp",  0,
        AV_OPT_TYPE_CONST, { .i64 = 0 } , 0, 0, VD, "enable pp"},
    { "pp0_fmt", "set pp0 output format",  OFFSET(output_format[0]),
        AV_OPT_TYPE_INT, { .i64 = 23 } , 0, 194, VD, "pp0_fmt"},
    { "pp1_fmt", "set pp1 output format",  OFFSET(output_format[1]),
        AV_OPT_TYPE_INT, { .i64 = 23 } , 0, 194, VD, "pp1_fmt"},

    // set pp0 format
    { "nv12", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_NV12 } , 0, 194, VD, "pp0_fmt"},
    { "nv21", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_NV21 } , 0, 194, VD, "pp0_fmt"},
    { "yuv420p", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_YUV420P } , 0, 194, VD, "pp0_fmt"},
    { "gray", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_GRAY8 } , 0, 194, VD, "pp0_fmt"},
    // set pp1 format
    { "nv12", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_NV12 } , 0, 194, VD, "pp1_fmt"},
    { "nv21", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_NV21 } , 0, 194, VD, "pp1_fmt"},
    { "yuv420p", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_YUV420P } , 0, 194, VD, "pp1_fmt"},
    { "gray", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_GRAY8 } , 0, 194, VD, "pp1_fmt"},
    { "rgb24", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_RGB24 } , 0, 194, VD, "pp1_fmt"},
    { "bgr24", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_BGR24 } , 0, 194, VD, "pp1_fmt"},
    { "argb", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_ARGB } , 0, 194, VD, "pp1_fmt"},
    { "abgr", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_ABGR } , 0, 194, VD, "pp1_fmt"},
    { "0rgb", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_0RGB } , 0, 194, VD, "pp1_fmt"},
    { "0bgr", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_0BGR } , 0, 194, VD, "pp1_fmt"},
    { NULL },
};

static const AVClass es_jpeg_decode_class = {
    .class_name = "es_jpeg_decoder",
    .item_name = av_default_item_name,
    .option = options,
    .version = LIBAVUTIL_VERSION_INT,
};

static const FFCodecDefault es_jpeg_decode_defaults[] = {
    { NULL },
};

static const enum AVPixelFormat es_jdec_support_pixfmts[] = {AV_PIX_FMT_ES,
                                                             AV_PIX_FMT_YUV420P,
                                                             AV_PIX_FMT_NV12,
                                                             AV_PIX_FMT_NV21,
                                                             AV_PIX_FMT_GRAY8,
                                                             AV_PIX_FMT_NONE};

static const AVCodecHWConfigInternal *es_hw_configs[] = {
    &(const AVCodecHWConfigInternal) {
        .public = {
            .pix_fmt     = AV_PIX_FMT_ES,
            .methods     = AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX
                           | AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX
                           | AV_CODEC_HW_CONFIG_METHOD_INTERNAL,
            .device_type = AV_HWDEVICE_TYPE_ES,
        },
        .hwaccel = NULL,
    },
    NULL
};

FFCodec ff_jpeg_es_decoder = {
    .p.name           = "jpeg_es_decoder",
    .p.long_name      = NULL_IF_CONFIG_SMALL("Eswin JPEG decoder, on VeriSilicon & GStreamer & FFmpeg."),
    .p.type           = AVMEDIA_TYPE_VIDEO,
    .p.id             = AV_CODEC_ID_MJPEG,
    .priv_data_size = sizeof(JDECContext),
    .init           = &ff_es_jpeg_decode_init,
    .close          = &ff_es_jpeg_decode_close,
    FF_CODEC_RECEIVE_FRAME_CB(ff_es_jpeg_decode_receive_frame),
    .flush          = &ff_es_jpeg_decode_flush,
    .p.priv_class     = &es_jpeg_decode_class,
    .p.capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_AVOID_PROBING,
    .defaults       = es_jpeg_decode_defaults,
    .p.pix_fmts       = es_jdec_support_pixfmts,
    .hw_configs     = es_hw_configs,
    .p.wrapper_name   = "es",
    //.bsfs           = "mjpeg2jpeg",
};