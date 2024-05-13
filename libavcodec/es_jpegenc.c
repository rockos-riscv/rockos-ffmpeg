
/*
 * Copyright (C) 2022 Eswin
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
#include "libavutil/opt.h"
#include "hwconfig.h"
#include "avcodec.h"
#include <libavutil/imgutils.h>

#include "jpegencapi.h"
#include "encinputlinebuffer.h"
#include "es_jpegenc.h"
#include "es_common.h"
#include "encode.h"
#include "codec_internal.h"

#ifndef OFFSET
#define OFFSET(x) offsetof(EsJpegEncodeContext, x)
#endif

#ifndef FLAGS
#define FLAGS (AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_EXPORT)
#endif

#ifndef UNUSED
#define UNUSED(x) (void)x;
#endif

#define JPEG_ES_ENCODER_STR "jpeg_es_encoder"

#define USER_DEFINED_QTABLE 10

/*------------------------------------------------------------------------------
    4. Local function prototypes
------------------------------------------------------------------------------*/
static int read_roimap(AVCodecContext *avctx);
static int read_non_roi_fliter(AVCodecContext *avctx);
static JpegEncRet es_jenc_alloc_roi_res(AVCodecContext *avctx);
static void es_jenc_get_aligned_pic_size_by_format(
    JpegEncFrameType type, u32 width, u32 height, u32 alignment, u64 *luma_size, u64 *chroma_size, u64 *picture_size);
static void es_jpeg_encode_report(AVCodecContext *avctx);  // TODO: Performance report
static JpegEncRet es_jenc_config_codec(AVCodecContext *avctx);
static JpegEncRet es_jenc_alloc_codec_res(AVCodecContext *avctx);
static JpegEncRet es_jenc_free_codec_res(AVCodecContext *avctx);
static JpegEncRet es_jenc_init_codec(AVCodecContext *avctx);
static JpegEncRet es_jenc_release_codec(AVCodecContext *avctx);
static JpegEncRet es_jenc_init_thumbnail(AVCodecContext *avctx);
static JpegEncRet es_jenc_set_thumbnail(AVCodecContext *avctx);
static JpegEncRet es_jenc_init_input_line_buffer(AVCodecContext *avctx);
static JpegEncRet es_jenc_set_input_line_buffer(AVCodecContext *avctx);
static JpegEncRet es_jenc_set_quant_table(AVCodecContext *avctx);
static int es_jenc_realloc_output_buffer(AVCodecContext *avctx, int new_size);

static const AVOption es_jpeg_encode_options[] = {
    // pre-processor setting
    {"rotation",
     "pre-processor, rotation. 0=0 degree, 90=right 90 degree, 270=left 90 degree, 180=right 180 "
     "degree.",
     OFFSET(jpeg_option_params.rotation),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     360,
     FLAGS},
    {"mirror",
     "pre-processor, mirror. 0=disable, 1=enable.",
     OFFSET(jpeg_option_params.mirror),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     1,
     FLAGS},
    // crop string:
    {"crop",
     "crop 'cx:N,cy:N,cw:N,ch:N',mean crop xoffset,yoffset,out_width,out_heigh",
     OFFSET(crop_str),
     AV_OPT_TYPE_STRING,
     {.str = "cx:0,cy:0,cw:0,ch:0"},
     0,
     0,
     FLAGS},
    // thumbnail setting:
    {"input_thumb",
     "thumbnail input file path. [thumbnail.jpg] ",
     OFFSET(jpeg_option_params.inputThumb),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     0,
     0,
     FLAGS},
    {"thumb_fmt",
     "thumbnail format. 0:disable 1:JPEG 2:RGB8 3:RGB24 ",
     OFFSET(jpeg_option_params.thumbnailFormat),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     3,
     FLAGS},
    {"thumb_w",
     "thumbnail width. [0, 255]",
     OFFSET(jpeg_option_params.widthThumb),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     255,
     FLAGS},
    {"thumb_h",
     "thumbnail heigh. [0, 255]",
     OFFSET(jpeg_option_params.heightThumb),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     255,
     FLAGS},
    // Quantization
    {"qp",
     "quantization scale.[0, 10] 10 = user defined qtable.",
     OFFSET(jpeg_option_params.qLevel),
     AV_OPT_TYPE_INT,
     {.i64 = 1},
     0,
     10,
     FLAGS},
    {"qtable_path",
     "user defined qtable path",
     OFFSET(jpeg_option_params.qTablePath),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     0,
     0,
     FLAGS},
    // Density
    {"units",
     "Units type of x- and y-density, 0 = pixel aspect ratio, 1 = dots/inch, 2 = dots/cm ",
     OFFSET(jpeg_option_params.unitsType),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     2,
     FLAGS},
    {"xdensity",
     "Xdensity to APP0 header. ",
     OFFSET(jpeg_option_params.xdensity),
     AV_OPT_TYPE_INT,
     {.i64 = 1},
     1,
     0xFFFFU,
     FLAGS},
    {"ydensity",
     "Ydensity to APP0 header. ",
     OFFSET(jpeg_option_params.ydensity),
     AV_OPT_TYPE_INT,
     {.i64 = 1},
     1,
     0xFFFFU,
     FLAGS},
    // ConstChroma
    {"enable_const_chroma",
     "Enable/Disable set chroma to a constant pixel value. 0:disable,1:enable  ",
     OFFSET(jpeg_option_params.constChromaEn),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     1,
     FLAGS},
    {"const_cb",
     "The constant pixel value for Cb.   ",
     OFFSET(jpeg_option_params.constCb),
     AV_OPT_TYPE_INT,
     {.i64 = 128},
     0,
     255,
     FLAGS},
    {"const_cr",
     "The constant pixel value for Cr.   ",
     OFFSET(jpeg_option_params.constCr),
     AV_OPT_TYPE_INT,
     {.i64 = 128},
     0,
     255,
     FLAGS},
    // ROI: jpeg only:
    {"roi_file",
     "Input file for roimap region. [jpeg_roimap.roi]",
     OFFSET(jpeg_option_params.roimapFile),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     0,
     0,
     FLAGS},
    {"non_roi_file",
     "Input file for nonroimap region filter. [filter.txt]",
     OFFSET(jpeg_option_params.nonRoiFilter),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     0,
     0,
     FLAGS},
    {"non_roi_level",
     "non_roi filter level (0 - 10), 10:user define ",
     OFFSET(jpeg_option_params.nonRoiLevel),
     AV_OPT_TYPE_INT,
     {.i64 = 10},
     0,
     10,
     FLAGS},
    {"enable_roi",
     "enable or disable the roi feature  ",
     OFFSET(jpeg_option_params.roi_enable),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     1,
     FLAGS},
    // mjpeg
    {"rc_mode",
     "Rate control mode, 0: single frame, 1: CBR, 2: VBR.",
     OFFSET(jpeg_option_params.rcMode),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     2,
     FLAGS},
    {"bit_per_second",
     "Target bits per second, 0: RC off, other: bit number.(unused)",
     OFFSET(jpeg_option_params.bitPerSecond),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     1048575,
     FLAGS},
    {"fixed_qp",
     "Fixed QP number for each frame.",
     OFFSET(jpeg_option_params.fixedQP),
     AV_OPT_TYPE_INT,
     {.i64 = -1},
     -1,
     51,
     FLAGS},
    {"frame_rate_num",
     "Frame rate number.(unused)",
     OFFSET(jpeg_option_params.frameRateNum),
     AV_OPT_TYPE_INT,
     {.i64 = 30},
     1,
     1048575,
     FLAGS},
    {"frame_rate_denom",
     "Frame rate denominator.(unused)",
     OFFSET(jpeg_option_params.frameRateDenom),
     AV_OPT_TYPE_INT,
     {.i64 = 1},
     1,
     1048575,
     FLAGS},
    {"pic_qp_delta_min",
     "Picture level QP delta min value.",
     OFFSET(jpeg_option_params.picQpDeltaMin),
     AV_OPT_TYPE_INT,
     {.i64 = -2},
     -10,
     -1,
     FLAGS},
    {"pic_qp_delta_max",
     "Picture level QP delta max value.",
     OFFSET(jpeg_option_params.picQpDeltaMax),
     AV_OPT_TYPE_INT,
     {.i64 = 3},
     1,
     10,
     FLAGS},
    {"qp_min", "Min QP value.(unused)", OFFSET(jpeg_option_params.qpmin), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 51, FLAGS},
    {"qp_max", "Max QP value.(unused)", OFFSET(jpeg_option_params.qpmax), AV_OPT_TYPE_INT, {.i64 = 51}, 0, 51, FLAGS},
    // marker type
    {"marker_type",
     "Quantization/Huffman table markers type, 0 = Single marker 1 = Multiple markers",
     OFFSET(jpeg_option_params.markerType),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     1,
     FLAGS},
    // line buffer
    {"linebuf_mode",
     "Line buffer mode, 0=disable; 1=SW Loopback enabled; 3=SW Loopback disable",
     OFFSET(jpeg_option_params.inputLineBufMode),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     4,
     FLAGS},
    {"linebuf_depth",
     "Number of MCU rows to control loop-back/handshaking,0 is only allowed with linebuf_mode = 3",
     OFFSET(jpeg_option_params.inputLineBufDepth),
     AV_OPT_TYPE_INT,
     {.i64 = 1},
     0,
     511,
     FLAGS},
    {"linebuf_amount",
     "Handshake sync amount for every loopback",
     OFFSET(jpeg_option_params.amountPerLoopBack),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     1023,
     FLAGS},
    {"stride_align",
     "set the stride alignment of input frame, multiple of 16",
     OFFSET(jpeg_option_params.exp_of_input_alignment),
     AV_OPT_TYPE_INT,
     {.i64 = 64},
     0,
     4096,
     FLAGS},
    {"partial_coding",
     "Encode a picture as several slices(partial Coding), 0 = disable, 1 = enable",
     OFFSET(jpeg_option_params.partialCoding),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     1,
     FLAGS},
    {"restart_interval",
     "Restart interval in MCU rows, each MCU row has a height of 16 pixels ",
     OFFSET(jpeg_option_params.restartInterval),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     1048575,
     FLAGS},
    {NULL},
};

static const AVCodecHWConfigInternal *es_jpeg_encode_hw_configs[] = {
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

static const FFCodecDefault es_jpeg_encode_defaults[] = {
    {NULL},
};

static const AVClass es_jpeg_encode_class = {
    .class_name = JPEG_ES_ENCODER_STR,
    .item_name = av_default_item_name,
    .option = es_jpeg_encode_options,
    .version = LIBAVUTIL_VERSION_INT,
};

/* jpeg support such input pix_fmt */
static const enum AVPixelFormat es_jenc_support_pixfmts[] = {AV_PIX_FMT_ES,
                                                             AV_PIX_FMT_YUV420P,
                                                             AV_PIX_FMT_NV12,
                                                             AV_PIX_FMT_NV21,
                                                             AV_PIX_FMT_YUYV422,
                                                             AV_PIX_FMT_UYVY422,
                                                             AV_PIX_FMT_P010LE,
                                                             AV_PIX_FMT_YUV420P10LE,
                                                             /*todo: test add more supported pixfmt*/
                                                             AV_PIX_FMT_NONE};

/**-------------------------FUNCTION------------------------------------**/
/**Private function ,print the params info*/
static void es_jenc_print_params(AVCodecContext *avctx, EsJpegEncParams *params) {
    if (!params) {
        av_log(avctx, AV_LOG_ERROR, "Params args invalid: Null pointer found.\n");
        return;
    }
    av_log(NULL, AV_LOG_INFO, "\n\t**********************************************************\n");
    av_log(NULL, AV_LOG_INFO, "\n\t-ESW JPEG: ENCODER PARAMS\n");
    av_log(NULL, AV_LOG_INFO, "");
    if (params->qLevel == USER_DEFINED_QTABLE) {
        av_log(avctx, AV_LOG_INFO, "JPEG: User Define qTableLuma \n");
    }
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: qp \t\t\t:%d\n", params->qLevel);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: inputWidth \t\t:%d\n", params->lumWidthSrc);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: inputHeight \t\t:%d\n", params->lumHeightSrc);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: outputWidth  \t\t:%d\n", params->width);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: outputHeight \t\t:%d\n", params->height);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: horOffsetSrc \t\t:%d\n", params->horOffsetSrc);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: verOffsetSrc \t\t:%d\n", params->verOffsetSrc);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: restartInterval \t\t:%d\n", params->restartInterval);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: frameType \t\t:%d\n", params->frameType);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: colorConversion \t\t:%d\n", params->colorConversion);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: rotation \t\t:%d\n", params->rotation);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: partialCoding \t\t:%d\n", params->partialCoding);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: codingMode \t\t:%d\n", params->codingMode);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: markerType \t\t:%d\n", params->markerType);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: unitsType \t\t:%d\n", params->unitsType);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: xdensity \t\t:%d\n", params->xdensity);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: ydensity \t\t:%d\n", params->ydensity);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: thumbnailformat \t\t:%d\n", params->thumbnailFormat);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: widthThumb \t\t:%d\n", params->widthThumb);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: heightThumb \t\t:%d\n", params->heightThumb);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: inputLineBufMode \t:%d\n", params->inputLineBufMode);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: inputLineBufDepth \t:%d\n", params->inputLineBufDepth);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: amountPerLoopBack \t:%d\n", params->amountPerLoopBack);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: mirror \t\t\t:%d\n", params->mirror);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: formatCustomizedType \t:%d\n", params->formatCustomizedType);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: constChromaEn \t\t:%d\n", params->constChromaEn);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: constCb \t\t\t:%u\n", params->constCb);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: constCr \t\t\t:%u\n", params->constCr);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: picQpDeltaMin \t\t:%i\n", params->picQpDeltaMin);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: picQpDeltaMax \t\t:%i\n", params->picQpDeltaMax);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: losslessEnable \t\t:%d\n", params->losslessEnable);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: predictMode \t\t:%d\n", params->predictMode);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: exp_of_input_alignment \t:%u\n", params->exp_of_input_alignment);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: streamBufChain \t\t:%u\n", params->streamBufChain);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: streamMultiSegmentMode \t:%u\n", params->streamMultiSegmentMode);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: mosaicEnables \t\t:%u\n", params->mosaicEnables);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: sramPowerdownDisable \t:%u\n", params->sramPowerdownDisable);
    av_log(avctx, AV_LOG_INFO, "\t-JPEG: burstMaxLength \t\t:%u\n", params->burstMaxLength);
    av_log(NULL, AV_LOG_INFO, "\n\t**********************************************************\n\n");
}

