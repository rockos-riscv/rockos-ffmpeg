#define LOG_TAG "esdecode"
#include <libavutil/log.h>
#include "libavutil/pixdesc.h"
#include "libavutil/hwcontext_es.h"
#include "hwconfig.h"
#include "internal.h"
#include "decode.h"
#include "esdecapi.h"
#include "eslog.h"
#include "esdecbuffer.h"
#include "encode.h"
#include "codec_internal.h"

static void ff_es_vdec_flush(AVCodecContext *avctx);
static enum AVPixelFormat ff_es_vdec_get_format(ESVDECContext *dec_ctx) {
    enum AVPixelFormat format = AV_PIX_FMT_NV12;
    if (!dec_ctx) {
        log_error(dec_ctx, "dec_ctx is null\n");
        return AV_PIX_FMT_NONE;
    }

    for (int i = 0; i < 2; i++) {
        log_info(dec_ctx, "pp_enabled[%d]: %d, target_pp: %d\n", i , dec_ctx->pp_enabled[i], dec_ctx->target_pp);
        if (dec_ctx->pp_enabled[i] && (dec_ctx->target_pp == -1 || dec_ctx->target_pp == i)) {
            format = dec_ctx->pp_fmt[i];
            log_info(dec_ctx, "target pp: %d, format: %d\n", i, format);
            break;
        }
    }

    return format;
}

static av_cold int ff_es_vdec_init(AVCodecContext *avctx) {
    int ret = FAILURE;
    AVHWFramesContext *hwframe_ctx;
    ESDecCodec codec;
    ESVDECContext *dec_ctx;

    if (!avctx || !avctx->priv_data) {
        log_error(avctx, "avctx or priv_data is null avctx: %p\n", avctx);
        return FAILURE;
    }

    dec_ctx = (ESVDECContext *)avctx->priv_data;
    es_decode_print_version_info(dec_ctx);

    if (!dec_ctx->pp_enabled[0] && !dec_ctx->pp_enabled[1]) {
        dec_ctx->pp_enabled[1] = 1;
        dec_ctx->target_pp = 1;
    }

    avctx->sw_pix_fmt = ff_es_vdec_get_format(dec_ctx);
    enum AVPixelFormat pix_fmts[3] = {AV_PIX_FMT_ES, avctx->sw_pix_fmt, AV_PIX_FMT_NONE};
    avctx->pix_fmt = ff_get_format(avctx, pix_fmts);

    log_info(avctx,
             "avctx sw_pix_fmt: %s, pix_fmt: %s\n",
             av_get_pix_fmt_name(avctx->sw_pix_fmt),
             av_get_pix_fmt_name(avctx->pix_fmt));

    if (avctx->hw_frames_ctx) {
        dec_ctx->hwframe = av_buffer_ref(avctx->hw_frames_ctx);
    } else {
        if (avctx->hw_device_ctx) {
            dec_ctx->hwdevice = av_buffer_ref(avctx->hw_device_ctx);
            if (!dec_ctx->hwdevice) {
                log_error(dec_ctx, "av_buffer_ref hw_device_ctx failed\n");
                return FAILURE;
            }
        } else {
            ret = av_hwdevice_ctx_create(&dec_ctx->hwdevice, AV_HWDEVICE_TYPE_ES, "es", NULL, 0);
            if (ret) {
                log_error(dec_ctx, "av_hwdevice_ctx_create failed\n");
                return FAILURE;
            }
        }

        dec_ctx->hwframe = av_hwframe_ctx_alloc(dec_ctx->hwdevice);
        if (!dec_ctx->hwframe) {
            log_error(dec_ctx, "av_hwframe_ctx_alloc failed\n");
            return FAILURE;
        }
        hwframe_ctx = (AVHWFramesContext *)dec_ctx->hwframe->data;
        hwframe_ctx->format = AV_PIX_FMT_ES;
        hwframe_ctx->sw_format = avctx->sw_pix_fmt;
    }

    if (avctx->codec_id == AV_CODEC_ID_H264) {
        codec = ES_H264_H10P;
    } else if (avctx->codec_id == AV_CODEC_ID_HEVC) {
        codec = ES_HEVC;
    } else {
        log_error(dec_ctx, "not support codec id: 0x%x\n", avctx->codec_id);
        return FAILURE;
    }

    if (avctx->extra_hw_frames > 0) {
        dec_ctx->extra_hw_frames = avctx->extra_hw_frames;
    }

    if (dec_ctx->extra_hw_frames <= 0) {
        dec_ctx->extra_hw_frames = 4;
    }
    log_info(dec_ctx,
             "dec_ctx extra_hw_frames: %d, avctx extra_hw_frames: %d\n",
             dec_ctx->extra_hw_frames,
             avctx->extra_hw_frames);

    ret = es_decode_set_params(dec_ctx, codec);
    if (ret == FAILURE) {
        log_error(dec_ctx, "codec id: 0x%x, codec: %d set params failed\n", avctx->codec_id, codec);
        return ret;
    } else {
        log_info(dec_ctx, "codec id: 0x%x, codec: %d set params success\n", avctx->codec_id, codec);
    }

    ret = es_decode_init(dec_ctx);
    if (ret == FAILURE) {
        log_error(dec_ctx, "init decoder failed\n");
        return ret;
    }

    ret = es_decode_start(dec_ctx);
    if (ret == SUCCESS) {
        avctx->apply_cropping = 0;
        dec_ctx->state = ESDEC_STATE_STARTED;
        log_info(dec_ctx, "ff_es_vdec_init success\n");
    } else {
        log_error(dec_ctx, "ff_es_vdec_init failed\n");
    }

    return ret;
}

