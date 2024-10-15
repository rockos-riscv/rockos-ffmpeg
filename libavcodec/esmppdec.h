#ifndef AVCODEC_ESMPPDEC_H
#define AVCODEC_ESMPPDEC_H

#include <es_mpp.h>
#include <mpp_buffer.h>

#include "codec_internal.h"
#include "decode.h"
#include "hwconfig.h"
#include "internal.h"

#include "libavutil/hwcontext_esmpp.h"
#include "libavutil/mastering_display_metadata.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#define MAX_ERRINFO_COUNT 100

typedef struct ESMPPDecContext {
    AVClass *class;

    MppCtxPtr mctx;
    MppBufferGroupPtr buf_group;

    AVBufferRef *hwdevice;
    AVBufferRef *hwframe;

    AVPacket last_pkt;
    int eof;
    int draining;
    int info_change;
    int errinfo_cnt;

    int deint;
    int buf_mode;
    int output_fmt;
    int stride_align;
    int buf_cache_mode;
    int dfd;
    // crop
    char *crop;
    int crop_xoffset;
    int crop_yoffset;
    int crop_width;
    int crop_height;

    // scale
    char *scale;
    int scale_width;
    int scale_height;

    int first_packet;
} ESMPPDecContext;

enum {
    ESMPP_DEC_HALF_INTERNAL = 0,
    ESMPP_DEC_PURE_EXTERNAL = 1,
};

enum {
    ESMPP_DEC_DMA_UNCACHE = 0,
    ESMPP_DEC_DMA_CACHE = 1,
};

static const AVRational mpp_tb = {1, 1000000};

#define PTS_TO_MPP_PTS(pts, pts_tb) ((pts_tb.num && pts_tb.den) ? av_rescale_q(pts, pts_tb, mpp_tb) : pts)

#define MPP_PTS_TO_PTS(mpp_pts, pts_tb) ((pts_tb.num && pts_tb.den) ? av_rescale_q(mpp_pts, mpp_tb, pts_tb) : mpp_pts)

#define OFFSET(x) offsetof(ESMPPDecContext, x)
#define VD (AV_OPT_FLAG_DECODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