/**initialization the codec’s params*/
JpegEncRet es_jenc_init_params(AVCodecContext *avctx) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    EsJpegEncParams *params = &(jpeg_enc_ctx->jpeg_enc_params);
    EsJpegEncParams *option_params = &(jpeg_enc_ctx->jpeg_option_params);
    CropInfo crop_info;
    JpegEncRet ret_value;
    enum AVPixelFormat pix_fmt = avctx->pix_fmt;
    int counter;

    av_log(avctx, AV_LOG_INFO, "es_jenc_init_params() IN\n");
    memset(params, 0, sizeof(EsJpegEncParams));

    /*encode compress rate, default 10*/
    jpeg_enc_ctx->compress_rate = 10;

    /*stride must multiple of 16*/
    if (option_params->exp_of_input_alignment && option_params->exp_of_input_alignment % 16) {
        av_log(avctx, AV_LOG_ERROR, "in_align %d is not multiple of 16\n", option_params->exp_of_input_alignment);
        return JPEGENC_ERROR;
    }
    params->exp_of_input_alignment = log2(option_params->exp_of_input_alignment);

    // check input width and height
    params->lumWidthSrc = FFALIGN(avctx->width, 1 << params->exp_of_input_alignment);
    params->lumHeightSrc = avctx->height;

    // check output width and height
    params->width = avctx->width;
    params->height = avctx->height;

    // Parse crop string.
    if (ff_codec_get_crop(jpeg_enc_ctx->crop_str, &crop_info)) {
        av_log(avctx, AV_LOG_ERROR, "parser crop config error\n");
        return -1;
    }

    av_log(avctx,
           AV_LOG_DEBUG,
           "crop info: w:%d, h:%d, x:%d, y:%d\n",
           crop_info.crop_width,
           crop_info.crop_height,
           crop_info.crop_xoffset,
           crop_info.crop_yoffset);

    if (crop_info.crop_width > 0) params->width = crop_info.crop_width;
    if (crop_info.crop_height > 0) params->height = crop_info.crop_height;

    if (crop_info.crop_xoffset >= 0) params->horOffsetSrc = crop_info.crop_xoffset;
    if (crop_info.crop_yoffset >= 0) params->verOffsetSrc = crop_info.crop_yoffset;

    av_log(avctx,
           AV_LOG_INFO,
           "avctx: src wxh [%dx%d],dest wxh [%dx%d] \n",
           params->lumWidthSrc,
           params->lumHeightSrc,
           params->width,
           params->height);

    params->useVcmd = -1;
    jpeg_enc_ctx->roi_enable = option_params->roi_enable;
    params->roimapFile = option_params->roimapFile;
    params->nonRoiFilter = option_params->nonRoiFilter;
    params->nonRoiLevel = option_params->nonRoiLevel;

    // pix_fmt:
    if (pix_fmt == AV_PIX_FMT_ES) {
        pix_fmt = avctx->sw_pix_fmt;
    }
    av_log(avctx, AV_LOG_INFO, "pix_fmt: %s\n", av_get_pix_fmt_name(pix_fmt));
    switch (pix_fmt) {
        case AV_PIX_FMT_YUV420P:
            params->frameType = JPEGENC_YUV420_PLANAR;
            break;
        case AV_PIX_FMT_NV12:
            params->frameType = JPEGENC_YUV420_SEMIPLANAR;
            break;
        case AV_PIX_FMT_NV21:
            params->frameType = JPEGENC_YUV420_SEMIPLANAR_VU;
            break;
        case AV_PIX_FMT_UYVY422:
            params->frameType = JPEGENC_YUV422_INTERLEAVED_UYVY;
            break;
        case AV_PIX_FMT_YUYV422:
            params->frameType = JPEGENC_YUV422_INTERLEAVED_YUYV;
            break;
        case AV_PIX_FMT_YUV420P10LE:
            params->frameType = JPEGENC_YUV420_I010;
            break;
        case AV_PIX_FMT_P010LE:
            params->frameType = JPEGENC_YUV420_MS_P010;
            break;
        default:
            params->frameType = JPEGENC_YUV420_PLANAR;
            break;
    }

    // Thumbnail.
    ret_value = es_jenc_init_thumbnail(avctx);
    if (ret_value != JPEGENC_OK) {
        av_log(avctx, AV_LOG_INFO, "thumbnail init failed\n");
    }

    // mosaic VC unsupported
    params->mosaicEnables = 0;

    // Rotation.
    params->rotation = option_params->rotation;

    // Quantization
    params->qLevel = 1;
    ret_value = es_jenc_set_quant_table(avctx);
    if (ret_value != JPEGENC_OK) {
        av_log(avctx, AV_LOG_INFO, "quantization set failed\n");
    }

    // Slice mode
    params->partialCoding = option_params->partialCoding;
    params->restartInterval = option_params->restartInterval;

    params->colorConversion = 0;
    params->codingMode = JPEGENC_420_MODE;
    params->markerType = option_params->markerType;
    params->unitsType = option_params->unitsType;
    params->xdensity = option_params->xdensity;
    params->ydensity = option_params->ydensity;
    params->comLength = 0;
    params->inputLineBufMode = option_params->inputLineBufMode;
    jpeg_enc_ctx->input_line_buf_mode = params->inputLineBufMode;
    params->inputLineBufDepth = option_params->inputLineBufDepth;
    params->amountPerLoopBack = option_params->amountPerLoopBack;
    params->hashtype = 0;
    params->mirror = option_params->mirror;
    params->formatCustomizedType = -1;
    params->constChromaEn = option_params->constChromaEn;
    params->constCb = option_params->constCb;
    params->constCr = option_params->constCr;

    // lossless VC unsupported
    params->predictMode = 0;
    params->ptransValue = 0;

    // RC
    params->rcMode = option_params->rcMode;
    if (params->rcMode != JPEGENC_SINGLEFRAME) {
        // rc setting, using ffmpeg common option
        if (params->partialCoding) {
            params->frameRateNum = 1;
            params->frameRateDenom = 1;
        } else {
            params->frameRateNum = avctx->framerate.num;
            params->frameRateDenom = avctx->framerate.den;
        }
        params->bitPerSecond = avctx->bit_rate;
        params->fixedQP = option_params->fixedQP;
    } else {
        params->frameRateNum = 1;
        params->frameRateDenom = 1;
        params->bitPerSecond = 0;
        params->fixedQP = -1;
    }
    params->qpmin = avctx->qmin;
    params->qpmax = avctx->qmax;
    params->picQpDeltaMin = option_params->picQpDeltaMin;
    params->picQpDeltaMax = option_params->picQpDeltaMax;

    av_log(avctx,
           AV_LOG_INFO,
           "rc info: rcMode:%d, bps:%d, qpmin:%d, qpmax:%d, frameRateNum:%d, frameRateDenom:%d, "
           "picQpDeltaMin:%d, "
           "picQpDeltaMax:%d, fixedQP:%d \n",
           params->rcMode,
           params->bitPerSecond,
           params->qpmin,
           params->qpmax,
           params->frameRateNum,
           params->frameRateDenom,
           params->picQpDeltaMin,
           params->picQpDeltaMax,
           params->fixedQP);

    params->streamBufChain = 0;
    params->streamMultiSegmentMode = 0;
    params->streamMultiSegmentAmount = 4;
    strcpy(params->dec400CompTableinput, "dec400CompTableinput.bin");
    params->AXIAlignment = 0;
    params->irqTypeMask = 0x1f0;

    /*Overlay*/
    params->overlayEnables = 0;
    strcpy(params->osdDec400CompTableInput, "osdDec400CompTableinput.bin");

    for (counter = 0; counter < MAX_OVERLAY_NUM; counter++) {
        strcpy(params->olInput[counter], "olInput.yuv");
        params->olFormat[counter] = 0;
        params->olAlpha[counter] = 0;
        params->olWidth[counter] = 0;
        params->olHeight[counter] = 0;
        params->olXoffset[counter] = 0;
        params->olYoffset[counter] = 0;
        params->olYStride[counter] = 0;
        params->olUVStride[counter] = 0;
        params->olSuperTile[counter] = 0;
        params->olScaleWidth[counter] = 0;
        params->olScaleHeight[counter] = 0;
    }

    params->sramPowerdownDisable = 0;
    return JPEGENC_OK;
}

/**Set the codec’s params, If some params you want to fix , call es_jenc_get_params first*/
JpegEncRet es_jenc_set_params(AVCodecContext *avctx, EsJpegEncParams *params) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    EsJpegEncParams *destParams = &(jpeg_enc_ctx->jpeg_enc_params);

    av_log(avctx, AV_LOG_INFO, "es_jenc_set_params()\n");

    if (!params) {
        av_log(avctx, AV_LOG_ERROR, "Params args invalid: Null pointer found.\n");
        return JPEGENC_INVALID_ARGUMENT;
    }
    memcpy(destParams, params, sizeof(EsJpegEncParams));
    es_jenc_print_params(avctx, destParams);
    return JPEGENC_OK;
}

/**Get Current the codec’s params, */
JpegEncRet es_jenc_get_params(AVCodecContext *avctx, EsJpegEncParams *params) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    EsJpegEncParams *srcParams = &(jpeg_enc_ctx->jpeg_enc_params);

    av_log(avctx, AV_LOG_INFO, "es_jenc_get_params()\n");

    if (!params) {
        av_log(avctx, AV_LOG_ERROR, "Params args invalid: Null pointer found.\n");
        return JPEGENC_INVALID_ARGUMENT;
    }

    memset(params, 0, sizeof(EsJpegEncParams));
    memcpy(params, srcParams, sizeof(EsJpegEncParams));
    return JPEGENC_OK;
}

static JpegEncRet es_jenc_init_codec(AVCodecContext *avctx) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    JpegEncCfg *enc_config = &jpeg_enc_ctx->enc_config;
    JpegEncInst *enc_inst = &jpeg_enc_ctx->enc_inst;
    JpegEncRet ret_value = JPEGENC_OK;

    av_log(avctx, AV_LOG_INFO, "es_jenc_init_codec\n");

    if ((ret_value = JpegEncInit(enc_config, enc_inst, NULL)) != JPEGENC_OK) {
        av_log(avctx, AV_LOG_ERROR, "JpegEncInit failed\n");
    }
    av_log(avctx, AV_LOG_INFO, "JpegEncInit: ret_value = %d, enc_inst = %p.\n", (int)ret_value, enc_inst);

    return ret_value;
}

static int es_jenc_realloc_output_buffer(AVCodecContext *avctx, int new_size) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    JpegEncInst enc_inst = jpeg_enc_ctx->enc_inst;
    i32 counter = 0;
    const void *ewl_inst = JpegEncGetEwl(enc_inst);

    EWLFreeLinear(ewl_inst, &jpeg_enc_ctx->outbufMem[0]);
    memset(jpeg_enc_ctx->outbufMem, 0, sizeof(jpeg_enc_ctx->outbufMem));
    jpeg_enc_ctx->outbufMem[counter].mem_type = VPU_WR | CPU_WR | CPU_RD | EWL_MEM_TYPE_SLICE;
    if (EWLMallocLinear(ewl_inst, new_size, 0, &jpeg_enc_ctx->outbufMem[counter]) != EWL_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to allocate output buffer!\n");
        jpeg_enc_ctx->outbufMem[counter].virtualAddress = NULL;
        return -1;
    }
    av_log(
        avctx, AV_LOG_INFO, "Output buffer%d size:         %u bytes\n", counter, jpeg_enc_ctx->outbufMem[counter].size);
    av_log(avctx,
           AV_LOG_INFO,
           "Output buffer%d bus address:  %p\n",
           counter,
           (void *)jpeg_enc_ctx->outbufMem[counter].busAddress);
    av_log(avctx,
           AV_LOG_INFO,
           "Output buffer%d user address: %10p\n",
           counter,
           jpeg_enc_ctx->outbufMem[counter].virtualAddress);
    return 0;
}

static JpegEncRet es_jenc_free_codec_res(AVCodecContext *avctx) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    JpegEncInst enc_inst = jpeg_enc_ctx->enc_inst;

    const void *ewl_inst = JpegEncGetEwl(enc_inst);

    av_log(avctx, AV_LOG_INFO, "es_jenc_free_codec_res()\n");

    EWLFreeLinear(ewl_inst, &jpeg_enc_ctx->outbufMem[0]);
    EWLFreeLinear(ewl_inst, &jpeg_enc_ctx->roimapMem);
    EWLFreeLinear(ewl_inst, &jpeg_enc_ctx->input_buf_mem);

    if (jpeg_enc_ctx->thumb_data != NULL) {
        free(jpeg_enc_ctx->thumb_data);
        jpeg_enc_ctx->thumb_data = NULL;
        jpeg_enc_ctx->thumbnail_enable = false;
    }

    return JPEGENC_OK;
}