static int ff_es_vdec_eof_proc(ESVDECContext *dec_ctx, AVFrame *frame) {
    int ret;
    if (!dec_ctx) {
        return AVERROR(EINVAL);
    }

    log_info(dec_ctx, "dec_ctx->state: %d\n", dec_ctx->state);
    if (dec_ctx->state == ESDEC_STATE_STARTED) {
        ret = es_decode_send_packet(dec_ctx, NULL, -1 /*always waiting*/);
        if (ret == SUCCESS) {
            dec_ctx->state = ESDEC_STATE_STOPPING;
            log_info(dec_ctx, "send eos packet success enter stopping\n");
        } else {
            dec_ctx->state = ESDEC_STATE_STOPPED;
            log_info(dec_ctx, "dec_ctx->state: %d\n", dec_ctx->state);
        }
    }

    if (dec_ctx->state == ESDEC_STATE_STOPPING) {
        ret = es_decode_get_frame(dec_ctx, frame, -1 /* always waiting */);
        if (ret == AVERROR_EOF || ret == AVERROR_EXIT) {
            dec_ctx->state = ESDEC_STATE_STOPPED;
            log_info(dec_ctx, "recv eof decode enter stopped\n");
        } else {
            log_info(dec_ctx, "decode state stopping get frame ret: %d\n", ret);
        }
    }

    return ret;
}