#define ESMPP_VDEC_COMMON_OPTIONS                                                                                      \
    {"stride_align",                                                                                                   \
     "set out stride",                                                                                                 \
     OFFSET(stride_align),                                                                                             \
     AV_OPT_TYPE_INT,                                                                                                  \
     {.i64 = 64},                                                                                                      \
     1,                                                                                                                \
     2048,                                                                                                             \
     VD,                                                                                                               \
     "stride_align"},                                                                                                  \
        {"1", "1 byte align", 0, AV_OPT_TYPE_CONST, {.i64 = 1}, 0, 0, VD, "stride_align"},                             \
        {"8", "8 bytes align", 0, AV_OPT_TYPE_CONST, {.i64 = 8}, 0, 0, VD, "stride_align"},                            \
        {"16", "16 bytes align", 0, AV_OPT_TYPE_CONST, {.i64 = 16}, 0, 0, VD, "stride_align"},                         \
        {"32", "32 bytes align", 0, AV_OPT_TYPE_CONST, {.i64 = 32}, 0, 0, VD, "stride_align"},                         \
        {"64", "64 bytes align", 0, AV_OPT_TYPE_CONST, {.i64 = 64}, 0, 0, VD, "stride_align"},                         \
        {"128", "128 bytes align", 0, AV_OPT_TYPE_CONST, {.i64 = 128}, 0, 0, VD, "stride_align"},                      \
        {"256", "256 bytes align", 0, AV_OPT_TYPE_CONST, {.i64 = 256}, 0, 0, VD, "stride_align"},                      \
        {"512", "512 bytes align", 0, AV_OPT_TYPE_CONST, {.i64 = 512}, 0, 0, VD, "stride_align"},                      \
        {"1024", "1024 bytes align", 0, AV_OPT_TYPE_CONST, {.i64 = 1024}, 0, 0, VD, "stride_align"},                   \
        {"2048", "2048 bytes align", 0, AV_OPT_TYPE_CONST, {.i64 = 2048}, 0, 0, VD, "stride_align"},                   \
        {"buf_mode",                                                                                                   \
         "Set the buffer mode for MPP decoder",                                                                        \
         OFFSET(buf_mode),                                                                                             \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = ESMPP_DEC_HALF_INTERNAL},                                                                             \
         0,                                                                                                            \
         1,                                                                                                            \
         VD,                                                                                                           \
         "buf_mode"},                                                                                                  \
        {"half", "Half internal mode", 0, AV_OPT_TYPE_CONST, {.i64 = ESMPP_DEC_HALF_INTERNAL}, 0, 0, VD, "buf_mode"},  \
        {"ext", "Pure external mode", 0, AV_OPT_TYPE_CONST, {.i64 = ESMPP_DEC_PURE_EXTERNAL}, 0, 0, VD, "buf_mode"},   \
        {"buf_cache_mode",                                                                                             \
         "Set the buffer cache mode for MPP decoder",                                                                  \
         OFFSET(buf_cache_mode),                                                                                       \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = ESMPP_DEC_DMA_CACHE},                                                                                 \
         0,                                                                                                            \
         1,                                                                                                            \
         VD,                                                                                                           \
         "buf_cache_mode"},                                                                                            \
        {"uncache", "uncache mode", 0, AV_OPT_TYPE_CONST, {.i64 = ESMPP_DEC_DMA_UNCACHE}, 0, 0, VD, "buf_cache_mode"}, \
        {"cache", "cache mode", 0, AV_OPT_TYPE_CONST, {.i64 = ESMPP_DEC_DMA_CACHE}, 0, 0, VD, "buf_cache_mode"},       \
        {"crop",                                                                                                       \
         "crop (xoffset)x(yoffset)x(width)x(height)",                                                                  \
         OFFSET(crop),                                                                                                 \
         AV_OPT_TYPE_STRING,                                                                                           \
         {.str = NULL},                                                                                                \
         0,                                                                                                            \
         0,                                                                                                            \
         VD},                                                                                                          \
        {"scale", "width:height or ratio_x:ratio_y", OFFSET(scale), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, VD},      \
        {"dfd", "force drm format output", OFFSET(dfd), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, VD},                        \
        {"dec_pixfmt",                                                                                                 \
         "set decode output pixfmt",                                                                                   \
         OFFSET(output_fmt),                                                                                           \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = AV_PIX_FMT_YUV420P},                                                                                  \
         0,                                                                                                            \
         INT_MAX,                                                                                                      \
         VD,                                                                                                           \
         "dec_pixfmt"},                                                                                                \
        {"nv12", "nv12", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_NV12}, 0, 0, VD, "dec_pixfmt"},                      \
        {"nv21", "nv21", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_NV21}, 0, 0, VD, "dec_pixfmt"},                      \
        {"yuv420p", "yuv420p", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_YUV420P}, 0, 0, VD, "dec_pixfmt"},             \
        {"gray", "gray", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_GRAY8}, 0, 0, VD, "dec_pixfmt"},                     \
        {"rgb", "rgb", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_RGB24}, 0, 0, VD, "dec_pixfmt"},                       \
        {"bgr", "bgr", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_BGR24}, 0, 0, VD, "dec_pixfmt"},                       \
        {"bgra", "bgra", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_BGRA}, 0, 0, VD, "dec_pixfmt"},                      \
        {"rgba", "rgba", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_RGBA}, 0, 0, VD, "dec_pixfmt"},                      \
        {"bgr0", "bgr0", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_BGR0}, 0, 0, VD, "dec_pixfmt"}, {                    \
        "rgb0", "rgb0", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_RGB0}, 0, 0, VD, "dec_pixfmt"                         \
    }