static JpegEncRet es_jenc_alloc_codec_res(AVCodecContext *avctx) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    EsJpegEncParams *jpeg_params = &(jpeg_enc_ctx->jpeg_enc_params);
    JpegEncInst enc_inst = jpeg_enc_ctx->enc_inst;
    EWLLinearMem_t *pictureMem = &(jpeg_enc_ctx->input_buf_mem);

    i32 sliceRows = 0;
    u64 pictureSize;
    u32 streamBufTotalSize;

    i32 counter;
    u64 lumaSize, chromaSize;
    u32 input_alignment = (jpeg_params->exp_of_input_alignment == 0 ? 0 : (1 << jpeg_params->exp_of_input_alignment));
    i32 mcuh = (jpeg_params->codingMode == JPEGENC_422_MODE ? 8 : 16);
    i32 strmBufNum = jpeg_params->streamBufChain ? 2 : 1;
    i32 bufSizes[2] = {0, 0};
    JpegEncRet ret_value;

    const void *ewl_inst = JpegEncGetEwl(jpeg_enc_ctx->enc_inst);

    if (jpeg_params->codingMode == JPEGENC_420_MODE) {
        jpeg_enc_ctx->mb_width = (jpeg_params->lumWidthSrc + 15) / MB_SIZE_16;
        jpeg_enc_ctx->mb_height = (jpeg_params->lumHeightSrc + 15) / MB_SIZE_16;
    } else if (jpeg_params->codingMode == JPEGENC_422_MODE) {
        jpeg_enc_ctx->mb_width = (jpeg_params->lumWidthSrc + 15) / MB_SIZE_16;
        jpeg_enc_ctx->mb_height = (jpeg_params->lumHeightSrc + 7) / MB_SIZE_8;
    } else if (jpeg_params->codingMode == JPEGENC_MONO_MODE) {
        jpeg_enc_ctx->mb_width = (jpeg_params->lumWidthSrc + 7) / MB_SIZE_8;
        jpeg_enc_ctx->mb_height = (jpeg_params->lumHeightSrc + 7) / MB_SIZE_8;
    }

    av_log(avctx, AV_LOG_INFO, "es_jenc_alloc_codec_res\n");
    /* Set slice size and output buffer size
     * For output buffer size, 1 byte/pixel is enough for most images.
     * Some extra is needed for testing purposes (noise input) */

    if (jpeg_params->partialCoding == 0) {
        if (jpeg_params->frameType == JPEGENC_YUV420_PLANAR_8BIT_TILE_32_32)
            sliceRows = ((jpeg_params->lumHeightSrc + 32 - 1) & (~(32 - 1)));
        else if (jpeg_params->frameType == JPEGENC_YUV420_PLANAR_8BIT_TILE_16_16_PACKED_4)
            sliceRows = ((jpeg_params->lumHeightSrc + mcuh - 1) & (~(mcuh - 1)));
        else
            sliceRows = jpeg_params->lumHeightSrc;
    } else {
        sliceRows = jpeg_params->restartInterval * mcuh;
    }

    es_jenc_get_aligned_pic_size_by_format(jpeg_params->frameType,
                                           jpeg_params->lumWidthSrc,
                                           sliceRows,
                                           input_alignment,
                                           &lumaSize,
                                           &chromaSize,
                                           &pictureSize);

    JpegSetLumaSize(enc_inst, lumaSize, 0);
    JpegSetChromaSize(enc_inst, chromaSize, 0);

    if (jpeg_enc_ctx->compress_rate != 0) {
        // calculate output buffer size: input_buffer_size / compress_rate
        streamBufTotalSize = pictureSize / jpeg_enc_ctx->compress_rate;
    } else {
        // calculate output buffer size reference testbench
        streamBufTotalSize = jpeg_params->lumWidthSrc * sliceRows * 2;
    }

    if (streamBufTotalSize < JPEGENC_STREAM_MIN_BUF0_SIZE) {
        jpeg_enc_ctx->compress_rate = pictureSize / (JPEGENC_STREAM_MIN_BUF0_SIZE);
        if (pictureSize % (JPEGENC_STREAM_MIN_BUF0_SIZE)) {
            jpeg_enc_ctx->compress_rate--;
            if (jpeg_enc_ctx->compress_rate < 2) jpeg_enc_ctx->compress_rate = 1;
        }
        streamBufTotalSize = pictureSize / jpeg_enc_ctx->compress_rate;
    }
    av_log(avctx,
           AV_LOG_INFO,
           "jenc alloc outbuf size: %d is 1/%d of YUV \n",
           streamBufTotalSize,
           jpeg_enc_ctx->compress_rate);

    pictureMem->virtualAddress = NULL;
    /* Here we use the EWL instance directly from the encoder
     * because it is the easiest way to allocate the linear memories */
    pictureMem->mem_type = EXT_WR | VPU_RD | EWL_MEM_TYPE_DPB;
    if (EWLMallocLinear(ewl_inst, pictureSize, 0, pictureMem) != EWL_OK) {
        fprintf(stderr, "Failed to allocate input picture!\n");
        pictureMem->virtualAddress = NULL;
        return JPEGENC_ERROR;
    }
    av_log(avctx, AV_LOG_INFO, "Input buffer size:         %u bytes\n", pictureMem->size);
    av_log(avctx, AV_LOG_INFO, "Input buffer bus address:  %p\n", (void *)pictureMem->busAddress);
    av_log(avctx, AV_LOG_INFO, "Input bufferuser address: %10p\n", pictureMem->virtualAddress);

    if (strmBufNum == 1) {
        bufSizes[0] = (jpeg_params->streamMultiSegmentMode != 0 ? streamBufTotalSize / 128 : streamBufTotalSize);
    } else {
        /* set small stream buffer0 to test two stream buffers */
        // stream buffer chain in win2030 is unsupported.
        bufSizes[0] = streamBufTotalSize / 100;
        bufSizes[1] = streamBufTotalSize - bufSizes[0];
    }
    av_log(avctx, AV_LOG_DEBUG, "dbg: real output buf size : bufSizes[0] : %d \n", bufSizes[0]);

    memset(jpeg_enc_ctx->outbufMem, 0, sizeof(jpeg_enc_ctx->outbufMem));
    for (counter = 0; counter < strmBufNum; counter++) {
        u32 size = bufSizes[counter];

        /* For FPGA testing, smaller size maybe specified. */
        /* Max output buffer size is less than 256MB */
        // comment out outbufSize hard limitation for 16K*16K testing
        // size = size < (1024*1024*64) ? size : (1024*1024*64);

        jpeg_enc_ctx->outbufMem[counter].mem_type = VPU_WR | CPU_WR | CPU_RD | EWL_MEM_TYPE_SLICE;
        if (EWLMallocLinear(ewl_inst, size, 0, &jpeg_enc_ctx->outbufMem[counter]) != EWL_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to allocate output buffer!\n");
            jpeg_enc_ctx->outbufMem[counter].virtualAddress = NULL;
            return JPEGENC_ERROR;
        }
    }
    // RoiMap assume jpeg_params->width/height % 16 == 0
    // alloc roimapMem
    if (jpeg_enc_ctx->roi_enable) {
        ret_value = es_jenc_alloc_roi_res(avctx);
        jpeg_enc_ctx->roi_buffer_alloced = TRUE;
        if (ret_value != 0) {
            return ret_value;
        }
    }

    /*Overlay input buffer*/
    // TODO: now overlay inputbuffer is unused.
    // TODO: dec400 buffer is unused
    // TODO: osdDec400 buffer is unused
#ifndef ASIC_WAVE_TRACE_TRIGGER
    if (jpeg_enc_ctx->thumbnail_enable) {
        av_log(avctx,
               AV_LOG_INFO,
               "Input pic[%dx%d] encoding at thumb[%dx%d] + pic[%dx%d] ",
               jpeg_params->lumWidthSrc,
               jpeg_params->lumHeightSrc,
               jpeg_params->widthThumb,
               jpeg_params->heightThumb,
               jpeg_params->width,
               jpeg_params->height);
    } else {
        av_log(avctx,
               AV_LOG_INFO,
               "Input pic [%dx%d] encoding at [%dx%d]",
               jpeg_params->lumWidthSrc,
               jpeg_params->lumHeightSrc,
               jpeg_params->width,
               jpeg_params->height);
    }

    if (jpeg_params->partialCoding != 0)
        av_log(avctx, AV_LOG_INFO, "in slices of %dx%d", jpeg_params->width, sliceRows);
    av_log(avctx, AV_LOG_INFO, "\n");
#endif

#ifndef ASIC_WAVE_TRACE_TRIGGER
    for (counter = 0; counter < strmBufNum; counter++) {
        av_log(avctx,
               AV_LOG_INFO,
               "Output buffer%d size:         %u bytes\n",
               counter,
               jpeg_enc_ctx->outbufMem[counter].size);
        av_log(avctx,
               AV_LOG_INFO,
               "Output buffer%d bus address:  %p\n",
               counter,
               (void *)jpeg_enc_ctx->outbufMem[counter].busAddress);
        av_log(avctx,
               AV_LOG_INFO,
               "Output buffer%d user address: %10p\n",
               counter,
               jpeg_enc_ctx->outbufMem[counter].virtualAddress);
    }
#endif
    return JPEGENC_OK;
}

static JpegEncRet es_jenc_alloc_roi_res(AVCodecContext *avctx) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    EsJpegEncParams *jpeg_params = &(jpeg_enc_ctx->jpeg_enc_params);

    u32 RoiSize = 0;
    u32 ImageWidthMB = jpeg_enc_ctx->mb_width;
    u32 ImageHeightMB = jpeg_enc_ctx->mb_height;
    u32 sliceRowsMb = 0;
    u32 RoiSizePerSlice = 0;

    const void *ewl_inst = JpegEncGetEwl(jpeg_enc_ctx->enc_inst);

    if (jpeg_params->partialCoding == JPEGENC_WHOLE_FRAME) {
        RoiSize = (ImageWidthMB * ImageHeightMB + 7) / MB_SIZE_8;
        sliceRowsMb = ImageHeightMB;
    } else {
        if (jpeg_params->codingMode == JPEGENC_420_MODE) {
            sliceRowsMb = jpeg_params->restartInterval;
        } else {
            sliceRowsMb = jpeg_params->restartInterval * 2;
        }
        RoiSizePerSlice = ((sliceRowsMb * ImageWidthMB + 7) / MB_SIZE_8 + 15) & (~15);
        RoiSize = RoiSizePerSlice * ((ImageHeightMB + sliceRowsMb - 1) / sliceRowsMb);
    }

    jpeg_enc_ctx->roimapMem.mem_type = EXT_WR | VPU_RD | EWL_MEM_TYPE_VPU_WORKING;
    if (EWLMallocLinear(ewl_inst, RoiSize, 0, &jpeg_enc_ctx->roimapMem) != EWL_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to allocate RoiMap Memory!\n");
        jpeg_enc_ctx->roimapMem.virtualAddress = NULL;
        return JPEGENC_MEMORY_ERROR;
    }
    memset(jpeg_enc_ctx->roimapMem.virtualAddress, 0, RoiSize);
    jpeg_enc_ctx->roi_memory_size = RoiSize;
    av_log(avctx, AV_LOG_INFO, "roi buffer size:         %u bytes\n", jpeg_enc_ctx->roimapMem.size);
    av_log(avctx, AV_LOG_INFO, "roi bufferbus address:  %p\n", (void *)jpeg_enc_ctx->roimapMem.busAddress);
    av_log(avctx, AV_LOG_INFO, "roi buffer user address: %10p\n", jpeg_enc_ctx->roimapMem.virtualAddress);
    return JPEGENC_OK;
}

static int dynamic_read_roimap(AVCodecContext *avctx, const AVFrame *frame) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    EsJpegEncParams *jpeg_params = &(jpeg_enc_ctx->jpeg_enc_params);

    // roi rect val
    i32 RoiRectNum = 0;
    u32 RoiRectLeft[30];
    u32 RoiRectTop[30];
    u32 RoiRectWidth[30];
    u32 RoiRectHeight[30];

    int ImageWidthMB = jpeg_enc_ctx->mb_width;
    int ImageHeightMB = jpeg_enc_ctx->mb_height;
    u32 sliceRowsMb = 0;

    u8 *roimap_virtualAddr = NULL;
    u32 bitNum = 0;
    u32 MbNumPerSlice = 0;
    u8 roiValTmp = 0;
    u8 RoiRegion_SetVal[8] = {0};
    int ret = 0;
    int nb_rois = 0;
    SideDataRoiArea *roi_area = NULL;

    AVFrameSideData *sd = av_frame_get_side_data(frame, SIDE_DATA_TYPE_JENC_ROI_AREA);
    if (sd) {
        roi_area = (SideDataRoiArea *)sd->data;
        nb_rois = sd->size / sizeof(SideDataRoiArea);
        if (!nb_rois || sd->size % sizeof(SideDataRoiArea) != 0) {
            av_log(avctx, AV_LOG_ERROR, "Invalid AVRegionOfInterest. self_size.\n");
            return AVERROR(EINVAL);
        }
    }

    if (jpeg_params->partialCoding == JPEGENC_WHOLE_FRAME) {
        sliceRowsMb = ImageHeightMB;
    } else {
        if (jpeg_params->codingMode == JPEGENC_420_MODE) {
            sliceRowsMb = jpeg_params->restartInterval;
        } else {
            sliceRowsMb = jpeg_params->restartInterval * 2;
        }
    }

    // This list must be iterated in reverse because the first
    // region in the list applies when regions overlap.
    for (int i = nb_rois - 1; i >= 0; i--) {
        if ((roi_area->width + roi_area->x) > jpeg_params->lumWidthSrc
            || (roi_area->height + roi_area->y) > jpeg_params->lumHeightSrc) {
            av_log(avctx,
                   AV_LOG_ERROR,
                   "(%d, %d, %d, %d) (%d, %d): Error, The Roi Region Coordinate Input Is Out Of Picture Range!\n",
                   roi_area->x,
                   roi_area->y,
                   roi_area->width,
                   roi_area->height,
                   jpeg_params->lumWidthSrc,
                   jpeg_params->lumHeightSrc);

            return -1;
        }

        roi_area = (SideDataRoiArea *)(sd->data + sizeof(SideDataRoiArea) * i);
        if (jpeg_params->codingMode == JPEGENC_420_MODE) {
            RoiRectLeft[RoiRectNum] = roi_area->x / MB_SIZE_16;
            RoiRectTop[RoiRectNum] = roi_area->y / MB_SIZE_16;
            RoiRectWidth[RoiRectNum] = (roi_area->width + 15) / MB_SIZE_16;
            RoiRectHeight[RoiRectNum] = (roi_area->height + 15) / MB_SIZE_16;
        } else if (jpeg_params->codingMode == JPEGENC_422_MODE) {
            RoiRectLeft[RoiRectNum] = roi_area->x / MB_SIZE_16;
            RoiRectTop[RoiRectNum] = roi_area->y / MB_SIZE_8;
            RoiRectWidth[RoiRectNum] = (roi_area->width + 15) / MB_SIZE_16;
            RoiRectHeight[RoiRectNum] = (roi_area->height + 7) / MB_SIZE_8;
        } else if (jpeg_params->codingMode == JPEGENC_MONO_MODE) {
            RoiRectLeft[RoiRectNum] = roi_area->x / MB_SIZE_8;
            RoiRectTop[RoiRectNum] = roi_area->y / MB_SIZE_8;
            RoiRectWidth[RoiRectNum] = (roi_area->width + 7) / MB_SIZE_8;
            RoiRectHeight[RoiRectNum] = (roi_area->height + 7) / MB_SIZE_8;
        }

        av_log(avctx,
               AV_LOG_INFO,
               "Rect%d: left:%d, top:%d, width:%d, heigh:%d\n",
               RoiRectNum,
               roi_area->x,
               roi_area->y,
               roi_area->width,
               roi_area->height);
        RoiRectNum++;
        // for (int y = starty; y < endy; y++) {
        //     for (int x = startx; x < endx; x++) {
        //         qoffsets[x + y * mbx] = qoffset;
        //     }
        // }
    }
    goto write_roi_bitmap;