static int ff_es_vdec_receive_frame(AVCodecContext *avctx, AVFrame *frame) {
    int ret = SUCCESS;
    ESVDECContext *dec_ctx;
    AVPacket *avpkt;

    if (!avctx || !avctx->priv_data || !frame) {
        log_error(dec_ctx, "avctx or private or frame is null avctx: %p, frame: %p\n", avctx, frame);
        return AVERROR(EINVAL);
    }

    dec_ctx = (ESVDECContext *)avctx->priv_data;
    avpkt = &dec_ctx->pkt;

    if (dec_ctx->state == ESDEC_STATE_UNINIT) {
        log_error(dec_ctx, "dec_ctx state: %d error\n", dec_ctx->state);
        return AVERROR(EINVAL);
    } else if (dec_ctx->state == ESDEC_STATE_STOPPED || dec_ctx->state == ESDEC_STATE_CLOSED) {
        log_warn(dec_ctx, "dec_ctx state: %d closed\n", dec_ctx->state);
        return AVERROR_EOF;
    }

    do {
        if (dec_ctx->state == ESDEC_STATE_STOPPING) {
            ret = ff_es_vdec_eof_proc(dec_ctx, frame);
            break;
        }

        if (!avpkt->data || avpkt->size <= 0) {
            ret = ff_decode_get_packet(avctx, avpkt);
            log_debug(dec_ctx, "ff_decode_get_packet ret: 0x%x, size: %d, flags: %d\n", ret, avpkt->size, avpkt->flags);
            if (ret == SUCCESS) {
                dec_ctx->reordered_opaque = avctx->reordered_opaque;
                if (dec_ctx->state == ESDEC_STATE_FLUSHED) {
                    dec_ctx->state = ESDEC_STATE_STARTED;
                }
            }
        } else {
            log_debug(dec_ctx, "use last pkt size: %d\n", avpkt->size);
        }

        if (ret == AVERROR(EAGAIN)) {
            ret = es_decode_get_frame(dec_ctx, frame, 0 /*without waiting*/);
            if (ret == FAILURE) {
                ret = AVERROR(EAGAIN);
                log_debug(dec_ctx, "EAGAIN ret: %d\n", ret);
            }
            break;
        } else if (ret == AVERROR_EOF) {
            ret = ff_es_vdec_eof_proc(dec_ctx, frame);
            break;
        } else if (ret < 0) {
            log_error(dec_ctx, "ff_decode_get_packet failed ret: 0x%x\n", ret);
            break;
        }

        ret = es_decode_send_packet_receive_frame(dec_ctx, avpkt, frame);
    } while (0);

    if (ret == SUCCESS) {
        if (avctx->width != frame->width || avctx->height != frame->height) {
            avctx->width = frame->width;
            avctx->height = frame->height;
        }

        if (avctx->pix_fmt == AV_PIX_FMT_ES) {
            frame->format = AV_PIX_FMT_ES;
            frame->hw_frames_ctx = av_buffer_ref(dec_ctx->hwframe);
        } else if (frame->flags & AV_FRAME_FLAG_DISCARD) {
            frame->format = avctx->sw_pix_fmt;
        }
    }

    return ret;
}

static void ff_es_vdec_flush(AVCodecContext *avctx) {
    ESVDECContext *dec_ctx;
    if (!avctx || !avctx->priv_data) {
        log_info(avctx, "avctx or dec_ctx is null avctx: %p\n", avctx);
        return;
    }

    if (dec_ctx->state != ESDEC_STATE_UNINIT && dec_ctx->state != ESDEC_STATE_CLOSED
        && dec_ctx->state != ESDEC_STATE_FLUSHING && dec_ctx->state != ESDEC_STATE_FLUSHED) {
        dec_ctx = (ESVDECContext *)avctx->priv_data;
        av_packet_unref(&dec_ctx->pkt);
        es_decode_flush(dec_ctx);
    }

    log_info(avctx, "ff_es_vdec_flush dec_ctx->state: %d\n", dec_ctx->state);
}

static av_cold int ff_es_vdec_close(AVCodecContext *avctx) {
    ESVDECContext *dec_ctx;
    if (!avctx || !avctx->priv_data) {
        log_info(avctx, "avctx or dec_ctx is null avctx: %p\n", avctx);
        return 0;
    }

    log_info(avctx, "ff_es_vdec_close start\n");
    dec_ctx = (ESVDECContext *)avctx->priv_data;
    if (dec_ctx->state != ESDEC_STATE_CLOSED) {
        dec_ctx->state = ESDEC_STATE_CLOSED;
        es_decode_close(dec_ctx);
    }

    if (dec_ctx->dump_pkt_handle) {
        ff_codec_dump_file_close(&dec_ctx->dump_pkt_handle);
    }
    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if (dec_ctx->dump_frm_handle[i]) {
            ff_codec_dump_file_close(&dec_ctx->dump_frm_handle[i]);
        }
    }

    log_info(avctx, "ff_es_vdec_close end\n");

    return 0;
}