static const AVOption h264_options[] = {
    ESMPP_VDEC_COMMON_OPTIONS,
    {"p010le", "p010le", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_P010LE}, 0, 0, VD, "dec_pixfmt"},
    {NULL}};

static const AVOption hevc_options[] = {
    ESMPP_VDEC_COMMON_OPTIONS,
    {"p010le", "p010le", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_P010LE}, 0, 0, VD, "dec_pixfmt"},
    {NULL}};

static const AVOption mjpeg_options[] = {ESMPP_VDEC_COMMON_OPTIONS, {NULL}};

#define ESMPP_VDEC_COMMON_PIX_FMTS                                                                                  \
    AV_PIX_FMT_DRM_PRIME, AV_PIX_FMT_NV12, AV_PIX_FMT_NV21, AV_PIX_FMT_YUV420P, AV_PIX_FMT_GRAY8, AV_PIX_FMT_RGB24, \
        AV_PIX_FMT_BGR24, AV_PIX_FMT_BGRA, AV_PIX_FMT_RGBA, AV_PIX_FMT_BGR0, AV_PIX_FMT_RGB0

static const enum AVPixelFormat h264_pix_fmts[] = {
    ESMPP_VDEC_COMMON_PIX_FMTS,
    AV_PIX_FMT_P010LE,
    AV_PIX_FMT_NONE,
};

static const enum AVPixelFormat hevc_pix_fmts[] = {
    ESMPP_VDEC_COMMON_PIX_FMTS,
    AV_PIX_FMT_P010LE,
    AV_PIX_FMT_NONE,
};

static const enum AVPixelFormat mjpeg_pix_fmts[] = {
    ESMPP_VDEC_COMMON_PIX_FMTS,
    AV_PIX_FMT_NONE,
};

static const AVCodecHWConfigInternal *const mpp_dec_hw_configs[] = {
    &(const AVCodecHWConfigInternal){
        .public =
            {
                .pix_fmt = AV_PIX_FMT_DRM_PRIME,
                .methods = AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX | AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX
                           | AV_CODEC_HW_CONFIG_METHOD_INTERNAL,
                .device_type = AV_HWDEVICE_TYPE_ESMPP,
            },
        .hwaccel = NULL,
    },
    NULL};

#define DEFINE_ESMPP_DECODER(x, X, bsf_name)                                                       \
    static const AVClass x##_esmpp_decoder_class = {                                               \
        .class_name = #x "_esmpp_decoder",                                                         \
        .item_name = av_default_item_name,                                                         \
        .option = x##_options,                                                                     \
        .version = LIBAVUTIL_VERSION_INT,                                                          \
    };                                                                                             \
    const FFCodec ff_##x##_esmpp_decoder = {                                                       \
        .p.name = #x "_esmppvdec",                                                                 \
        .p.long_name = NULL_IF_CONFIG_SMALL("Eswin " #x " video decoder"),                         \
        .p.type = AVMEDIA_TYPE_VIDEO,                                                              \
        .p.id = AV_CODEC_ID_##X,                                                                   \
        .priv_data_size = sizeof(ESMPPDecContext),                                                 \
        .p.priv_class = &x##_esmpp_decoder_class,                                                  \
        .init = mpp_decode_init,                                                                   \
        .close = mpp_decode_close,                                                                 \
        FF_CODEC_RECEIVE_FRAME_CB(mpp_decode_receive_frame),                                       \
        .flush = mpp_decode_flush,                                                                 \
        .bsfs = bsf_name,                                                                          \
        .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE, \
        .caps_internal = FF_CODEC_CAP_NOT_INIT_THREADSAFE | FF_CODEC_CAP_SETS_FRAME_PROPS,         \
        .p.pix_fmts = x##_pix_fmts,                                                                \
        .hw_configs = mpp_dec_hw_configs,                                                          \
        .p.wrapper_name = "esmpp",                                                                 \
    };

#endif /* AVCODEC_ESMPPDEC_H */