write_roi_bitmap:
    memset(jpeg_enc_ctx->roimapMem.virtualAddress, 0, jpeg_enc_ctx->roi_memory_size);
    // ROI bitmap need 1
    // roimap_virtualAddr = (u8 *)roimapMem.virtualAddress;
    roimap_virtualAddr = (u8 *)(jpeg_enc_ctx->roimapMem.virtualAddress);
    for (int roinum = 0; roinum < RoiRectNum; roinum++) {
        roimap_virtualAddr = (u8 *)(jpeg_enc_ctx->roimapMem.virtualAddress);
        for (int rows = 0; rows < ImageHeightMB; rows++) {
            for (int cals = 0; cals < ImageWidthMB; cals++) {
                if ((rows >= RoiRectTop[roinum] && rows < (RoiRectTop[roinum] + RoiRectHeight[roinum]))
                    && cals >= RoiRectLeft[roinum] && cals < (RoiRectLeft[roinum] + RoiRectWidth[roinum]))  // ROI
                {
                    RoiRegion_SetVal[bitNum] = 1;
                } else {
                    RoiRegion_SetVal[bitNum] = 0;
                }
                bitNum++;
                if (bitNum == 8 || (rows == ImageHeightMB - 1 && cals == ImageWidthMB - 1)
                    || (MbNumPerSlice == sliceRowsMb * ImageWidthMB - 1)) {
                    roiValTmp = RoiRegion_SetVal[0] * 128 + RoiRegion_SetVal[1] * 64 + RoiRegion_SetVal[2] * 32
                                + RoiRegion_SetVal[3] * 16 + RoiRegion_SetVal[4] * 8 + RoiRegion_SetVal[5] * 4
                                + RoiRegion_SetVal[6] * 2 + RoiRegion_SetVal[7] * 1;
                    *roimap_virtualAddr = *roimap_virtualAddr | roiValTmp;
                    roimap_virtualAddr++;
                    if ((MbNumPerSlice == sliceRowsMb * ImageWidthMB - 1) && jpeg_params->partialCoding == 1) {
                        roimap_virtualAddr = (u8 *)(((ptr_t)roimap_virtualAddr + 15) & (~15));
                    }
                    roiValTmp = 0;
                    bitNum = 0;
                    memset(RoiRegion_SetVal, 0, sizeof(RoiRegion_SetVal));
                }
                MbNumPerSlice++;
                if (MbNumPerSlice == sliceRowsMb * ImageWidthMB
                    || (rows == ImageHeightMB - 1 && cals == ImageWidthMB - 1)) {
                    MbNumPerSlice = 0;
                }
            }
        }
    }

    ret = EWLSyncMemData(&jpeg_enc_ctx->roimapMem, 0, jpeg_enc_ctx->roi_memory_size, HOST_TO_DEVICE);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "SyncMemData failed\n");
        return ret;
    }

    return ret;
}

static int read_roimap(AVCodecContext *avctx) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    EsJpegEncParams *jpeg_params = &(jpeg_enc_ctx->jpeg_enc_params);
    // roi rect val
    i32 RoiRectNum = 0;
    u32 RoiRectLeft[30];
    u32 RoiRectTop[30];
    u32 RoiRectWidth[30];
    u32 RoiRectHeight[30];

    int ImageWidthMB = jpeg_enc_ctx->mb_width;
    int ImageHeightMB = jpeg_enc_ctx->mb_height;
    u32 sliceRowsMb = 0;

    u8 *roimap_virtualAddr = NULL;
    u32 bitNum = 0;
    u32 MbNumPerSlice = 0;
    u8 roiValTmp = 0;
    u8 RoiRegion_SetVal[8] = {0};
    int ret = 0;

    if (jpeg_params->partialCoding == JPEGENC_WHOLE_FRAME) {
        sliceRowsMb = ImageHeightMB;
    } else {
        if (jpeg_params->codingMode == JPEGENC_420_MODE) {
            sliceRowsMb = jpeg_params->restartInterval;
        } else {
            sliceRowsMb = jpeg_params->restartInterval * 2;
        }
    }
    // read roi map from file:
    if (jpeg_params->roimapFile != NULL) {
        char buf[30];
        FILE *fpROI = fopen(jpeg_params->roimapFile, "r");
        if (!fpROI) {
            av_log(avctx, AV_LOG_ERROR, "jpeg_map.roi, Can Not Open File %s\n", jpeg_params->roimapFile);
            return -1;
        }

        av_log(avctx, AV_LOG_INFO, "read roi map from file:%s \n", jpeg_params->roimapFile);

        while (fgets(buf, 30, fpROI) != NULL) {
            if (buf[0] == 'r') {
                sscanf(buf,
                       "roi=(%d,%d,%d,%d)",
                       &RoiRectLeft[RoiRectNum],
                       &RoiRectTop[RoiRectNum],
                       &RoiRectWidth[RoiRectNum],
                       &RoiRectHeight[RoiRectNum]);
                if (RoiRectLeft[RoiRectNum] + RoiRectWidth[RoiRectNum] > jpeg_params->lumWidthSrc
                    || RoiRectTop[RoiRectNum] + RoiRectHeight[RoiRectNum] > jpeg_params->height) {
                    printf("jpeg_map.roi: Error, The Roi Region Coordinate Input Is Out Of Picture Range!\n");
                    return -1;
                }
                av_log(avctx,
                       AV_LOG_INFO,
                       "Rect%d: left:%d, top:%d, width:%d, heigh:%d\n",
                       RoiRectNum,
                       RoiRectLeft[RoiRectNum],
                       RoiRectTop[RoiRectNum],
                       RoiRectWidth[RoiRectNum],
                       RoiRectHeight[RoiRectNum]);
                if (jpeg_params->codingMode == JPEGENC_420_MODE) {
                    RoiRectLeft[RoiRectNum] = RoiRectLeft[RoiRectNum] / MB_SIZE_16;
                    RoiRectTop[RoiRectNum] = RoiRectTop[RoiRectNum] / MB_SIZE_16;
                    RoiRectWidth[RoiRectNum] = (RoiRectWidth[RoiRectNum] + 15) / MB_SIZE_16;
                    RoiRectHeight[RoiRectNum] = (RoiRectHeight[RoiRectNum] + 15) / MB_SIZE_16;
                } else if (jpeg_params->codingMode == JPEGENC_422_MODE) {
                    RoiRectLeft[RoiRectNum] = RoiRectLeft[RoiRectNum] / MB_SIZE_16;
                    RoiRectTop[RoiRectNum] = RoiRectTop[RoiRectNum] / MB_SIZE_8;
                    RoiRectWidth[RoiRectNum] = (RoiRectWidth[RoiRectNum] + 15) / MB_SIZE_16;
                    RoiRectHeight[RoiRectNum] = (RoiRectHeight[RoiRectNum] + 7) / MB_SIZE_8;
                } else if (jpeg_params->codingMode == JPEGENC_MONO_MODE) {
                    RoiRectLeft[RoiRectNum] = RoiRectLeft[RoiRectNum] / MB_SIZE_8;
                    RoiRectTop[RoiRectNum] = RoiRectTop[RoiRectNum] / MB_SIZE_8;
                    RoiRectWidth[RoiRectNum] = (RoiRectWidth[RoiRectNum] + 7) / MB_SIZE_8;
                    RoiRectHeight[RoiRectNum] = (RoiRectHeight[RoiRectNum] + 7) / MB_SIZE_8;
                }

                RoiRectNum++;
            }
        }
        fclose(fpROI);
        goto write_roi_bitmap;
    }

write_roi_bitmap:
    memset(jpeg_enc_ctx->roimapMem.virtualAddress, 0, jpeg_enc_ctx->roi_memory_size);
    // ROI bitmap need 1
    // roimap_virtualAddr = (u8 *)roimapMem.virtualAddress;
    roimap_virtualAddr = (u8 *)(jpeg_enc_ctx->roimapMem.virtualAddress);
    for (int roinum = 0; roinum < RoiRectNum; roinum++) {
        roimap_virtualAddr = (u8 *)(jpeg_enc_ctx->roimapMem.virtualAddress);
        for (int rows = 0; rows < ImageHeightMB; rows++) {
            for (int cals = 0; cals < ImageWidthMB; cals++) {
                if ((rows >= RoiRectTop[roinum] && rows < (RoiRectTop[roinum] + RoiRectHeight[roinum]))
                    && cals >= RoiRectLeft[roinum] && cals < (RoiRectLeft[roinum] + RoiRectWidth[roinum]))  // ROI
                {
                    RoiRegion_SetVal[bitNum] = 1;
                } else {
                    RoiRegion_SetVal[bitNum] = 0;
                }
                bitNum++;
                if (bitNum == 8 || (rows == ImageHeightMB - 1 && cals == ImageWidthMB - 1)
                    || (MbNumPerSlice == sliceRowsMb * ImageWidthMB - 1)) {
                    roiValTmp = RoiRegion_SetVal[0] * 128 + RoiRegion_SetVal[1] * 64 + RoiRegion_SetVal[2] * 32
                                + RoiRegion_SetVal[3] * 16 + RoiRegion_SetVal[4] * 8 + RoiRegion_SetVal[5] * 4
                                + RoiRegion_SetVal[6] * 2 + RoiRegion_SetVal[7] * 1;
                    *roimap_virtualAddr = *roimap_virtualAddr | roiValTmp;
                    roimap_virtualAddr++;
                    if ((MbNumPerSlice == sliceRowsMb * ImageWidthMB - 1) && jpeg_params->partialCoding == 1) {
                        roimap_virtualAddr = (u8 *)(((ptr_t)roimap_virtualAddr + 15) & (~15));
                    }
                    roiValTmp = 0;
                    bitNum = 0;
                    memset(RoiRegion_SetVal, 0, sizeof(RoiRegion_SetVal));
                }
                MbNumPerSlice++;
                if (MbNumPerSlice == sliceRowsMb * ImageWidthMB
                    || (rows == ImageHeightMB - 1 && cals == ImageWidthMB - 1)) {
                    MbNumPerSlice = 0;
                }
            }
        }
    }

    ret = EWLSyncMemData(&jpeg_enc_ctx->roimapMem, 0, jpeg_enc_ctx->roi_memory_size, HOST_TO_DEVICE);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "SyncMemData failed\n");
        return ret;
    }

    return ret;
}

static int dynamic_read_non_roi_filter(AVCodecContext *avctx, SideDataJencNonRoiFilter *sd_non_roi_filter) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    JpegEncCfg *enc_config = &jpeg_enc_ctx->enc_config;

    if (sd_non_roi_filter->non_roi_qp_level == 10) {
        av_log(avctx, AV_LOG_INFO, "Use user qp map\n");
        for (int i = 0; i < 128; i++) {
            enc_config->filter[i] = sd_non_roi_filter->non_roi_filter_map[i];
        }
    } else if (sd_non_roi_filter->non_roi_qp_level <= 9) {
        for (int j = 0; j < 64; j++) {
            enc_config->filter[j] = NonRoiFilterLuminance[sd_non_roi_filter->non_roi_qp_level][j];
            enc_config->filter[j + 64] = NonRoiFilterChrominance[sd_non_roi_filter->non_roi_qp_level][j];
        }
    } else {
        av_log(avctx, AV_LOG_INFO, "non roi fliter not set\n");
        return 1;
    }

    // print fliter:
    av_log(avctx, AV_LOG_INFO, "non_roi_qp_level:%d  print non ROI fliter: \n ", sd_non_roi_filter->non_roi_qp_level);
    av_log(NULL,
           AV_LOG_INFO,
           "luma: %d %d %d %d %d %d %d %d",
           enc_config->filter[0],
           enc_config->filter[1],
           enc_config->filter[2],
           enc_config->filter[3],
           enc_config->filter[4],
           enc_config->filter[5],
           enc_config->filter[6],
           enc_config->filter[7]);
    av_log(NULL,
           AV_LOG_INFO,
           "chroma: %d %d %d %d %d %d %d %d",
           enc_config->filter[64],
           enc_config->filter[65],
           enc_config->filter[66],
           enc_config->filter[67],
           enc_config->filter[68],
           enc_config->filter[69],
           enc_config->filter[70],
           enc_config->filter[71]);
    // num = 0;
    // while (num < 128) {
    //     for (int i = 0; i < 8; i++) {
    //         av_log(NULL, AV_LOG_INFO, "%d ", enc_config->filter[num]);
    //         num++;
    //     }
    //     av_log(NULL, AV_LOG_INFO, "\n ");
    // }

    return 0;
}

static int read_non_roi_fliter(AVCodecContext *avctx) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    EsJpegEncParams *jpeg_params = &(jpeg_enc_ctx->jpeg_enc_params);
    JpegEncCfg *enc_config = &jpeg_enc_ctx->enc_config;

    FILE *fpFilter;
    char buf[32];
    int val[8];
    int num = 0;

    av_log(avctx,
           AV_LOG_DEBUG,
           "roimapFile = %s, nonRoiFilter = %s,nonRoiLevel = %d \n",
           jpeg_params->roimapFile,
           jpeg_params->nonRoiFilter,
           jpeg_params->nonRoiLevel);

    if (jpeg_params->nonRoiFilter) {
        fpFilter = fopen(jpeg_params->nonRoiFilter, "r");
        if (fpFilter == NULL) {
            av_log(avctx, AV_LOG_ERROR, "filter.txt: Error, Can Not Open File %s\n", jpeg_params->nonRoiFilter);
            return -1;
        }
        while (fgets(buf, 32, fpFilter) != NULL) {
            if (buf[0] == '#' || buf[0] == '\r' || buf[0] == '\n') {
                continue;
            }
            sscanf(
                buf, "%d %d %d %d %d %d %d %d", &val[0], &val[1], &val[2], &val[3], &val[4], &val[5], &val[6], &val[7]);
            for (int i = 0; i < 8; i++) {
                enc_config->filter[num] = (u8)val[i];
                num++;
            }
        }
        fclose(fpFilter);
    } else if (jpeg_params->nonRoiLevel >= 0 && jpeg_params->nonRoiLevel <= 9) {
        for (int j = 0; j < 64; j++) {
            enc_config->filter[j] = NonRoiFilterLuminance[jpeg_params->nonRoiLevel][j];
            enc_config->filter[j + 64] = NonRoiFilterChrominance[jpeg_params->nonRoiLevel][j];
        }
    } else {
        av_log(avctx, AV_LOG_INFO, "non roi fliter not set\n");
        return 1;
    }

    // print fliter:
    av_log(avctx, AV_LOG_INFO, " print non ROI fliter: \n ");
    av_log(NULL,
           AV_LOG_INFO,
           "luma: %d %d %d %d %d %d %d %d",
           enc_config->filter[0],
           enc_config->filter[1],
           enc_config->filter[2],
           enc_config->filter[3],
           enc_config->filter[4],
           enc_config->filter[5],
           enc_config->filter[6],
           enc_config->filter[7]);
    av_log(NULL,
           AV_LOG_INFO,
           "chroma: %d %d %d %d %d %d %d %d",
           enc_config->filter[64],
           enc_config->filter[65],
           enc_config->filter[66],
           enc_config->filter[67],
           enc_config->filter[68],
           enc_config->filter[69],
           enc_config->filter[70],
           enc_config->filter[71]);

    // num = 0;
    // while (num < 128) {
    //     for (int i = 0; i < 8; i++) {
    //         av_log(NULL, AV_LOG_INFO, "%d ", enc_config->filter[num]);
    //         num++;
    //     }
    //     av_log(NULL, AV_LOG_INFO, "\n ");
    // }

    return 0;
}