#define OFFSET(x) offsetof(ESVDECContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    {"stride_align", "set out stride", OFFSET(stride_align), AV_OPT_TYPE_INT, {.i64 = 64}, 1, 1024, VD},
    {"drop_frame_interval",
     "set drop frame interval",
     OFFSET(drop_frame_interval),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     INT_MAX,
     VD},
    {"extra_output_frames",
     "set extra output frame number",
     OFFSET(extra_hw_frames),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     128,
     VD},
    {"pp0", "pp0 enable", OFFSET(pp_enabled[0]), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, VD, "enable pp"},
    {"pp1", "pp1 enable", OFFSET(pp_enabled[1]), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, VD, "enable pp"},
    {"enable", "enable pp", 0, AV_OPT_TYPE_CONST, {.i64 = 1}, 0, 0, VD, "enable pp"},
    {"disable", "disable pp", 0, AV_OPT_TYPE_CONST, {.i64 = 0}, 0, 0, VD, "enable pp"},
    {"crop", "crop (top)x(bottom)x(left)x(right)", OFFSET(crop[1]), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, VD},
    {"pp0_crop", "crop (top)x(bottom)x(left)x(right)", OFFSET(crop[0]), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, VD},
    {"pp1_crop", "crop (top)x(bottom)x(left)x(right)", OFFSET(crop[1]), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, VD},
    {"down_scale", "width:height or ratio_x:ratio_y", OFFSET(scale), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, VD},
    {"pixfmt",
     "set output pix_fmt",
     OFFSET(pp_fmt[1]),
     AV_OPT_TYPE_INT,
     {.i64 = AV_PIX_FMT_YUV420P},
     -1,
     INT_MAX,
     VD,
     "pp1_fmt"},
    {"pp0_pixfmt",
     "set pp0 pix_fmt",
     OFFSET(pp_fmt[0]),
     AV_OPT_TYPE_INT,
     {.i64 = AV_PIX_FMT_YUV420P},
     -1,
     INT_MAX,
     VD,
     "pp0_fmt"},
    {"nv12", "", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_NV12}, 0, 0, VD, "pp0_fmt"},
    {"nv21", "", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_NV21}, 0, 0, VD, "pp0_fmt"},
    {"yuv420p", "", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_YUV420P}, 0, 0, VD, "pp0_fmt"},
    {"gray", "", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_GRAY8}, 0, 0, VD, "pp0_fmt"},
    {"p010le", "", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_P010LE}, 0, 0, VD, "pp0_fmt"},
    {"pp1_pixfmt",
     "set pp1 pix_fmt",
     OFFSET(pp_fmt[1]),
     AV_OPT_TYPE_INT,
     {.i64 = AV_PIX_FMT_YUV420P},
     -1,
     INT_MAX,
     VD,
     "pp1_fmt"},
    {"nv12", "", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_NV12}, 0, 0, VD, "pp1_fmt"},
    {"nv21", "", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_NV21}, 0, 0, VD, "pp1_fmt"},
    {"yuv420p", "", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_YUV420P}, 0, 0, VD, "pp1_fmt"},
    {"gray", "", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_GRAY8}, 0, 0, VD, "pp1_fmt"},
    {"p010le", "", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_P010LE}, 0, 0, VD, "pp1_fmt"},
    {"rgb", "", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_RGB24}, 0, 0, VD, "pp1_fmt"},
    {"bgr", "", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_BGR24}, 0, 0, VD, "pp1_fmt"},
    {"argb", "", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_ARGB}, 0, 0, VD, "pp1_fmt"},
    {"abgr", "", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_ABGR}, 0, 0, VD, "pp1_fmt"},
    {"xrgb", "", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_0RGB}, 0, 0, VD, "pp1_fmt"},
    {"xbgr", "", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_0BGR}, 0, 0, VD, "pp1_fmt"},
    {"rgb48le", "", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_RGB48LE}, 0, 0, VD, "pp1_fmt"},
    {"bgr48le", "", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_BGR48LE}, 0, 0, VD, "pp1_fmt"},
    {"rbga64le", "", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_RGBA64LE}, 0, 0, VD, "pp1_fmt"},
    {"bgra64le", "output format", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_BGRA64LE}, 0, 0, VD, "pp1_fmt"},
    {"target_pp", "", OFFSET(target_pp), AV_OPT_TYPE_INT, {.i64 = -1}, -1, 1, VD, "target_pp"},
    {"auto",
     "both pp0&pp1 enabled and the target is pp0, pp0 or pp1 is enabled, the target is pp0 or pp1",
     0,
     AV_OPT_TYPE_CONST,
     {.i64 = -1},
     0,
     0,
     VD,
     "target_pp"},
    {"pp0", "the target to pp0", 0, AV_OPT_TYPE_CONST, {.i64 = 0}, 0, 0, VD, "target_pp"},
    {"pp1", "the target to pp1", 0, AV_OPT_TYPE_CONST, {.i64 = 1}, 0, 0, VD, "target_pp"},
    {"packet_dump", "set dump packet", OFFSET(packet_dump), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, VD},
    {"dump_path", "set dump packet path", OFFSET(dump_path), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, VD},
    {"packet_dump_time", "set dump packet time", OFFSET(packet_dump_time), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 60000, VD},
    {"pp0_frame_dump", "set pp0 frame dump", OFFSET(frame_dump[0]), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, VD},
    {"pp1_frame_dump", "set pp1 frame dump", OFFSET(frame_dump[1]), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, VD},
    {"pp0_frame_dump_time",
     "dump pp0 frame time",
     OFFSET(frame_dump_time[0]),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     60000,
     VD},
    {"pp1_frame_dump_time",
     "set dump pp1 frame time",
     OFFSET(frame_dump_time[1]),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     60000,
     VD},
    {NULL},
};