void es_jenc_get_aligned_pic_size_by_format(
    JpegEncFrameType type, u32 width, u32 height, u32 alignment, u64 *luma_size, u64 *chroma_size, u64 *picture_size) {
    u32 luma_stride = 0, chroma_stride = 0;
    u64 lumaSize = 0, chromaSize = 0, pictureSize = 0;

    JpegEncGetAlignedStride(width, type, &luma_stride, &chroma_stride, alignment);
    switch (type) {
        case JPEGENC_YUV420_PLANAR:
        case JPEGENC_YVU420_PLANAR:
            lumaSize = (u64)luma_stride * height;
            chromaSize = (u64)chroma_stride * height / 2 * 2;
            break;
        case JPEGENC_YUV420_SEMIPLANAR:
        case JPEGENC_YUV420_SEMIPLANAR_VU:
            lumaSize = (u64)luma_stride * height;
            chromaSize = (u64)chroma_stride * height / 2;
            break;
        case JPEGENC_YUV422_INTERLEAVED_YUYV:
        case JPEGENC_YUV422_INTERLEAVED_UYVY:
        case JPEGENC_RGB565:
        case JPEGENC_BGR565:
        case JPEGENC_RGB555:
        case JPEGENC_BGR555:
        case JPEGENC_RGB444:
        case JPEGENC_BGR444:
        case JPEGENC_RGB888:
        case JPEGENC_BGR888:
        case JPEGENC_RGB101010:
        case JPEGENC_BGR101010:
            lumaSize = (u64)luma_stride * height;
            chromaSize = 0;
            break;
        case JPEGENC_YUV420_I010:
            lumaSize = (u64)luma_stride * height;
            chromaSize = (u64)chroma_stride * height / 2 * 2;
            break;
        case JPEGENC_YUV420_MS_P010:
            lumaSize = (u64)luma_stride * height;
            chromaSize = (u64)chroma_stride * height / 2;
            break;
        case JPEGENC_YUV420_PLANAR_8BIT_TILE_32_32:
            lumaSize = (u64)luma_stride * height;
            chromaSize = (u64)lumaSize / 2;
            break;
        case JPEGENC_YUV420_PLANAR_8BIT_TILE_16_16_PACKED_4:
            lumaSize = (u64)luma_stride * height * 2 * 12 / 8;
            chromaSize = 0;
            break;
        case JPEGENC_YUV420_SEMIPLANAR_8BIT_TILE_4_4:
        case JPEGENC_YUV420_SEMIPLANAR_VU_8BIT_TILE_4_4:
        case JPEGENC_YUV420_PLANAR_10BIT_P010_TILE_4_4:
            lumaSize = (u64)luma_stride * ((height + 3) / 4);
            chromaSize = (u64)chroma_stride * (((height / 2) + 3) / 4);
            break;
        case JPEGENC_YUV420_SEMIPLANAR_101010:
            lumaSize = luma_stride * height;
            chromaSize = chroma_stride * height / 2;
            break;
        case JPEGENC_YUV422_888:
            lumaSize = (u64)luma_stride * height;
            chromaSize = (u64)chroma_stride * height;
            break;
        case JPEGENC_YUV420_8BIT_TILE_8_8:
            lumaSize = (u64)luma_stride * ((height + 7) / 8);
            chromaSize = (u64)chroma_stride * (((height / 2) + 3) / 4);
            break;
        case JPEGENC_YUV420_10BIT_TILE_8_8:
            lumaSize = (u64)luma_stride * ((height + 7) / 8);
            chromaSize = (u64)chroma_stride * (((height / 2) + 3) / 4);
            break;
        case JPEGENC_YUV420_FBC64:
        case JPEGENC_YUV420_UV_8BIT_TILE_128_2:
        case JPEGENC_YUV420_UV_10BIT_TILE_128_2:
            lumaSize = luma_stride * ((height + 1) / 2);
            chromaSize = chroma_stride * (((height / 2) + 1) / 2);
            break;
        default:
            av_log(NULL, AV_LOG_INFO, "not support this format\n");
            chromaSize = lumaSize = 0;
            break;
    }

    pictureSize = lumaSize + chromaSize;
    if (luma_size != NULL) *luma_size = lumaSize;
    if (chroma_size != NULL) *chroma_size = chromaSize;
    if (picture_size != NULL) *picture_size = pictureSize;
}

static JpegEncRet es_jenc_config_codec(AVCodecContext *avctx) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    EsJpegEncParams *jpeg_params = &(jpeg_enc_ctx->jpeg_enc_params);
    JpegEncCfg *jpeg_config = &(jpeg_enc_ctx->enc_config);
    int counter;

    memset(jpeg_config, 0, sizeof(JpegEncCfg));

    jpeg_config->enableRoimap = jpeg_enc_ctx->roi_enable;

    /* lossless mode */
    jpeg_config->losslessEn = 0;
    jpeg_config->predictMode = jpeg_params->predictMode;
    jpeg_config->ptransValue = jpeg_params->ptransValue;

    jpeg_config->inputWidth = (jpeg_params->lumWidthSrc + 15) & (~15); /* API limitation */
    if (jpeg_config->inputWidth != (u32)jpeg_params->lumWidthSrc) {
        av_log(avctx, AV_LOG_WARNING, "Warning: Input width must be multiple of 16!\n");
    }
    jpeg_config->inputWidth = jpeg_params->lumWidthSrc;
    jpeg_config->inputHeight = jpeg_params->lumHeightSrc;

    jpeg_config->xOffset = jpeg_params->horOffsetSrc;
    jpeg_config->yOffset = jpeg_params->verOffsetSrc;

    switch (jpeg_params->rotation) {
        case 90:
            jpeg_config->rotation = JPEGENC_ROTATE_90R;
            break;
        case 180:
            jpeg_config->rotation = JPEGENC_ROTATE_180;
            break;
        case 270:
            jpeg_config->rotation = JPEGENC_ROTATE_90L;
            break;
        case 0:
        default:
            jpeg_config->rotation = JPEGENC_ROTATE_0;
            break;
    }
    if ((jpeg_config->rotation != JPEGENC_ROTATE_0) && (jpeg_config->rotation != JPEGENC_ROTATE_180)) {
        /* full */
        jpeg_config->codingWidth = jpeg_params->height;
        jpeg_config->codingHeight = jpeg_params->width;
        jpeg_config->xDensity = jpeg_params->ydensity;
        jpeg_config->yDensity = jpeg_params->xdensity;
    } else {
        /* full */
        jpeg_config->codingWidth = jpeg_params->width;
        jpeg_config->codingHeight = jpeg_params->height;
        jpeg_config->xDensity = jpeg_params->xdensity;
        jpeg_config->yDensity = jpeg_params->ydensity;
    }

    if (jpeg_params->partialCoding && (jpeg_config->rotation != JPEGENC_ROTATE_0)) {
        av_log(avctx, AV_LOG_ERROR, "slice mode(partial coding) not support rotation\n");
        return JPEGENC_INVALID_ARGUMENT;
    }

    jpeg_config->mirror = jpeg_params->mirror;

    if (jpeg_params->qLevel == USER_DEFINED_QTABLE) {
        jpeg_config->qTableLuma = jpeg_enc_ctx->qtable_luma;      // qTableLuma;
        jpeg_config->qTableChroma = jpeg_enc_ctx->qtable_chroma;  // qTableChroma;
    } else {
        jpeg_config->qLevel = jpeg_params->qLevel;
    }

    jpeg_config->restartInterval = jpeg_params->restartInterval;
    jpeg_config->codingType = (JpegEncCodingType)jpeg_params->partialCoding;
    jpeg_config->frameType = (JpegEncFrameType)jpeg_params->frameType;
    jpeg_config->unitsType = (JpegEncAppUnitsType)jpeg_params->unitsType;
    jpeg_config->markerType = (JpegEncTableMarkerType)jpeg_params->markerType;
    jpeg_config->colorConversion.type = (JpegEncColorConversionType)jpeg_params->colorConversion;
    if (jpeg_config->colorConversion.type == JPEGENC_RGBTOYUV_USER_DEFINED) {
        /* User defined RGB to YCbCr conversion coefficients, scaled by 16-bits */
        jpeg_config->colorConversion.coeffA = 20000;
        jpeg_config->colorConversion.coeffB = 44000;
        jpeg_config->colorConversion.coeffC = 5000;
        jpeg_config->colorConversion.coeffE = 35000;
        jpeg_config->colorConversion.coeffF = 38000;
        jpeg_config->colorConversion.coeffG = 35000;
        jpeg_config->colorConversion.coeffH = 38000;
        jpeg_config->colorConversion.LumaOffset = 0;
    }
    jpeg_config->codingMode = (JpegEncCodingMode)jpeg_params->codingMode;

    /* low latency */
    jpeg_config->inputLineBufEn = (jpeg_params->inputLineBufMode > 0) ? 1 : 0;
    jpeg_config->inputLineBufLoopBackEn =
        (jpeg_params->inputLineBufMode == 1 || jpeg_params->inputLineBufMode == 2) ? 1 : 0;
    jpeg_config->inputLineBufDepth = jpeg_params->inputLineBufDepth;
    jpeg_config->amountPerLoopBack = jpeg_params->amountPerLoopBack;
    jpeg_config->inputLineBufHwModeEn =
        (jpeg_params->inputLineBufMode == 2 || jpeg_params->inputLineBufMode == 4) ? 1 : 0;
    jpeg_config->inputLineBufCbFunc = VCEncInputLineBufDone;
    jpeg_config->inputLineBufCbData = &jpeg_enc_ctx->lineBufCfg;
    jpeg_config->hashType = jpeg_params->hashtype;

    /* flexa sbi */
    jpeg_config->sbi_id_0 = 0;
    jpeg_config->sbi_id_1 = 1;
    jpeg_config->sbi_id_2 = 2;

    /*stream multi-segment*/
    jpeg_config->streamMultiSegmentMode = jpeg_params->streamMultiSegmentMode;
    jpeg_config->streamMultiSegmentAmount = jpeg_params->streamMultiSegmentAmount;
    jpeg_config->streamMultiSegCbFunc = NULL;
    jpeg_config->streamMultiSegCbData = NULL;

    /* constant chroma control */
    jpeg_config->constChromaEn = jpeg_params->constChromaEn;
    jpeg_config->constCb = jpeg_params->constCb;
    jpeg_config->constCr = jpeg_params->constCr;

    /* jpeg rate_ctrl*/
    jpeg_config->targetBitPerSecond = jpeg_params->bitPerSecond;
    jpeg_config->frameRateNum = 1;
    jpeg_config->frameRateDenom = 1;

    // framerate valid only when RC enabled
    if (jpeg_params->bitPerSecond) {
        jpeg_config->frameRateNum = jpeg_params->frameRateNum;
        jpeg_config->frameRateDenom = jpeg_params->frameRateDenom;
    }
    jpeg_config->qpmin = jpeg_params->qpmin;
    jpeg_config->qpmax = jpeg_params->qpmax;
    jpeg_config->fixedQP = jpeg_params->fixedQP;
    jpeg_config->rcMode = jpeg_params->rcMode;
    jpeg_config->picQpDeltaMax = jpeg_params->picQpDeltaMax;
    jpeg_config->picQpDeltaMin = jpeg_params->picQpDeltaMin;

    /*stride*/
    jpeg_config->exp_of_input_alignment = jpeg_params->exp_of_input_alignment;

    /* overlay control */
    for (counter = 0; counter < MAX_OVERLAY_NUM; counter++) {
        jpeg_config->olEnable[counter] = (jpeg_params->overlayEnables >> counter) & 1;
        jpeg_config->olFormat[counter] = jpeg_params->olFormat[counter];
        jpeg_config->olAlpha[counter] = jpeg_params->olAlpha[counter];
        jpeg_config->olWidth[counter] = jpeg_params->olWidth[counter];
        jpeg_config->olCropWidth[counter] = jpeg_params->olCropWidth[counter];
        jpeg_config->olHeight[counter] = jpeg_params->olHeight[counter];
        jpeg_config->olCropHeight[counter] = jpeg_params->olCropHeight[counter];
        jpeg_config->olXoffset[counter] = jpeg_params->olXoffset[counter];
        jpeg_config->olCropXoffset[counter] = jpeg_params->olCropXoffset[counter];
        jpeg_config->olYoffset[counter] = jpeg_params->olYoffset[counter];
        jpeg_config->olCropYoffset[counter] = jpeg_params->olCropYoffset[counter];
        jpeg_config->olYStride[counter] = jpeg_params->olYStride[counter];
        jpeg_config->olUVStride[counter] = jpeg_params->olUVStride[counter];
        jpeg_config->olBitmapY[counter] = jpeg_params->olBitmapY[counter];
        jpeg_config->olBitmapU[counter] = jpeg_params->olBitmapU[counter];
        jpeg_config->olBitmapV[counter] = jpeg_params->olBitmapV[counter];
        jpeg_config->olSuperTile[counter] = jpeg_params->olSuperTile[counter];
        jpeg_config->olScaleWidth[counter] = jpeg_params->olScaleWidth[counter];
        jpeg_config->olScaleHeight[counter] = jpeg_params->olScaleHeight[counter];
    }

    /* mosaic controls */
    for (counter = 0; counter < MAX_MOSAIC_NUM; counter++) {
        jpeg_config->mosEnable[counter] = (jpeg_params->mosaicEnables >> counter) & 1;
        jpeg_config->mosWidth[counter] = jpeg_params->mosWidth[counter];
        jpeg_config->mosHeight[counter] = jpeg_params->mosHeight[counter];
        jpeg_config->mosXoffset[counter] = jpeg_params->mosXoffset[counter];
        jpeg_config->mosYoffset[counter] = jpeg_params->mosYoffset[counter];
    }

    /* SRAM power down mode disable  */
    jpeg_config->sramPowerdownDisable = jpeg_params->sramPowerdownDisable;

    jpeg_config->AXIAlignment = jpeg_params->AXIAlignment;
    jpeg_config->irqTypeMask = jpeg_params->irqTypeMask;
    jpeg_config->burstMaxLength = jpeg_params->burstMaxLength;

    jpeg_config->comLength = jpeg_params->comLength;
    return JPEGENC_OK;
}

void es_jpeg_encode_report(AVCodecContext *avctx) {
    UNUSED(avctx);
    // TODO: referece vsv_encode_report();
}

/*------------------------------------------------------------------------------
    es_jenc_init_input_line_buffer
    -get line buffer params for IRQ handle
    -get address of input line buffer
------------------------------------------------------------------------------*/
static JpegEncRet es_jenc_init_input_line_buffer(AVCodecContext *avctx) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    JpegEncIn *encIn = (JpegEncIn *)&(jpeg_enc_ctx->enc_input);
    EWLLinearMem_t *input_buf_mem = &jpeg_enc_ctx->input_buf_mem;
    JpegEncInst inst = jpeg_enc_ctx->enc_inst;
    JpegEncCfg *encCfg = (JpegEncCfg *)&jpeg_enc_ctx->enc_config;
    inputLineBufferCfg *lineBufCfg = (inputLineBufferCfg *)&(jpeg_enc_ctx->lineBufCfg);
    u32 luma_stride, chroma_stride;
    u64 luma_size = 0, chroma_size = 0;

    JpegEncGetAlignedStride(
        encCfg->inputWidth, encCfg->frameType, &luma_stride, &chroma_stride, 1 << encCfg->exp_of_input_alignment);
    memset(lineBufCfg, 0, sizeof(inputLineBufferCfg));

    JpegGetLumaSize(jpeg_enc_ctx->enc_inst, &luma_size, NULL);
    JpegGetChromaSize(jpeg_enc_ctx->enc_inst, &chroma_size, NULL);

    lineBufCfg->inst = (void *)inst;
    // lineBufCfg->asic   = &(((jpegInstance_s *)inst)->asic);
    lineBufCfg->wrCnt = 0;
    lineBufCfg->depth = encCfg->inputLineBufDepth;
    lineBufCfg->inputFormat = encCfg->frameType;
    lineBufCfg->lumaStride = luma_stride;
    lineBufCfg->chromaStride = chroma_stride;
    lineBufCfg->encWidth = encCfg->codingWidth;
    lineBufCfg->encHeight = encCfg->codingHeight;
    lineBufCfg->hwHandShake = encCfg->inputLineBufHwModeEn;
    lineBufCfg->loopBackEn = encCfg->inputLineBufLoopBackEn;
    lineBufCfg->amountPerLoopBack = encCfg->amountPerLoopBack;
    lineBufCfg->srcHeight = encCfg->codingType ? encCfg->restartInterval * 16 : encCfg->inputHeight;
    lineBufCfg->srcVerOffset = encCfg->codingType ? 0 : encCfg->yOffset;
    lineBufCfg->getMbLines = &JpegEncGetEncodedMbLines;
    lineBufCfg->setMbLines = &JpegEncSetInputMBLines;
    lineBufCfg->ctbSize = 16;
    lineBufCfg->lumSrc = (u8 *)input_buf_mem->virtualAddress;
    lineBufCfg->cbSrc = lineBufCfg->lumSrc + luma_size;
    lineBufCfg->crSrc = lineBufCfg->cbSrc + chroma_size / 2;
    lineBufCfg->initSegNum = 0;
    lineBufCfg->client_type = EWL_CLIENT_TYPE_JPEG_ENC;

    if (VCEncInitInputLineBuffer(lineBufCfg)) {
        return JPEGENC_ERROR;
    }

    /* loopback mode */
    if (lineBufCfg->loopBackEn && lineBufCfg->lumBuf.buf) {
        encIn->busLum = lineBufCfg->lumBuf.busAddress;
        encIn->busCb = lineBufCfg->cbBuf.busAddress;
        encIn->busCr = lineBufCfg->crBuf.busAddress;

        /* data in SRAM start from the line to be encoded*/
        if (encCfg->codingType == JPEGENC_WHOLE_FRAME) {
            encCfg->yOffset = 0;
        }
    }
    return JPEGENC_OK;
}

/*------------------------------------------------------------------------------

    es_jenc_set_input_line_buffer
    -setup inputLineBufferCfg
    -initialize line buffer

------------------------------------------------------------------------------*/
static JpegEncRet es_jenc_set_input_line_buffer(AVCodecContext *avctx) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    JpegEncIn *encIn = (JpegEncIn *)&(jpeg_enc_ctx->enc_input);
    JpegEncCfg *encCfg = (JpegEncCfg *)&jpeg_enc_ctx->enc_config;
    inputLineBufferCfg *lineBufCfg = (inputLineBufferCfg *)&(jpeg_enc_ctx->lineBufCfg);

    if (encCfg->codingType == JPEGENC_SLICED_FRAME) {
        i32 h = encCfg->codingHeight + encCfg->yOffset;
        i32 sliceRows = encCfg->restartInterval * 16;
        i32 rows = jpeg_enc_ctx->sliceIdx * sliceRows;
        if ((rows + sliceRows) <= h)
            lineBufCfg->encHeight = sliceRows;
        else
            lineBufCfg->encHeight = h % sliceRows;
    }
    lineBufCfg->wrCnt = 0;
    encIn->lineBufWrCnt = VCEncStartInputLineBuffer(lineBufCfg, HANTRO_FALSE);
    encIn->initSegNum = lineBufCfg->initSegNum;

    av_log(avctx,
           AV_LOG_INFO,
           "Low latency: depth:%d ,inputFormat:%d, lumaStride:%d, chromaStride:%d,encWidth:%d,encHeight:%d\n",
           lineBufCfg->depth,
           lineBufCfg->inputFormat,
           lineBufCfg->lumaStride,
           lineBufCfg->chromaStride,
           lineBufCfg->encWidth,
           lineBufCfg->encHeight);
    av_log(avctx,
           AV_LOG_INFO,
           "Low latency: hwHandShake:%d,loopBackEn:%d,amountPerLoopBack:%d,srcHeight:%d, srcVerOffset:%d,\n",
           lineBufCfg->hwHandShake,
           lineBufCfg->loopBackEn,
           lineBufCfg->amountPerLoopBack,
           lineBufCfg->srcHeight,
           lineBufCfg->srcVerOffset);
    av_log(avctx,
           AV_LOG_INFO,
           "Low latency: lineBufCfg->lumSrc:%p,lineBufCfg->cbSrc:%p, lineBufCfg->crSrc:%p\n",
           lineBufCfg->lumSrc,
           lineBufCfg->cbSrc,
           lineBufCfg->crSrc);
    av_log(
        avctx, AV_LOG_INFO, "encIn->lineBufWrCnt:%d, encIn->initSegNum:%d\n", encIn->lineBufWrCnt, encIn->initSegNum);

    return JPEGENC_OK;
}

static int ReadQTable(char *qTableFileName, u8 *qTableLuma, u8 *qTableChroma) {
    FILE *fp = NULL;
    u8 *qTable;
    int line[8];
    int i, j, num;

    fp = fopen(qTableFileName, "rt");
    if (fp == NULL) return -1;

    qTable = qTableLuma;
    for (i = 0; i < 16; i++) {
        num = fscanf(fp,
                     "%d %d %d %d %d %d %d %d\n",
                     line,
                     line + 1,
                     line + 2,
                     line + 3,
                     line + 4,
                     line + 5,
                     line + 6,
                     line + 7);
        if (num != 8) {
            fclose(fp);
            return -1;
        }
        for (j = 0; j < 8; j++) {
            if (line[j] > 255) {
                fclose(fp);
                av_log(NULL, AV_LOG_ERROR, "Invalid Quant Table value %d at %d.\n", line[j], i * 8 + j);
                return -1;
            }
            qTable[j] = (u8)line[j];
        }

        if (i == 7)
            qTable = qTableChroma;
        else
            qTable += 8;
    }
    fclose(fp);
    return 0;
}

static JpegEncRet es_jenc_set_quant_table(AVCodecContext *avctx) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    EsJpegEncParams *params = &(jpeg_enc_ctx->jpeg_enc_params);
    EsJpegEncParams *option_params = &(jpeg_enc_ctx->jpeg_option_params);
    /* An example of user defined quantization table */
    u8 *qTableLuma = jpeg_enc_ctx->qtable_luma;
    u8 *qTableChroma = jpeg_enc_ctx->qtable_chroma;

    params->qLevel = option_params->qLevel;
    if (params->qLevel == USER_DEFINED_QTABLE && (0 != strcmp(option_params->qTablePath, ""))) {
        // read qtable
        av_log(avctx, AV_LOG_INFO, "qLevel %u, user qtable: %s\n", params->qLevel, option_params->qTablePath);
        if (0 != ReadQTable(option_params->qTablePath, qTableLuma, qTableChroma)) {
            av_log(avctx, AV_LOG_INFO, "Failed to open qtable from file %s\n. ", params->qTablePath);
            return JPEGENC_ERROR;
        }
    }

    return JPEGENC_OK;
}

static JpegEncRet es_jenc_release_codec(AVCodecContext *avctx) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    JpegEncInst enc_inst = jpeg_enc_ctx->enc_inst;
    JpegEncRet ret_value = JPEGENC_OK;

    av_log(avctx, AV_LOG_INFO, "es_jenc_release_codec()\n");

    ret_value = JpegEncRelease(enc_inst);
    if (ret_value != JPEGENC_OK) {
        av_log(
            avctx, AV_LOG_ERROR, "JpegEncRelease failed\n: ret_value = %d, enc_inst = %p.\n", (int)ret_value, enc_inst);
    }

    av_log(avctx, AV_LOG_DEBUG, "JpegEncRelease: ret_value = %d, enc_inst = %p.\n", (int)ret_value, enc_inst);

    return ret_value;
}

static JpegEncRet es_jenc_init_thumbnail(AVCodecContext *avctx) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    EsJpegEncParams *option_params = &(jpeg_enc_ctx->jpeg_option_params);
    JpegEncThumb *jpeg_thumb = &(jpeg_enc_ctx->jpeg_thumb);
    size_t size;
    FILE *fThumb;

    if (option_params->thumbnailFormat == 0 || option_params->inputThumb == NULL) {
        av_log(avctx, AV_LOG_INFO, "No thumbnail picture input\n");
        jpeg_enc_ctx->thumbnail_enable = false;
        return JPEGENC_OK;
    }
    if (option_params->widthThumb > 0xFF || option_params->heightThumb > 0xFF) {
        av_log(avctx, AV_LOG_ERROR, "thumbnail picture width/heigh invalid\n");
        jpeg_enc_ctx->thumbnail_enable = false;
        return JPEGENC_ERROR;
    }

    memset(jpeg_thumb, 0, sizeof(JpegEncThumb));
    // open thumbnail file:
    fThumb = fopen(option_params->inputThumb, "rb");
    if (fThumb == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Unable to open Thumbnail file: %s\n", option_params->inputThumb);
        return JPEGENC_ERROR;
    }
    jpeg_thumb->width = (u8)option_params->widthThumb;
    jpeg_thumb->height = (u8)option_params->heightThumb;

    switch (option_params->thumbnailFormat) {
        case 1:
            fseek(fThumb, 0, SEEK_END);
            jpeg_thumb->dataLength = ftell(fThumb);
            jpeg_thumb->format = JPEGENC_THUMB_JPEG;
            fseek(fThumb, 0, SEEK_SET);
            break;
        case 2:
            jpeg_thumb->dataLength = 3 * 256 + jpeg_thumb->width * jpeg_thumb->height;
            jpeg_thumb->format = JPEGENC_THUMB_PALETTE_RGB8;
            break;
        case 3:
            jpeg_thumb->dataLength = jpeg_thumb->width * jpeg_thumb->height * 3;
            jpeg_thumb->format = JPEGENC_THUMB_RGB24;
            break;
    }

    jpeg_enc_ctx->thumb_data = (u8 *)malloc(jpeg_thumb->dataLength);
    size = fread(jpeg_enc_ctx->thumb_data, 1, jpeg_thumb->dataLength, fThumb);
    if (size = 0) {
        av_log(avctx, AV_LOG_ERROR, "Unable to read Thumbnail file\n");
        jpeg_enc_ctx->thumbnail_enable = false;
        return JPEGENC_ERROR;
    }
    fclose(fThumb);
    jpeg_thumb->data = jpeg_enc_ctx->thumb_data;
    jpeg_enc_ctx->thumbnail_enable = true;

    av_log(avctx,
           AV_LOG_INFO,
           "input thumbnail:%s, width:%d  height:%d, data:%p, data_len:%u\n",
           option_params->inputThumb,
           jpeg_thumb->width,
           jpeg_thumb->height,
           jpeg_enc_ctx->thumb_data,
           jpeg_thumb->dataLength);
    return JPEGENC_OK;
}

static JpegEncRet es_jenc_set_thumbnail(AVCodecContext *avctx) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    JpegEncRet ret_value = JPEGENC_OK;
    JpegEncThumb *jpeg_thumb = &(jpeg_enc_ctx->jpeg_thumb);

    if (jpeg_enc_ctx->thumbnail_enable) {
        ret_value = JpegEncSetThumbnail(jpeg_enc_ctx->enc_inst, jpeg_thumb);
        if (ret_value != JPEGENC_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to set thumbnail. Error code: %8i\n", ret_value);
        }
    }

    return ret_value;
}

static int es_jenc_encode_got_mem_fd(const AVFrame *frame, int64_t *mem_fd) {
    AVFrameSideData *sd = NULL;
    if (!frame) return -1;
    if (!frame->nb_side_data) return -1;

    // mem fd
    sd = av_frame_get_side_data(frame, SIDE_DATA_TYPE_MEM_FRAME_FD);
    if (sd) {
        *mem_fd = *((int64_t *)sd->data);
        av_log(NULL, AV_LOG_INFO, "got mem_fd: %lx\n", *mem_fd);
        return 0;
    }

    return -1;
}