static const AVCodecHWConfigInternal *es_hw_configs[] = {
    &(const AVCodecHWConfigInternal){
        .public =
            {
                .pix_fmt = AV_PIX_FMT_ES,
                .methods = AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX | AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX
                           | AV_CODEC_HW_CONFIG_METHOD_INTERNAL,
                .device_type = AV_HWDEVICE_TYPE_ES,
            },
        .hwaccel = NULL,
    },
    NULL};

static const enum AVPixelFormat es_vdec_support_pixfmts[] = {AV_PIX_FMT_ES,
                                                             AV_PIX_FMT_YUV420P,
                                                             AV_PIX_FMT_NV12,
                                                             AV_PIX_FMT_NV21,
                                                             AV_PIX_FMT_GRAY8,
                                                             AV_PIX_FMT_P010,
                                                             AV_PIX_FMT_NONE};

#define ES_VIDEO_DEC(ctype, CTYPE)                                                               \
    static const AVClass es_##ctype##_decoder_class = {                                          \
        .class_name = #ctype "_esvdec",                                                          \
        .item_name = av_default_item_name,                                                       \
        .option = options,                                                                       \
        .version = LIBAVUTIL_VERSION_INT,                                                        \
    };                                                                                           \
    FFCodec ff_##ctype##_es_decoder = {                                                          \
        .p.name = #ctype "_esvdec",                                                                \
        .p.long_name = NULL_IF_CONFIG_SMALL("Eswin " #ctype " video decoder"),                     \
        .p.type = AVMEDIA_TYPE_VIDEO,                                                              \
        .p.id = AV_CODEC_ID_##CTYPE,                                                               \
        .priv_data_size = sizeof(ESVDECContext),                                                 \
        .p.priv_class = &es_##ctype##_decoder_class,                                               \
        .init = ff_es_vdec_init,                                                                 \
        .close = ff_es_vdec_close,                                                               \
        FF_CODEC_RECEIVE_FRAME_CB(ff_es_vdec_receive_frame),                                     \
        .flush = ff_es_vdec_flush,                                                               \
        .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE, \
        .p.pix_fmts = es_vdec_support_pixfmts,                                                     \
        .hw_configs = es_hw_configs,                                                             \
        .p.wrapper_name = "esvdec",                                                                \
        .bsfs = #ctype "_mp4toannexb",                                                           \
    };

ES_VIDEO_DEC(hevc, HEVC)
ES_VIDEO_DEC(h264, H264)

#undef ES_VIDEO_DEC