static int es_jenc_send_fd_by_avpacket(AVPacket *pkt, int64_t mem_fd) {
    uint8_t *buf;
    int64_t *fd;
    if (!pkt) {
        av_log(NULL, AV_LOG_ERROR, "es_jenc_send_fd_by_avpacket, invalid pointers\n");
        return -1;
    }

    // dma fd
    buf = av_packet_new_side_data(pkt, SIDE_DATA_TYPE_MEM_FRAME_FD_RELEASE, sizeof(mem_fd));
    fd = (int64_t *)buf;
    *fd = mem_fd;

    av_log(NULL, AV_LOG_INFO, "encoded one frame, release pkt with dma fd[%lx]\n", mem_fd);

    return 0;
}

static JpegEncRet es_jenc_fill_input_buffer(AVCodecContext *avctx, const AVFrame *pict) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    JpegEncIn *enc_in = (JpegEncIn *)&(jpeg_enc_ctx->enc_input);
    JpegEncCfg *encCfg = (JpegEncCfg *)&jpeg_enc_ctx->enc_config;
    EWLLinearMem_t *yuv_frame_mem = (EWLLinearMem_t *)&(jpeg_enc_ctx->input_buf_mem);
    u64 luma_size = 0, chroma_size = 0;
    u32 luma_stride, chroma_stride;
    uint8_t *data[3];
    int linesize[3];
    int height = 0;
    int i = 0;

    JpegGetLumaSize(jpeg_enc_ctx->enc_inst, &luma_size, NULL);
    JpegGetChromaSize(jpeg_enc_ctx->enc_inst, &chroma_size, NULL);
    JpegEncGetAlignedStride(
        encCfg->inputWidth, encCfg->frameType, &luma_stride, &chroma_stride, 1 << encCfg->exp_of_input_alignment);

    av_log(avctx,
           AV_LOG_DEBUG,
           "input picture:w: %d, h: %d, format: %d, linesize: %d, %d, %d, data: %p, %p, %p\n",
           pict->width,
           pict->height,
           pict->format,
           pict->linesize[0],
           pict->linesize[1],
           pict->linesize[2],
           pict->data[0],
           pict->data[1],
           pict->data[2]);

    av_log(avctx,
           AV_LOG_DEBUG,
           "luma_stride: %d, chroma_stride: %d, lumaSize: %ld, chromaSize: %ld\n",
           luma_stride,
           chroma_stride,
           luma_size,
           chroma_size);

    switch (encCfg->frameType) {
        case JPEGENC_YUV420_PLANAR:
        case JPEGENC_YUV420_I010:
            data[0] = (uint8_t *)yuv_frame_mem->virtualAddress;
            data[1] = data[0] + luma_size;
            data[2] = data[1] + chroma_size / 2;
            linesize[0] = luma_stride;
            linesize[1] = chroma_stride;
            linesize[2] = chroma_stride;
            enc_in->busLum = yuv_frame_mem->busAddress;
            enc_in->busCb = enc_in->busLum + luma_size;
            enc_in->busCr = enc_in->busCb + chroma_size / 2;
            break;
        case JPEGENC_YUV420_SEMIPLANAR:
        case JPEGENC_YUV420_SEMIPLANAR_VU:
        case JPEGENC_YUV420_MS_P010:
            data[0] = (uint8_t *)yuv_frame_mem->virtualAddress;
            data[1] = data[0] + luma_size;
            data[2] = NULL;
            linesize[0] = luma_stride;
            linesize[1] = chroma_stride;
            linesize[2] = 0;
            enc_in->busLum = yuv_frame_mem->busAddress;
            enc_in->busCb = enc_in->busLum + luma_size;
            enc_in->busCr = 0;
            break;
        case JPEGENC_YUV422_INTERLEAVED_YUYV:
        case JPEGENC_YUV422_INTERLEAVED_UYVY:

            data[0] = (uint8_t *)yuv_frame_mem->virtualAddress;
            data[1] = NULL;
            data[2] = NULL;
            linesize[0] = luma_stride;
            linesize[1] = 0;
            linesize[2] = 0;
            enc_in->busLum = yuv_frame_mem->busAddress;
            enc_in->busCb = 0;
            enc_in->busCr = 0;
            break;
        default:
            av_log(avctx, AV_LOG_ERROR, "not support format: %d\n", encCfg->frameType);
            return JPEGENC_ERROR;
    }

    for (i = 0; i < FF_ARRAY_ELEMS(data) && data[i]; i++) {
        height = !i ? pict->height : pict->height >> 1;
        if (linesize[i] == pict->linesize[i]) {
            ff_es_codec_memcpy_block(pict->data[i], data[i], pict->linesize[i] * height);
        } else {
            ff_es_codec_memcpy_by_line(pict->data[i], data[i], pict->linesize[i], linesize[i], height);
        }
    }

    if (EWLSyncMemData(&jpeg_enc_ctx->input_buf_mem, 0, luma_size + chroma_size, HOST_TO_DEVICE) != EWL_OK) {
        av_log(avctx, AV_LOG_ERROR, "EWLSyncMemData() failed \n");
        return JPEGENC_ERROR;
    }

    return JPEGENC_OK;
}

static JpegEncRet es_jpeg_encode_encode(AVCodecContext *avctx, AVPacket *avpkt, const AVFrame *pict) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    JpegEncRet ret_value = JPEGENC_OK;
    JpegEncIn *enc_in = (JpegEncIn *)&(jpeg_enc_ctx->enc_input);
    JpegEncOut *enc_out = (JpegEncOut *)&jpeg_enc_ctx->enc_output;
    JpegEncCfg *encCfg = (JpegEncCfg *)&jpeg_enc_ctx->enc_config;
    u64 luma_size = 0, chroma_size = 0;
    u64 input_buf_size = 0;
    u32 luma_stride, chroma_stride;
    // output:
    u32 buf0Len;
    u32 writeSize;
    int new_size;

    // input fd
    int64_t mem_fd = 0;
    unsigned long vir_addr = 0;

#ifdef SUPPORT_DMA_HEAP
    const void *ewl_inst = JpegEncGetEwl(jpeg_enc_ctx->enc_inst);
#endif

    memset(enc_in, 0, sizeof(JpegEncIn));
    // output buffer
    for (int i = 0; i < 1; i++) {
        enc_in->pOutBuf[i] = (u8 *)jpeg_enc_ctx->outbufMem[0].virtualAddress;
        enc_in->busOutBuf[i] = jpeg_enc_ctx->outbufMem[0].busAddress;
        enc_in->outBufSize[i] = jpeg_enc_ctx->outbufMem[0].size;
        av_log(
            avctx, AV_LOG_DEBUG, "encIn.pOutBuf[%d] Vaddr:%p, size:%d\n", i, enc_in->pOutBuf[i], enc_in->outBufSize[i]);
    }

    // input buffer
    JpegGetLumaSize(jpeg_enc_ctx->enc_inst, &luma_size, NULL);
    JpegGetChromaSize(jpeg_enc_ctx->enc_inst, &chroma_size, NULL);
    JpegEncGetAlignedStride(
        encCfg->inputWidth, encCfg->frameType, &luma_stride, &chroma_stride, 1 << encCfg->exp_of_input_alignment);
    input_buf_size = luma_size + chroma_size;
    if (!es_jenc_encode_got_mem_fd(pict, &mem_fd)) {
        // get dma buffer virtual addr
#ifdef SUPPORT_DMA_HEAP
        EWLGetIovaByFd(ewl_inst, mem_fd, &vir_addr);
#else
        vir_addr = mem_fd;
#endif
        if (luma_stride != pict->linesize[0]) {
            av_log(avctx,
                   AV_LOG_ERROR,
                   "for share buffer, jenc alignment[%d] !=  linesize[%d] failed\n",
                   luma_stride,
                   pict->linesize[0]);
            return JPEGENC_ERROR;
        }
        enc_in->busLum = (ptr_t)vir_addr;
        enc_in->busCb = enc_in->busLum + luma_size;
        enc_in->busCr = enc_in->busCb + chroma_size / 2;
    } else {
        ret_value = es_jenc_fill_input_buffer(avctx, pict);
        if (ret_value != JPEGENC_OK) {
            av_log(avctx, AV_LOG_ERROR, "fill input buffer mem failed, ret:%d\n", ret_value);
            return JPEGENC_ERROR;
        }
    }

    av_log(avctx, AV_LOG_DEBUG, "pEncIn->busLum = %p, lumaSize:%ld\n", (void *)enc_in->busLum, luma_size);
    av_log(avctx, AV_LOG_DEBUG, "pEncIn->busCb = %p,chromaSize:%ld\n", (void *)enc_in->busCb, chroma_size);
    av_log(avctx, AV_LOG_DEBUG, "pEncIn->busCr = %p\n", (void *)enc_in->busCr);

    enc_in->frameHeader = 1;
    enc_in->lineBufWrCnt = 0;
    enc_in->dec400Enable = 1;

    if (jpeg_enc_ctx->roi_enable) {
        enc_in->filter = jpeg_enc_ctx->enc_config.filter;
        enc_in->busRoiMap = jpeg_enc_ctx->roimapMem.busAddress;
    } else {
        enc_in->filter = NULL;
        enc_in->busRoiMap = 0;
    }

    /* Set thumbnail every frame*/
    ret_value = es_jenc_set_thumbnail(avctx);
    if (ret_value != 0) {
        av_log(avctx, AV_LOG_ERROR, "es_jpeg_set_enc_thumbnail error. ret_value = %d\n", ret_value);
        return ret_value;
    }

    /* Multi Segment Mode */
    if (jpeg_enc_ctx->stream_multi_segment_mode != 0) {
        av_log(avctx, AV_LOG_DEBUG, "stream multi segment mode enable\n");
        // VS don't support stream multi-segment
        // InitStreamSegmentCrl(&streamSegCtl, &cmdl, fout, &encIn);
    }

    /* Low latency */
    if (jpeg_enc_ctx->input_line_buf_mode) {
        ret_value = es_jenc_set_input_line_buffer(avctx);
        if (ret_value != JPEGENC_OK) {
            av_log(avctx, AV_LOG_ERROR, "es_jenc_set_input_line_buffer() failed ,ret:%d\n", ret_value);
            return ret_value;
        }
    }
reenc:
    gettimeofday(&jpeg_enc_ctx->time_frame_start, 0);
    ret_value = JpegEncEncode(jpeg_enc_ctx->enc_inst, enc_in, enc_out);
    gettimeofday(&jpeg_enc_ctx->time_frame_end, 0);
    switch (ret_value) {
        case JPEGENC_FRAME_READY:
        case JPEGENC_RESTART_INTERVAL:
            if (ret_value == JPEGENC_RESTART_INTERVAL) {
                av_log(avctx, AV_LOG_INFO, "partial frame encoded succsess\n");
            }

            av_log(avctx, AV_LOG_DEBUG, "enc_output->streamSize = %d\n", jpeg_enc_ctx->out_stream_size);

            if (jpeg_enc_ctx->input_line_buf_mode) {
                VCEncUpdateInitSegNum(&jpeg_enc_ctx->lineBufCfg);
            }

            buf0Len = jpeg_enc_ctx->outbufMem[0].size;
            writeSize = MIN(enc_out->jfifSize, buf0Len - enc_out->invalidBytesInBuf0Tail);

#if 0
            {
                // In some cases the EOI marker (0xff 0xd9) will be missing.
                // So we check and add it when the scenario occurs.
                u8 * head = (u8 *) jpeg_enc_ctx->outbufMem[0].virtualAddress;
                u8 * tail = head + writeSize - 2;
                if ((tail[0] != 0xff) && (tail[1] != 0xd9)) {
                    tail[2] = 0xff;
                    tail[3] = 0xd9;
                    writeSize += 2;
                    av_log(avctx, AV_LOG_WARNING, "EOI marker missing and added.\n");
                }
            }
#endif

            if (av_new_packet(avpkt, writeSize)) {
                return -1;
            }

            memcpy(avpkt->data, (u8 *)jpeg_enc_ctx->outbufMem[0].virtualAddress, writeSize);
            avpkt->size = writeSize;
            avpkt->pts = pict->pts;
            break;
        case JPEGENC_OUTPUT_BUFFER_OVERFLOW:
            av_log(avctx, AV_LOG_WARNING, " out put buffer overflow\n");

            // choose decompress rate
            jpeg_enc_ctx->compress_rate--;
            if (jpeg_enc_ctx->compress_rate < 2) {
                jpeg_enc_ctx->compress_rate = 1;
            }
            new_size = input_buf_size / jpeg_enc_ctx->compress_rate;
            if (jpeg_enc_ctx->outbufMem[0].size >= new_size) {
                // VS testbench defalut value
                new_size = encCfg->inputWidth * encCfg->inputHeight * 2;
            }
            av_log(avctx, AV_LOG_INFO, " out put buffer realloc %d->%d\n", jpeg_enc_ctx->outbufMem[0].size, new_size);
            if (es_jenc_realloc_output_buffer(avctx, new_size) != 0) {
                av_log(avctx, AV_LOG_ERROR, "out put buffer realloc failed\n");
                return JPEGENC_ERROR;
            }
            enc_in->pOutBuf[0] = (u8 *)jpeg_enc_ctx->outbufMem[0].virtualAddress;
            enc_in->busOutBuf[0] = jpeg_enc_ctx->outbufMem[0].busAddress;
            enc_in->outBufSize[0] = jpeg_enc_ctx->outbufMem[0].size;
            goto reenc;
            break;
        default:
            av_log(avctx, AV_LOG_ERROR, " encoded failed ret = %d\n", ret_value);
            break;
    }

    if (mem_fd != 0) {
#ifdef SUPPORT_DMA_HEAP
        EWLPutIovaByFd(ewl_inst, mem_fd);
#endif
        es_jenc_send_fd_by_avpacket(avpkt, mem_fd);
    }

    return ret_value;
}

static av_cold JpegEncRet es_jpeg_encode_open(AVCodecContext *avctx) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    JpegEncRet ret_value = JPEGENC_ERROR;
    JpegEncApiVersion jpeg_api_ver;
    JpegEncBuild jpeg_api_build;
    i32 useVcmd = 1;

    if (jpeg_enc_ctx->encoder_is_open) {
        av_log(avctx, AV_LOG_INFO, "encoder is already open.\n");
        return 0;
    }

    if (jpeg_enc_ctx->input_line_buf_mode) {
        useVcmd = 0;
    }
    EWLAttach(NULL, 0, useVcmd);
    jpeg_api_ver = JpegEncGetApiVersion();
    jpeg_api_build = JpegEncGetBuild(0, NULL);
#ifdef FB_SYSLOG_ENABLE
    ENC_INFO_PRINT((const char *)&jpeg_enc_ctx->log_header,
                   "JPEG Encoder API version %d.%d.%d\n",
                   jpeg_api_ver.major,
                   jpeg_api_ver.minor,
                   jpeg_api_ver.clnum);
    ENC_INFO_PRINT((const char *)&jpeg_enc_ctx->log_header,
                   "JPEG Encoder HW ID:  0x%08x\t SW Build: %u.%u.%u\n\n",
                   jpeg_api_build.hwBuild,
                   jpeg_api_build.swBuild / 1000000,
                   (jpeg_api_build.swBuild / 1000) % 1000,
                   jpeg_api_build.swBuild % 1000);
#else
    UNUSED(jpeg_api_ver);
    UNUSED(jpeg_api_build);
#endif

    ret_value = es_jenc_init_codec(avctx);
    if (ret_value != JPEGENC_OK) {
        goto error_exit;
    }

    ret_value = es_jenc_alloc_codec_res(avctx);
    if (ret_value != JPEGENC_OK) {
        goto error_exit;
    }

    /* init low latency */
    if (jpeg_enc_ctx->input_line_buf_mode) {
        av_log(avctx, AV_LOG_INFO, "inputline buffer mode enable\n");
        ret_value = es_jenc_init_input_line_buffer(avctx);
        if (ret_value != JPEGENC_OK) {
            av_log(avctx, AV_LOG_ERROR, "es_jenc_init_input_line_buffer() failed ,ret:%d\n", ret_value);
            goto error_exit;
        }
    }

    /* init input roi*/
    if (jpeg_enc_ctx->roi_enable) {
        if (read_non_roi_fliter(avctx) != 0) {
            av_log(avctx, AV_LOG_ERROR, "read_non_roi_fliter() failed \n");
            goto error_exit;
        } else {
            // set roi
            read_roimap(avctx);
        }
    }

    ret_value = JpegEncSetPictureSize(jpeg_enc_ctx->enc_inst, &(jpeg_enc_ctx->enc_config));
    if (ret_value != JPEGENC_OK) {
        av_log(avctx, AV_LOG_ERROR, "JpegEncSetPictureSize() failed ,ret:%d\n", ret_value);
        goto error_exit;
    }
    jpeg_enc_ctx->encoder_is_open = 1;
    return JPEGENC_OK;

error_exit:
    es_jenc_free_codec_res(avctx);
    es_jenc_release_codec(avctx);
    return ret_value;
}

#ifndef CONFIG_ESW_FF_ENHANCEMENT
static int es_jpeg_encode_init_hwcontext(AVCodecContext *avctx) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;

    /* hw device & frame init */
    if (avctx->hw_frames_ctx) {
        AVHWFramesContext *hwframe;

        hwframe = (AVHWFramesContext *)avctx->hw_frames_ctx->data;
        jpeg_enc_ctx->hwdevice = (AVVSVDeviceContext *)((AVHWDeviceContext *)hwframe->device_ref->data)->hwctx;
        if (!jpeg_enc_ctx->hwdevice) {
            return AVERROR(ENOMEM);
        }
    } else if (avctx->hw_device_ctx) {
        av_log(avctx, AV_LOG_TRACE, "%s(%d) avctx->hw_device_ctx = %p\n", __FUNCTION__, __LINE__, avctx->hw_device_ctx);
        jpeg_enc_ctx->hwdevice = (AVVSVDeviceContext *)((AVHWDeviceContext *)avctx->hw_device_ctx->data)->hwctx;
        av_log(avctx,
               AV_LOG_TRACE,
               "%s(%d) jpeg_enc_ctx->hwdevice = %p\n",
               __FUNCTION__,
               __LINE__,
               jpeg_enc_ctx->hwdevice);
        if (!jpeg_enc_ctx->hwdevice) {
            return AVERROR(ENOMEM);
        }
    } else {
        return AVERROR(EINVAL);
    }

    // if (jpeg_enc_ctx->lookahead_depth) {
    //     jpeg_enc_ctx->hwdevice->lookahead = 1;
    //     nb_frames = 17 + jpeg_enc_ctx->lookahead_depth;
    // } else {
    //     nb_frames = 8 + 2;
    // }

    // if (jpeg_enc_ctx->hwdevice->nb_frames < nb_frames) {
    //     jpeg_enc_ctx->hwdevice->nb_frames = nb_frames;
    // }

    return 0;
}
#endif

av_cold int ff_es_jpeg_encode_init(AVCodecContext *avctx) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    EsJpegEncParams params;
    JpegEncRet ret_value = JPEGENC_OK;

    av_log(avctx, AV_LOG_TRACE, "%s %d\n", __FUNCTION__, __LINE__);

    /* If you add something that can fail above this av_frame_alloc(),
     * modify ff_es_jpeg_encode_close() accordingly. */
    jpeg_enc_ctx->frame = av_frame_alloc();
    if (!jpeg_enc_ctx->frame) {
        return AVERROR(ENOMEM);
    }

    // Create thread/mutex, use video encoder thread utils:
    ret_value = esenc_init(&jpeg_enc_ctx->tc, avctx, &ff_es_jpeg_encode_encode2);
    if (ret_value < 0) {
        ret_value = JPEGENC_ERROR;
        goto error_exit;
    }

    // init the jpeg encoder params, set to default value.
    ret_value = es_jenc_init_params(avctx);
    if (ret_value != JPEGENC_OK) {
        goto error_exit;
    }

    // get the codec's current params, and fix the params user set
    ret_value = es_jenc_get_params(avctx, &params);
    if (ret_value != JPEGENC_OK) {
        goto error_exit;
    }

    // set the user's params to codec
    ret_value = es_jenc_set_params(avctx, &params);
    if (ret_value != JPEGENC_OK) {
        goto error_exit;
    }

    // config the params to the codec
    ret_value = es_jenc_config_codec(avctx);
    if (ret_value != JPEGENC_OK) {
        goto error_exit;
    }

#ifndef CONFIG_ESW_FF_ENHANCEMENT
    //  hwcontext init
    ret_value = es_jpeg_encode_init_hwcontext(avctx);
    if (ret_value < 0) {
        goto error_exit;
    }
#endif

    // open the codec
    jpeg_enc_ctx->encoder_is_open = 0;
    ret_value = es_jpeg_encode_open(avctx);
    if (ret_value != JPEGENC_OK) {
        av_log(avctx, AV_LOG_ERROR, "es_jpeg_encode_open error. ret_value = %d\n", ret_value);
        goto error_exit;
    }
    return JPEGENC_OK;
error_exit:
    ff_es_jpeg_encode_close(avctx);
    return ret_value;
}

av_cold int ff_es_jpeg_encode_close(AVCodecContext *avctx) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;

    /* We check ctx->frame to know whether encode_init()
     * has been called and va_config/va_context initialized. */
    if (!jpeg_enc_ctx->frame) return 0;

    esenc_close(jpeg_enc_ctx->tc);

    es_jpeg_encode_report(avctx);

    if (jpeg_enc_ctx->encoder_is_open) {
        es_jenc_free_codec_res(avctx);
        es_jenc_release_codec(avctx);
        jpeg_enc_ctx->encoder_is_open = 0;
    }

    av_frame_free(&jpeg_enc_ctx->frame);
    if (avctx->extradata) {
        av_freep(&avctx->extradata);
    }

    return JPEGENC_OK;
}

int ff_es_jpeg_encode_send_frame(AVCodecContext *avctx, const AVFrame *frame) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    return esenc_send_frame(jpeg_enc_ctx->tc, frame);
}

int ff_es_jpeg_encode_receive_packet(AVCodecContext *avctx, AVPacket *avpkt) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    AVFrame *frame = jpeg_enc_ctx->frame;
    int err;

    err = ff_encode_get_frame(avctx, frame);
    if (err < 0 && err != AVERROR_EOF) {
        return err;
    }

    if (err == AVERROR_EOF) {
        av_log(avctx, AV_LOG_INFO, "send NULL frame ,eos frame\n");
        frame = NULL;
    }

#ifdef DUMP_ESJENC_INPUT
    if (frame) {
        av_log(avctx, AV_LOG_ERROR, "open  inputyuv.yuv\n");
        FILE *input_f = fopen("inputyuv.yuv", "wb+");
        if (input_f == NULL) av_log(avctx, AV_LOG_ERROR, "open  inputyuv.yuv failed\n");
        av_log(avctx, AV_LOG_ERROR, "frame->data[0,1,2]:[%p,%p,%p]\n", frame->data[0], frame->data[1], frame->data[2]);
        av_log(avctx,
               AV_LOG_ERROR,
               "frame->linesize[0,1,2]:[%d,%d,%d]\n",
               frame->linesize[0],
               frame->linesize[1],
               frame->linesize[2]);
        av_log(avctx, AV_LOG_ERROR, "frame w h:[%d,%d]\n", frame->width, frame->height);
        fwrite(frame->data[0], frame->width * frame->height * 3 / 2, 1, input_f);
        fflush(input_f);
        fclose(input_f);
    }
#endif

    err = esenc_send_frame(jpeg_enc_ctx->tc, frame);
    if (err != 0) {
        av_log(avctx, AV_LOG_ERROR, "esenc_send_frame failed res=:%d\n", err);
        return err;
    }
    return esenc_receive_packet(jpeg_enc_ctx->tc, avpkt);
}

static JpegEncRet es_jenc_side_data_parse(AVCodecContext *avctx, const AVFrame *frame) {
    EsJpegEncodeContext *jpeg_enc_ctx = (EsJpegEncodeContext *)avctx->priv_data;
    JpegEncThumb *jpeg_thumb = &(jpeg_enc_ctx->jpeg_thumb);
    AVFrameSideData *sd = NULL;
    SideDataThumbnail *sd_thumbnail = NULL;
    SideDataRoiArea *sd_roi_areas = NULL;
    SideDataJencNonRoiFilter *sd_non_roi_filter = NULL;
    if (!avctx) return JPEGENC_ERROR;
    if (!frame) return JPEGENC_OK;
    if (!frame->nb_side_data) return JPEGENC_OK;

    // got thumbnail side data
    sd = av_frame_get_side_data(frame, SIDE_DATA_TYPE_JENC_THUMBNAIL);
    if (sd) {
        sd_thumbnail = (SideDataThumbnail *)sd->data;
        av_log(avctx, AV_LOG_INFO, "received thumbnail sd, data_size = %d\n", sd_thumbnail->data_length);
    }

    // got roi from side data
    sd = av_frame_get_side_data(frame, SIDE_DATA_TYPE_JENC_ROI_AREA);
    if (sd) {
        sd_roi_areas = (SideDataRoiArea *)sd->data;
    }

    sd = av_frame_get_side_data(frame, SIDE_DATA_TYPE_JENC_NON_ROI_FILTER);
    if (sd) {
        sd_non_roi_filter = (SideDataJencNonRoiFilter *)sd->data;
        av_log(avctx, AV_LOG_INFO, "received non roi filter sd, level:%d\n", sd_non_roi_filter->non_roi_qp_level);
    }

    if (sd_thumbnail) {
        jpeg_thumb->format = sd_thumbnail->format;
        jpeg_thumb->width = sd_thumbnail->width;
        jpeg_thumb->height = sd_thumbnail->height;
        jpeg_thumb->data = sd_thumbnail->data;
        jpeg_thumb->dataLength = sd_thumbnail->data_length;
        jpeg_enc_ctx->thumbnail_enable = 1;
    }

    if (sd_roi_areas != NULL && sd_non_roi_filter != NULL) {
        // 1.alloc roi
        if (jpeg_enc_ctx->roi_buffer_alloced == FALSE) {
            es_jenc_alloc_roi_res(avctx);
            jpeg_enc_ctx->roi_buffer_alloced = TRUE;
        }
        // 2.set non roi filter
        dynamic_read_non_roi_filter(avctx, sd_non_roi_filter);
        // 3.set roi area
        dynamic_read_roimap(avctx, frame);
        jpeg_enc_ctx->roi_enable = 1;
    }

    return JPEGENC_OK;
}

int ff_es_jpeg_encode_encode2(AVCodecContext *avctx, AVPacket *avpkt, const AVFrame *frame, int *got_packet) {
    JpegEncRet ret_value = JPEGENC_OK;
    *got_packet = 0;

    ret_value = es_jenc_side_data_parse(avctx, frame);
    if (frame == NULL) {
        av_log(avctx, AV_LOG_INFO, "ff_es_jpeg_encode_encode2 frame is null\n");
        ret_value = AVERROR_EOF;
    } else {
        av_log(avctx,
               AV_LOG_DEBUG,
               "frame ref count %d for %p\n",
               av_buffer_get_ref_count(frame->buf[0]),
               frame->buf[0]->data);

        ret_value = es_jpeg_encode_encode(avctx, avpkt, frame);

        if (ret_value == JPEGENC_FRAME_READY || ret_value == JPEGENC_RESTART_INTERVAL) {
            *got_packet = 1;
        } else {
            /* need error process */
            ret_value = JPEGENC_ERROR;
        }
    }
    av_log(avctx, AV_LOG_DEBUG, "%s(%d) got_packet %d\n", __FUNCTION__, __LINE__, *got_packet);

    return (int)ret_value;
}

const FFCodec ff_jpeg_es_encoder = {
    .p.name = JPEG_ES_ENCODER_STR,
    .p.long_name = NULL_IF_CONFIG_SMALL("Eswin JPEG encoder, on VeriSilicon & GStreamer & FFmpeg"),
    .p.type = AVMEDIA_TYPE_VIDEO,
    .p.id = AV_CODEC_ID_MJPEG,
    .priv_data_size = sizeof(EsJpegEncodeContext),
    .init = ff_es_jpeg_encode_init,
    .close = ff_es_jpeg_encode_close,
    // .send_frame = &ff_es_jpeg_encode_send_frame,
    FF_CODEC_RECEIVE_PACKET_CB(ff_es_jpeg_encode_receive_packet),
    // FF_CODEC_ENCODE_CB(ff_es_jpeg_encode_encode2),
    .p.priv_class = &es_jpeg_encode_class,
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE,
    .defaults = es_jpeg_encode_defaults,
    .p.pix_fmts = es_jenc_support_pixfmts,
    .hw_configs = es_jpeg_encode_hw_configs,
    .p.wrapper_name = "es",
};
