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

#include <float.h>
#include <limits.h>

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
#include "hwconfig.h"
#include "internal.h"
#include "esenc_vid.h"
#include "es_common.h"
#include "hevcencapi_utils.h"
#include "esenc_vid_internal.h"
#include "esenc_vid_buffer.h"
#include "encode.h"

// config if encoding the cu info, only for testing.
// now we don't have good way to pass the cu info data to user
//#define DEBUG_CU_INFO

extern uint32_t getEWLMallocInoutSize(uint32_t alignment, uint32_t in_size);

int32_t vce_default_bitrate[] = {100000, 250000, 500000, 1000000, 3000000, 5000000, 10000000};

static void venc_print_roi(RoiParas *roi_paras) {
    if (!roi_paras) return;

    av_log(NULL, AV_LOG_INFO, "roi config cnt: %d\n", roi_paras->num_of_roi);
    for (int i = 0; i < roi_paras->num_of_roi; i++) {
        av_log(NULL,
               AV_LOG_INFO,
               "roi index: %d, enable: %d, absQp: %d, qp: %d, (%4d, %4d, %4d, %4d)\n",
               roi_paras->roi_attr[i].index,
               roi_paras->roi_attr[i].enable,
               roi_paras->roi_attr[i].is_absQp,
               roi_paras->roi_attr[i].qp,
               roi_paras->roi_attr[i].x,
               roi_paras->roi_attr[i].y,
               roi_paras->roi_attr[i].width,
               roi_paras->roi_attr[i].height);
    }
}

/* parse roi option*/
// format: "r0:0:0:0:0:0:0:0;r1:0:0:0:0:0:0:0"
static int venc_get_roi(char *str, RoiParas *roi_paras) {
    char *p = str;
    unsigned int cnt = 0;
    RoiAttr *roi_attr = NULL;

    if (!str || !roi_paras) return -1;

    memset(roi_paras, 0x00, sizeof(RoiParas));

    while ((p = strstr(p, "r")) != NULL) {
        roi_attr = &roi_paras->roi_attr[cnt];
        // index
        roi_attr->index = atoi(p + 1);

        // enable
        if ((p = strstr(p, ":")) == NULL) return -1;
        p++;
        roi_attr->enable = atoi(p);
        // absQp
        if ((p = strstr(p, ":")) == NULL) return -1;
        p++;
        roi_attr->is_absQp = atoi(p);
        // qp
        if ((p = strstr(p, ":")) == NULL) return -1;
        p++;
        roi_attr->qp = atoi(p);
        // left
        if ((p = strstr(p, ":")) == NULL) return -1;
        p++;
        roi_attr->x = atoi(p);
        // top
        if ((p = strstr(p, ":")) == NULL) return -1;
        p++;
        roi_attr->y = atoi(p);
        // right
        if ((p = strstr(p, ":")) == NULL) return -1;
        p++;
        roi_attr->width = atoi(p);
        // bottom
        if ((p = strstr(p, ":")) == NULL) return -1;
        p++;
        roi_attr->height = atoi(p);

        // num
        cnt++;
        if (cnt >= MAX_ROI_NUM) break;
    }

    roi_paras->num_of_roi = cnt;
    // venc_print_roi(roi_paras);

    return 0;
}

static void venc_print_ipcm(IpcmParas *ipcm_paras) {
    if (!ipcm_paras) return;

    av_log(NULL, AV_LOG_INFO, "ipcm config cnt: %d\n", ipcm_paras->num_of_ipcm);
    for (int i = 0; i < ipcm_paras->num_of_ipcm; i++) {
        av_log(NULL,
               AV_LOG_INFO,
               "ipcm index: %d, enable: %d, (%d, %d, %d, %d)\n",
               ipcm_paras->ipcm_attr[i].index,
               ipcm_paras->ipcm_attr[i].enable,
               ipcm_paras->ipcm_attr[i].x,
               ipcm_paras->ipcm_attr[i].y,
               ipcm_paras->ipcm_attr[i].width,
               ipcm_paras->ipcm_attr[i].height);
    }
}

/* parse ipcm option*/
// format: "i0:0:0:0:0:0;i1:0:0:0:0:0"
static int venc_get_ipcm(char *str, IpcmParas *ipcm_paras) {
    char *p = str;
    unsigned int cnt = 0;
    IpcmAttr *ipcm_attr = NULL;

    if (!str || !ipcm_paras) return -1;

    memset(ipcm_paras, 0x00, sizeof(IpcmParas));

    while ((p = strstr(p, "i")) != NULL) {
        ipcm_attr = &ipcm_paras->ipcm_attr[cnt];
        // index
        ipcm_attr->index = atoi(p + 1);
        // enable
        if ((p = strstr(p, ":")) == NULL) return -1;
        p++;
        ipcm_attr->enable = atoi(p);
        // left
        if ((p = strstr(p, ":")) == NULL) return -1;
        p++;
        ipcm_attr->x = atoi(p);
        // top
        if ((p = strstr(p, ":")) == NULL) return -1;
        p++;
        ipcm_attr->y = atoi(p);
        // right
        if ((p = strstr(p, ":")) == NULL) return -1;
        p++;
        ipcm_attr->width = atoi(p);
        // bottom
        if ((p = strstr(p, ":")) == NULL) return -1;
        p++;
        ipcm_attr->height = atoi(p);

        // num
        cnt++;
        if (cnt >= MAX_IPCM_NUM) break;
    }

    ipcm_paras->num_of_ipcm = cnt;
    // venc_print_ipcm(ipcm_paras);

    return 0;
}

/* parse hdr10display option*/
// format: "h0:0:0:0:0:0:0:0:0:0:0"
static int venc_get_hdr10_display(char *str, Hdr10DisplayAttr *Hdr10DisplayAttr) {
    char *pt = str;
    if (!str || !Hdr10DisplayAttr) return -1;

    while ((pt = strstr(pt, "h")) != NULL) {
        // enable
        Hdr10DisplayAttr->hdr10_display_enable = atoi(pt + 1);
        if ((pt = strstr(pt, ":")) == NULL) return -1;
        pt++;
        // dx0
        Hdr10DisplayAttr->hdr10_dx0 = atoi(pt);
        if ((pt = strstr(pt, ":")) == NULL) return -1;
        pt++;
        // dy0
        Hdr10DisplayAttr->hdr10_dy0 = atoi(pt);
        if ((pt = strstr(pt, ":")) == NULL) return -1;
        pt++;
        // dx1
        Hdr10DisplayAttr->hdr10_dx1 = atoi(pt);
        if ((pt = strstr(pt, ":")) == NULL) return -1;
        pt++;
        // dy1
        Hdr10DisplayAttr->hdr10_dy1 = atoi(pt);
        if ((pt = strstr(pt, ":")) == NULL) return -1;
        pt++;
        // dx2
        Hdr10DisplayAttr->hdr10_dx2 = atoi(pt);
        if ((pt = strstr(pt, ":")) == NULL) return -1;
        pt++;
        // dy2
        Hdr10DisplayAttr->hdr10_dy2 = atoi(pt);
        if ((pt = strstr(pt, ":")) == NULL) return -1;
        pt++;
        // wx
        Hdr10DisplayAttr->hdr10_wx = atoi(pt);
        if ((pt = strstr(pt, ":")) == NULL) return -1;
        pt++;
        // wy
        Hdr10DisplayAttr->hdr10_wy = atoi(pt);
        if ((pt = strstr(pt, ":")) == NULL) return -1;
        pt++;
        // max
        Hdr10DisplayAttr->hdr10_maxluma = atoi(pt);
        if ((pt = strstr(pt, ":")) == NULL) return -1;
        pt++;
        // min
        Hdr10DisplayAttr->hdr10_minluma = atoi(pt);
    }

    av_log(NULL,
           AV_LOG_INFO,
           "HDR10 Display: enable:%d, dx0=%d, dy0=%d, dx1=%d, dy1=%d, dx2=%d, dy2=%d, wx=%d, wy=%d, maxluma=%d, "
           "minluma=%d .\n",
           Hdr10DisplayAttr->hdr10_display_enable,
           Hdr10DisplayAttr->hdr10_dx0,
           Hdr10DisplayAttr->hdr10_dy0,
           Hdr10DisplayAttr->hdr10_dx1,
           Hdr10DisplayAttr->hdr10_dx1,
           Hdr10DisplayAttr->hdr10_dx2,
           Hdr10DisplayAttr->hdr10_dx2,
           Hdr10DisplayAttr->hdr10_wx,
           Hdr10DisplayAttr->hdr10_wy,
           Hdr10DisplayAttr->hdr10_maxluma,
           Hdr10DisplayAttr->hdr10_minluma);

    return 0;
}
/* parse hdr10light option*/
// format: "h0:0:0"
static int venc_get_hdr10_light(char *str, Hdr10LightAttr *Hdr10LightAttr) {
    char *p = str;
    if (!str || !Hdr10LightAttr) return -1;

    while ((p = strstr(p, "h")) != NULL) {
        // enable
        Hdr10LightAttr->hdr10_lightlevel_enable = atoi(p + 1);
        if ((p = strstr(p, ":")) == NULL) return -1;
        p++;
        // maxlevel
        Hdr10LightAttr->hdr10_maxlight = atoi(p);
        if ((p = strstr(p, ":")) == NULL) return -1;
        p++;
        // avglevel
        Hdr10LightAttr->hdr10_avglight = atoi(p);
    }

    av_log(NULL,
           AV_LOG_INFO,
           "HDR10 Light: enable:%d, maxlevel=%d, avglevel=%d. \n",
           Hdr10LightAttr->hdr10_lightlevel_enable,
           Hdr10LightAttr->hdr10_maxlight,
           Hdr10LightAttr->hdr10_avglight);
    return 0;
}

/* parse hdr10color option*/
// format: "h0:9:9:0"
static int venc_get_hdr10_color(char *str, Hdr10ColorAttr *Hdr10ColorAttr) {
    char *p = str;
    if (!str || !Hdr10ColorAttr) return -1;

    while ((p = strstr(p, "h")) != NULL) {
        // enable
        Hdr10ColorAttr->hdr10_color_enable = atoi(p + 1);
        if ((p = strstr(p, ":")) == NULL) return -1;
        p++;
        // primary
        Hdr10ColorAttr->hdr10_primary = atoi(p);
        if ((p = strstr(p, ":")) == NULL) return -1;
        p++;
        // matrix
        Hdr10ColorAttr->hdr10_matrix = atoi(p);
        if ((p = strstr(p, ":")) == NULL) return -1;
        p++;
        // transfer
        Hdr10ColorAttr->hdr10_transfer = atoi(p);
    }

    av_log(NULL,
           AV_LOG_INFO,
           "HDR10 Color: enable:%d, primary=%d, matrix=%d, transfer:%d. \n",
           Hdr10ColorAttr->hdr10_color_enable,
           Hdr10ColorAttr->hdr10_primary,
           Hdr10ColorAttr->hdr10_matrix,
           Hdr10ColorAttr->hdr10_transfer);

    return 0;
}

static int32_t vsv_check_area(VCEncPictureArea *area, ESEncVidContext *ctx) {
    int32_t w = (ctx->width + ctx->max_cu_size - 1) / ctx->max_cu_size;
    int32_t h = (ctx->height + ctx->max_cu_size - 1) / ctx->max_cu_size;

    // av_log(NULL, AV_LOG_INFO, "vsv_check_area, w: %d, h: %d\n", w, h);
    // av_log(NULL, AV_LOG_INFO, "vsv_check_area, %d-%d-%d-%d\n", area->left, area->top, area->right, area->bottom);

    if ((area->left < (uint32_t)w) && (area->right < (uint32_t)w) && (area->top < (uint32_t)h)
        && (area->bottom < (uint32_t)h))
        return 1;

    return 0;
}

static int vce_get_res_index(int w, int h) {
    if (w * h >= 3840 * 2160) {
        return 6;
    } else if (w * h >= 1920 * 1080) {
        return 5;
    } else if (w * h >= 1280 * 720) {
        return 4;
    } else if (w * h >= 854 * 480) {
        return 3;
    } else if (w * h >= 640 * 360) {
        return 2;
    } else if (w * h >= 428 * 240) {
        return 1;
    }
    return 0;
}

static int vsv_encode_set_vceparam(AVCodecContext *avctx) {
    ESEncVidContext *ctx = (ESEncVidContext *)avctx->priv_data;
    ESEncVidContext *options = ctx;
    ESEncVidInternalContext *in_ctx = (ESEncVidInternalContext *)&ctx->in_ctx;
    uint32_t width, height;

    /* get default bitrate or limit bitrate */
    width = options->width;
    height = options->height;
    av_log(NULL, AV_LOG_DEBUG, "check bitrate by width %d height %d\n", width, height);

    if (options->bit_per_second == DEFAULT_VALUE) {
        int res_index = vce_get_res_index(width, height);
        options->bit_per_second = vce_default_bitrate[res_index];
        av_log(NULL, AV_LOG_DEBUG, "get default bitrate %d\n", options->bit_per_second);
    }
    return 0;
}

static int vsv_preset_params_set(AVCodecContext *avctx) {
    ESEncVidContext *ctx = (ESEncVidContext *)avctx->priv_data;
    ESEncVidContext *options = ctx;
    VSVPreset preset = 0;

    av_log(avctx, AV_LOG_DEBUG, "+++ vcepreset %s\n", ctx->preset);

    if (ctx->preset) {
        if (strcmp(ctx->preset, "superfast") == 0) {
            preset = VSV_PRESET_SUPERFAST;
        } else if (strcmp(ctx->preset, "fast") == 0) {
            preset = VSV_PRESET_FAST;
        } else if (strcmp(ctx->preset, "medium") == 0) {
            preset = VSV_PRESET_MEDIUM;
        } else if (strcmp(ctx->preset, "slow") == 0) {
            preset = VSV_PRESET_SLOW;
        } else if (strcmp(ctx->preset, "superslow") == 0) {
            preset = VSV_PRESET_SUPERSLOW;
        } else {
            av_log(avctx, AV_LOG_ERROR, "unknow vcepreset %s\n", ctx->preset);
            return -1;
        }

        if (options->codec_format == VCENC_VIDEO_CODEC_HEVC) {
            switch (preset) {
                case VSV_PRESET_SUPERFAST:
                    if (options->intra_qp_delta == DEFAULT) options->intra_qp_delta = -2;
                    if (options->qp_hdr == DEFAULT) options->qp_hdr = -1;
                    if (options->pic_rc == DEFAULT) options->pic_rc = 1;
                    if (options->ctb_rc == DEFAULT) options->ctb_rc = 0;
                    if (options->gop_size == DEFAULT) options->gop_size = 1;
                    if (options->rdo_level == DEFAULT) options->rdo_level = 1;
                    if (options->lookahead_depth == DEFAULT) options->lookahead_depth = 0;
                    break;
                case VSV_PRESET_FAST:
                    if (options->intra_qp_delta == DEFAULT) options->intra_qp_delta = -2;
                    if (options->qp_hdr == DEFAULT) options->qp_hdr = -1;
                    if (options->pic_rc == DEFAULT) options->pic_rc = 1;
                    if (options->ctb_rc == DEFAULT) options->ctb_rc = 0;
                    if (options->gop_size == DEFAULT) options->gop_size = 4;
                    if (options->rdo_level == DEFAULT) options->rdo_level = 1;
                    if (options->lookahead_depth == DEFAULT) options->lookahead_depth = 0;
                    break;
                case VSV_PRESET_MEDIUM:
                    if (options->intra_qp_delta == DEFAULT) options->intra_qp_delta = -2;
                    if (options->qp_hdr == DEFAULT) options->qp_hdr = -1;
                    if (options->pic_rc == DEFAULT) options->pic_rc = 1;
                    if (options->ctb_rc == DEFAULT) options->ctb_rc = 0;
                    if (options->gop_size == DEFAULT) options->gop_size = 4;
                    if (options->rdo_level == DEFAULT) options->rdo_level = 1;
                    if (options->lookahead_depth == DEFAULT) options->lookahead_depth = 20;
                    break;
                case VSV_PRESET_SLOW:
                    if (options->intra_qp_delta == DEFAULT) options->intra_qp_delta = -2;
                    if (options->qp_hdr == DEFAULT) options->qp_hdr = -1;
                    if (options->pic_rc == DEFAULT) options->pic_rc = 1;
                    if (options->ctb_rc == DEFAULT) options->ctb_rc = 0;
                    if (options->gop_size == DEFAULT) options->gop_size = 0;
                    if (options->rdo_level == DEFAULT) options->rdo_level = 2;
                    if (options->lookahead_depth == DEFAULT) options->lookahead_depth = 30;
                    break;
                case VSV_PRESET_SUPERSLOW:
                    if (options->intra_qp_delta == DEFAULT) options->intra_qp_delta = -2;
                    if (options->qp_hdr == DEFAULT) options->qp_hdr = -1;
                    if (options->pic_rc == DEFAULT) options->pic_rc = 1;
                    if (options->ctb_rc == DEFAULT) options->ctb_rc = 0;
                    if (options->gop_size == DEFAULT) options->gop_size = 0;
                    if (options->rdo_level == DEFAULT) options->rdo_level = 3;
                    if (options->lookahead_depth == DEFAULT) options->lookahead_depth = 40;
                    break;
                case VSV_PRESET_NONE:
                    break;
                default:
                    av_log(avctx, AV_LOG_ERROR, "unknow preset %d\n", preset);
                    return -1;
            }

        } else if (options->codec_format == VCENC_VIDEO_CODEC_H264) {
            switch (preset) {
                case VSV_PRESET_SUPERFAST:
                    if (options->intra_qp_delta == DEFAULT) options->intra_qp_delta = -2;
                    if (options->qp_hdr == DEFAULT) options->qp_hdr = -1;
                    if (options->pic_rc == DEFAULT) options->pic_rc = 1;
                    if (options->ctb_rc == DEFAULT) options->ctb_rc = 0;
                    if (options->gop_size == DEFAULT) options->gop_size = 1;
                    if (options->lookahead_depth == DEFAULT) options->lookahead_depth = 0;
                    break;
                case VSV_PRESET_FAST:
                    if (options->intra_qp_delta == DEFAULT) options->intra_qp_delta = -2;
                    if (options->qp_hdr == DEFAULT) options->qp_hdr = -1;
                    if (options->pic_rc == DEFAULT) options->pic_rc = 1;
                    if (options->ctb_rc == DEFAULT) options->ctb_rc = 0;
                    if (options->gop_size == DEFAULT) options->gop_size = 4;
                    if (options->lookahead_depth == DEFAULT) options->lookahead_depth = 0;
                    break;
                case VSV_PRESET_MEDIUM:
                    if (options->intra_qp_delta == DEFAULT) options->intra_qp_delta = -2;
                    if (options->qp_hdr == DEFAULT) options->qp_hdr = -1;
                    if (options->pic_rc == DEFAULT) options->pic_rc = 1;
                    if (options->ctb_rc == DEFAULT) options->ctb_rc = 0;
                    if (options->gop_size == DEFAULT) options->gop_size = 4;
                    if (options->lookahead_depth == DEFAULT) options->lookahead_depth = 20;
                    break;
                case VSV_PRESET_SLOW:
                    if (options->intra_qp_delta == DEFAULT) options->intra_qp_delta = -2;
                    if (options->qp_hdr == DEFAULT) options->qp_hdr = -1;
                    if (options->pic_rc == DEFAULT) options->pic_rc = 1;
                    if (options->ctb_rc == DEFAULT) options->ctb_rc = 0;
                    if (options->gop_size == DEFAULT) options->gop_size = 0;
                    if (options->lookahead_depth == DEFAULT) options->lookahead_depth = 30;
                    break;
                case VSV_PRESET_SUPERSLOW:
                    if (options->intra_qp_delta == DEFAULT) options->intra_qp_delta = -2;
                    if (options->qp_hdr == DEFAULT) options->qp_hdr = -1;
                    if (options->pic_rc == DEFAULT) options->pic_rc = 1;
                    if (options->ctb_rc == DEFAULT) options->ctb_rc = 0;
                    if (options->gop_size == DEFAULT) options->gop_size = 0;
                    if (options->lookahead_depth == DEFAULT) options->lookahead_depth = 40;
                    break;
                case VSV_PRESET_NONE:
                    break;
                default:
                    av_log(avctx, AV_LOG_ERROR, "unknow preset %d\n", preset);
                    return -1;
            }
        }
    } else {
        if (options->rdo_level == DEFAULT) options->rdo_level = 1;
        if (options->gop_size == DEFAULT) options->gop_size = 0;
        if (options->lookahead_depth == DEFAULT) options->lookahead_depth = 0;
    }

    return 0;
}

static int esenc_vid_validate_options(ESEncVidContext *esenc_vide_ctx) {
    if (!esenc_vide_ctx) {
        av_log(NULL, AV_LOG_ERROR, "esenc_vid_check_resolution, esenc_vide_ctx is NULL\n");
        return -1;
    }

    // check resolution
    if (IS_H264(esenc_vide_ctx->codec_format)) {
        if (esenc_vide_ctx->width < 136 || esenc_vide_ctx->width > 8192 || esenc_vide_ctx->height < 128
            || esenc_vide_ctx->height > 8640) {
            av_log(NULL, AV_LOG_ERROR, "unsupport resolution: %dx%d\n", esenc_vide_ctx->width, esenc_vide_ctx->height);
            return -1;
        }
    } else {
        if (esenc_vide_ctx->width < 144 || esenc_vide_ctx->width > 8192 || esenc_vide_ctx->height < 128
            || esenc_vide_ctx->height > 8640) {
            av_log(NULL, AV_LOG_ERROR, "unsupport resolution: %dx%d\n", esenc_vide_ctx->width, esenc_vide_ctx->height);
            return -1;
        }
    }

    return 0;
}

static int vsv_encode_set_default_opt(AVCodecContext *avctx) {
    ESEncVidContext *ctx = (ESEncVidContext *)avctx->priv_data;
    ESEncVidContext *options = ctx;
    enum AVPixelFormat pix_fmt = avctx->pix_fmt;
    int i;

    /*stride*/
    // hw request input frame alignment must >= 64
    if (options->exp_of_input_alignment < 64 && options->exp_of_input_alignment % 16) {
        av_log(NULL, AV_LOG_WARNING, "force stride from %d to 64\n", options->exp_of_input_alignment);
        options->exp_of_input_alignment = 64;
    }
    options->exp_of_input_alignment = log2(options->exp_of_input_alignment);
    // input raw pictrure W and H
    options->lum_width_src = FFALIGN(avctx->width, 1 << options->exp_of_input_alignment);
    options->lum_height_src = avctx->height;

    // encoded streaming w and h
    options->width = avctx->width;
    options->height = avctx->height;

    CropInfo crop_info;
    // Parse crop string.
    if (ff_codec_get_crop(options->crop_str, &crop_info)) {
        av_log(NULL, AV_LOG_ERROR, "parser crop config error\n");
        return -1;
    }

    av_log(NULL,
           AV_LOG_DEBUG,
           "crop info: w:%d, h:%d, x:%d, y:%d\n",
           crop_info.crop_width,
           crop_info.crop_height,
           crop_info.crop_xoffset,
           crop_info.crop_yoffset);

    if (crop_info.crop_width > 0) options->width = crop_info.crop_width;
    if (crop_info.crop_height > 0) options->height = crop_info.crop_height;

    if (crop_info.crop_xoffset >= 0) options->hor_offset_src = crop_info.crop_xoffset;
    if (crop_info.crop_yoffset >= 0) options->ver_offset_src = crop_info.crop_yoffset;

    // framerate,ffmpeg video common option
    options->output_rate_denom = avctx->framerate.den;
    options->output_rate_numer = avctx->framerate.num;

    if (pix_fmt == AV_PIX_FMT_ES) {
        pix_fmt = avctx->sw_pix_fmt;
    }

    switch (pix_fmt) {
        case AV_PIX_FMT_YUV420P:
            options->input_format = VCENC_YUV420_PLANAR;
            break;
        case AV_PIX_FMT_NV12:
            options->input_format = VCENC_YUV420_SEMIPLANAR;
            break;
        case AV_PIX_FMT_NV21:
            options->input_format = VCENC_YUV420_SEMIPLANAR_VU;
            break;
        case AV_PIX_FMT_UYVY422:
            options->input_format = VCENC_YUV422_INTERLEAVED_UYVY;
            break;
        case AV_PIX_FMT_YUYV422:
            options->input_format = VCENC_YUV422_INTERLEAVED_YUYV;
            break;
        case AV_PIX_FMT_YUV420P10LE:
            options->input_format = VCENC_YUV420_PLANAR_10BIT_I010;
            break;
        case AV_PIX_FMT_P010LE:
            options->input_format = VCENC_YUV420_PLANAR_10BIT_P010;
            break;
        default:
            av_log(NULL, AV_LOG_ERROR, "pix_fmt: %s not support\n", ff_vsv_encode_get_fmt_char(pix_fmt));
            return -1;
    }

    options->format_customized_type = -1;

    options->max_cu_size = 64;
    options->min_cu_size = 8;
    options->max_tr_size = 16;
    options->min_tr_size = 4;
    options->tr_depth_intra = 2;  // mfu =>0
    options->tr_depth_inter = (options->max_cu_size == 64) ? 4 : 3;
    // options->intra_pic_rate   = 0;  // only first is idr.
    if (!strcmp(avctx->codec->name, "h265_es_encoder")) {
        options->codec_format = VCENC_VIDEO_CODEC_HEVC;
        options->roi_map_delta_qp_block_unit = 3;
    } else if (!strcmp(avctx->codec->name, "h264_es_encoder")) {
        options->codec_format = VCENC_VIDEO_CODEC_H264;
        options->max_cu_size = 16;
        options->min_cu_size = 8;
        options->max_tr_size = 16;
        options->min_tr_size = 4;
        options->tr_depth_intra = 1;
        options->tr_depth_inter = 2;
        options->roi_map_delta_qp_block_unit = 2;
    }

    options->tol_moving_bit_rate = 2000;
    options->monitor_frames = DEFAULT;
    options->u32_static_scene_ibit_percent = 80;
    // options->intra_qp_delta = DEFAULT;
    // options->b_frame_qp_delta = -1;

    options->tc_offset = 0;
    options->beta_offset = 0;

    options->smooth_psnr_in_gop = 0;

    // options->byte_stream = 1;

    // options->chroma_qp_offset = 0;

    options->strong_intra_smoothing_enabled_flag = 0;

    options->pcm_loop_filter_disabled_flag = 0;

    options->intra_area_left = options->intra_area_right = options->intra_area_top = options->intra_area_bottom =
        -1; /* disabled */
    // options->ipcm1_area_left = options->ipcm1_area_right = options->ipcm1_area_top = options->ipcm1_area_bottom =
    //     -1; /* disabled */
    // options->ipcm2_area_left = options->ipcm2_area_right = options->ipcm2_area_top = options->ipcm2_area_bottom =
    //     -1; /* disabled */
    options->gdr_duration = 0;

    options->cabac_init_flag = 0;
    options->cir_start = 0;
    options->cir_interval = 0;
    options->enable_deblock_override = 0;
    options->deblock_override = 0;

    options->enable_scaling_list = 0;

    options->compressor = 0;
    options->block_rc_size = DEFAULT;
    options->rc_qp_delta_range = DEFAULT;
    options->rc_base_mb_complexity = DEFAULT;
    // options->pic_qp_delta_min = DEFAULT;
    options->pic_qp_delta_max = DEFAULT;
    // options->ctb_rc_row_qp_step = DEFAULT;

    // options->bitrate_window = DEFAULT;
    options->gop_size = options->b_nums + 1;
    options->num_refP = 1;
    // options->gop_cfg = NULL;
    // options->gop_lowdelay = 0;
    options->long_term_gap = 0;
    options->long_term_gap_offset = 0;
    options->long_term_qp_delta = 0;
    options->ltr_interval = DEFAULT;

    options->out_recon_frame = 1;

    // options->roi_map_delta_qp_block_unit = 3;
    // options->roi_map_delta_qp_enable = 0;
    options->roi_map_delta_qp_file = NULL;
    options->roi_map_delta_qp_bin_file = NULL;
    options->roi_map_info_bin_file = NULL;
    options->roimap_cu_ctrl_info_bin_file = NULL;
    options->roimap_cu_ctrl_index_bin_file = NULL;
    options->roi_cu_ctrl_ver = 0;
    options->roi_qp_delta_ver = 1;
    // options->ipcm_map_enable = 0;
    options->ipcm_map_file = NULL;

    options->interlaced_frame = 0;

    /* low latency */
    // options->input_line_buf_mode = 0;
    // options->input_line_buf_depth = DEFAULT;
    // options->amount_per_loop_back = 0;
    options->exp_of_ref_alignment = 6;
    options->exp_of_ref_ch_alignment = 6;

    options->multimode = 0;

    for (i = 0; i < MAX_STREAMS; i++) options->streamcfg[i] = NULL;

#ifdef DEBUG_CU_INFO
    options->enable_output_cu_info = 1;
#else
    options->enable_output_cu_info = 0;
#endif
    options->p010_ref_enable = 0;

    // options->rdo_level = 1;
    options->hashtype = 0;
    options->verbose = 0;

    // /* constant chroma control */
    // options->const_chroma_en = 0;
    // options->const_cb = DEFAULT;
    // options->const_cr = DEFAULT;

    for (i = 0; i < MAX_SCENE_CHANGE; i++) options->scene_change[i] = 0;

    options->tiles_enabled_flag = 0;
    options->num_tile_columns = 1;
    options->num_tile_rows = 1;
    options->loop_filter_across_tiles_enabled_flag = 1;

    options->skip_frame_enabled_flag = 0;
    options->skip_frame_poc = 0;

    options->pic_order_cnt_type = 0;
    options->log2_max_pic_order_cnt_lsb = 16;
    options->log2_max_frame_num = 12;

    options->rps_in_slice_header = 0;
    options->ssim = 1;
    options->cutree_blkratio = 1;

    /* skip mode */
    // options->skip_map_enable = 0;
    options->skip_map_file = NULL;
    options->skip_map_block_unit = 0;

    /* frame level core parallel option */
    options->parallel_core_num = 1;

    // add for transcode
    options->internal_enc_index = 0;  //-1;

    /* two stream buffer */
    options->stream_buf_chain = 0;

    /*multi-segment of stream buffer*/
    options->stream_multi_segment_mode = 0;
    options->stream_multi_segment_amount = 4;

    /*dump register*/
    options->dump_register = 0;

    options->rasterscan = 0;

    // rc setting, using ffmpeg common option
    options->bit_per_second = avctx->bit_rate;
    options->qp_min = avctx->qmin;
    options->qp_max = avctx->qmax;

    options->qp_hdr = DEFAULT;
    options->u32_static_scene_ibit_percent = 80;

    // roi
    if (venc_get_roi(options->roi_str, &options->roi_tbl)) {
        av_log(NULL, AV_LOG_ERROR, "parser roi config error\n");
        return -1;
    }

    // ipcm
    if (venc_get_ipcm(options->ipcm_str, &options->ipcm_tbl)) {
        av_log(NULL, AV_LOG_ERROR, "parser ipcm config error\n");
        return -1;
    }

    // hdr10
    if (venc_get_hdr10_display(options->hdr_display_str, &options->hdr10_display)) {
        av_log(NULL, AV_LOG_ERROR, "parser hdr10_display config error\n");
        return -1;
    }
    if (venc_get_hdr10_light(options->hdr_light_str, &options->hdr10_light)) {
        av_log(NULL, AV_LOG_ERROR, "parser hdr10_light config error\n");
        return -1;
    }
    if (venc_get_hdr10_color(options->hdr_color_str, &options->hdr10_color)) {
        av_log(NULL, AV_LOG_ERROR, "parser hdr10_color config error\n");
        return -1;
    }

    // validate options
    if (esenc_vid_validate_options(options)) {
        av_log(NULL, AV_LOG_ERROR, "esenc_vid_validate_options error\n");
        return -1;
    }

    return 0;
}

static int vsv_encode_get_options(AVCodecContext *avctx) {
    ESEncVidContext *ctx = (ESEncVidContext *)avctx->priv_data;
    ESEncVidContext *options = ctx;

    av_log(avctx, AV_LOG_DEBUG, "+++ vsv_encode_get_options\n");
    av_log(avctx, AV_LOG_DEBUG, "+++ options->lum_width_src = %d\n", options->lum_width_src);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->lum_height_src = %d\n", options->lum_height_src);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->width = %d\n", options->width);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->height = %d\n", options->height);
    av_log(avctx,
           AV_LOG_DEBUG,
           "+++ options->input_format = %d, pix_fmt: %d, sw_pix_fmt: %d\n",
           options->input_format,
           avctx->pix_fmt,
           avctx->sw_pix_fmt);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->rotation = %d\n", options->rotation);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->bitdepth = %d\n", options->bitdepth);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->max_TLayers = %d\n", options->max_TLayers);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->mirror = %d\n", options->mirror);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->hor_offset_src = %d\n", options->hor_offset_src);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->ver_offset_src = %d\n", options->ver_offset_src);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->bit_per_second = %d\n", options->bit_per_second);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->output_rate_numer = %d\n", options->output_rate_numer);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->output_rate_denom = %d\n", options->output_rate_denom);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->intra_pic_rate = %d\n", options->intra_pic_rate);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->bitrate_window = %d\n", options->bitrate_window);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->intra_qp_delta = %d\n", options->intra_qp_delta);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->qp_hdr = %d\n", options->qp_hdr);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->qp_min = %d\n", options->qp_min);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->qp_max = %d\n", options->qp_max);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->fixed_intra_qp = %d\n", options->fixed_qp_I);

    av_log(avctx, AV_LOG_DEBUG, "+++ options->pic_skip = %d\n", options->pic_skip);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->profile = %d\n", options->profile);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->level = %d\n", options->level);

    av_log(avctx, AV_LOG_DEBUG, "+++ options->tier = %d\n", options->tier);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->exp_of_input_alignment  = %d\n", options->exp_of_input_alignment);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->exp_of_ref_alignment    = %d\n", options->exp_of_ref_alignment);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->exp_of_ref_ch_alignment = %d\n", options->exp_of_ref_ch_alignment);

    av_log(avctx, AV_LOG_DEBUG, "+++ options->byte_stream = %d\n", options->byte_stream);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->video_range = %d\n", options->video_range);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->chroma_qp_offset = %d\n", options->chroma_qp_offset);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->gop_size = %d\n", options->gop_size);
    av_log(avctx, AV_LOG_DEBUG, "+++ options->lookahead_depth = %d\n", options->lookahead_depth);

    av_log(avctx,
           AV_LOG_DEBUG,
           "+++ dump_path:%s, frame en:%d, pkt en:%d, frame time:%d, pkt time:%d\n",
           options->dump_path,
           options->dump_frame_enable,
           options->dump_pkt_enable,
           options->dump_frame_time,
           options->dump_pkt_time);

    return 0;
}

static void getAlignedPicSizebyFormat(int32_t type,
                                      uint32_t width,
                                      uint32_t height,
                                      uint32_t alignment,
                                      uint32_t *para_luma_size,
                                      uint32_t *para_chroma_size,
                                      uint32_t *para_picture_size) {
    uint32_t luma_stride = 0, chroma_stride = 0;
    uint32_t luma_size = 0, chroma_size = 0, picture_size = 0;

    VCEncGetAlignedStride(width, type, &luma_stride, &chroma_stride, alignment);
    switch (type) {
        case VCENC_YUV420_PLANAR:
            luma_size = luma_stride * height;
            chroma_size = chroma_stride * height / 2 * 2;
            break;
        case VCENC_YUV420_SEMIPLANAR:
        case VCENC_YUV420_SEMIPLANAR_VU:
            luma_size = luma_stride * height;
            chroma_size = chroma_stride * height / 2;
            break;
        case VCENC_YUV422_INTERLEAVED_YUYV:
        case VCENC_YUV422_INTERLEAVED_UYVY:
        case VCENC_RGB565:
        case VCENC_BGR565:
        case VCENC_RGB555:
        case VCENC_BGR555:
        case VCENC_RGB444:
        case VCENC_BGR444:
        case VCENC_RGB888:
        case VCENC_BGR888:
        case VCENC_RGB101010:
        case VCENC_BGR101010:
            luma_size = luma_stride * height;
            chroma_size = 0;
            break;
        case VCENC_YUV420_PLANAR_10BIT_I010:
            luma_size = luma_stride * height;
            chroma_size = chroma_stride * height / 2 * 2;
            break;
        case VCENC_YUV420_PLANAR_10BIT_P010:

            luma_size = luma_stride * height;
            chroma_size = chroma_stride * height / 2;
            break;
        case VCENC_YUV420_PLANAR_10BIT_PACKED_PLANAR:
            luma_size = luma_stride * 10 / 8 * height;
            chroma_size = chroma_stride * 10 / 8 * height / 2 * 2;
            break;
        case VCENC_YUV420_10BIT_PACKED_Y0L2:
            luma_size = luma_stride * 2 * 2 * height / 2;
            chroma_size = 0;
            break;
        case VCENC_YUV420_SEMIPLANAR_101010:
            luma_size = luma_stride * height;
            chroma_size = chroma_stride * height / 2;
            break;
        case VCENC_YUV420_8BIT_TILE_64_4:
        case VCENC_YUV420_UV_8BIT_TILE_64_4:
            luma_size = luma_stride * ((height + 3) / 4);
            chroma_size = chroma_stride * (((height / 2) + 3) / 4);
            break;
        case VCENC_YUV420_10BIT_TILE_32_4:
            luma_size = luma_stride * ((height + 3) / 4);
            chroma_size = chroma_stride * (((height / 2) + 3) / 4);
            break;
        case VCENC_YUV420_10BIT_TILE_48_4:
        case VCENC_YUV420_VU_10BIT_TILE_48_4:
            luma_size = luma_stride * ((height + 3) / 4);
            chroma_size = chroma_stride * (((height / 2) + 3) / 4);
            break;
        case VCENC_YUV420_8BIT_TILE_128_2:
        case VCENC_YUV420_UV_8BIT_TILE_128_2:
            luma_size = luma_stride * ((height + 1) / 2);
            chroma_size = chroma_stride * (((height / 2) + 1) / 2);
            break;
        case VCENC_YUV420_10BIT_TILE_96_2:
        case VCENC_YUV420_VU_10BIT_TILE_96_2:
            luma_size = luma_stride * ((height + 1) / 2);
            chroma_size = chroma_stride * (((height / 2) + 1) / 2);
            break;
#ifdef SUPPORT_TCACHE
        case INPUT_FORMAT_YUV420_SEMIPLANAR_10BIT_P010BE:
#endif
            luma_stride = STRIDE(width * 2, alignment);
            chroma_stride = STRIDE(width * 2, alignment);

            luma_size = luma_stride * height;
            chroma_size = chroma_stride * height / 2;
            break;
#ifdef SUPPORT_TCACHE
        case INPUT_FORMAT_RFC_8BIT_COMPRESSED_FB:
            luma_stride = STRIDE(width, DTRC_INPUT_WIDTH_ALIGNMENT);
            luma_size = luma_stride * STRIDE(height, DTRC_INPUT_HEIGHT_ALIGNMENT);
            // lumaSize = luma_stride * ((height+3)/4);
            chroma_size = luma_size / 2;
            break;
        case INPUT_FORMAT_RFC_10BIT_COMPRESSED_FB:
            luma_stride = STRIDE(width, DTRC_INPUT_WIDTH_ALIGNMENT);
            luma_size = luma_stride * STRIDE(height, DTRC_INPUT_HEIGHT_ALIGNMENT) * 10 / 8;
            chroma_size = luma_size / 2;
            break;
        case INPUT_FORMAT_YUV420_PLANAR_10BIT_P010LE:
        case INPUT_FORMAT_YUV420_PLANAR_10BIT_P010BE:
            luma_stride = STRIDE(width * 2, alignment);
            chroma_stride = STRIDE((width / 2) * 2, alignment);
            luma_size = luma_stride * height;
            chroma_size = chroma_stride * (height / 2) * 2;
            break;
        case INPUT_FORMAT_ARGB_FB:
        case INPUT_FORMAT_ABGR_FB:
        case INPUT_FORMAT_RGBA_FB:
        case INPUT_FORMAT_BGRA_FB:
            luma_stride = STRIDE(width * 4, alignment);
            luma_size = luma_stride * height;
            chroma_size = 0;
            break;
        case INPUT_FORMAT_RGB24_FB:
        case INPUT_FORMAT_BGR24_FB:
            luma_stride = STRIDE(width * 3, alignment);
            luma_size = luma_stride * height;
            chroma_size = 0;
            break;
        case INPUT_FORMAT_YUV422P:
            luma_stride = STRIDE(width, alignment);
            chroma_stride = STRIDE(width / 2, alignment);
            luma_size = luma_stride * height;
            chroma_size = chroma_stride * height * 2;
            break;
        case INPUT_FORMAT_YUV422P10LE:
        case INPUT_FORMAT_YUV422P10BE:
            luma_stride = STRIDE(width * 2, alignment);
            chroma_stride = STRIDE((width / 2) * 2, alignment);
            luma_size = luma_stride * height;
            chroma_size = chroma_stride * height * 2;
            break;
        case INPUT_FORMAT_YUV444P:
            luma_stride = STRIDE(width * 2, alignment);
            luma_size = luma_stride * height;
            chroma_size = luma_size * 2;
            break;
#endif

        default:
            printf("not support this format\n");
            chroma_size = luma_size = 0;
            break;
    }

    picture_size = luma_size + chroma_size;
    if (para_luma_size != NULL) *para_luma_size = luma_size;
    if (para_chroma_size != NULL) *para_chroma_size = chroma_size;
    if (para_picture_size != NULL) *para_picture_size = picture_size;
}

/*------------------------------------------------------------------------------
Function name : vsv_init_pic_config
Description   : initial pic reference configure
Return type   : void
Argument      : VCEncIn *enc_in
------------------------------------------------------------------------------*/
static void vsv_init_pic_config(VCEncIn *enc_in) {
    int32_t i, j, k, i32_poc;
    int32_t i32_maxpic_order_cnt_lsb = 1 << 16;

    ASSERT(enc_in != NULL);

    enc_in->gopCurrPicConfig.codingType = FRAME_TYPE_RESERVED;
    enc_in->gopCurrPicConfig.numRefPics = NUMREFPICS_RESERVED;
    enc_in->gopCurrPicConfig.poc = -1;
    enc_in->gopCurrPicConfig.QpFactor = QPFACTOR_RESERVED;
    enc_in->gopCurrPicConfig.QpOffset = QPOFFSET_RESERVED;
    enc_in->gopCurrPicConfig.temporalId = TEMPORALID_RESERVED;
    enc_in->i8SpecialRpsIdx = -1;
    for (k = 0; k < VCENC_MAX_REF_FRAMES; k++) {
        enc_in->gopCurrPicConfig.refPics[k].ref_pic = INVALITED_POC;
        enc_in->gopCurrPicConfig.refPics[k].used_by_cur = 0;
    }

    for (k = 0; k < VCENC_MAX_LT_REF_FRAMES; k++) enc_in->long_term_ref_pic[k] = INVALITED_POC;

    enc_in->bIsPeriodUsingLTR = 0;
    enc_in->bIsPeriodUpdateLTR = 0;

    for (i = 0; i < enc_in->gopConfig.special_size; i++) {
        if (enc_in->gopConfig.pGopPicSpecialCfg[i].i32Interval <= 0) continue;

        if (enc_in->gopConfig.pGopPicSpecialCfg[i].i32Ltr == 0)
            enc_in->bIsPeriodUsingLTR = 1;
        else {
            enc_in->bIsPeriodUpdateLTR = 1;

            for (k = 0; k < (int32_t)enc_in->gopConfig.pGopPicSpecialCfg[i].numRefPics; k++) {
                int32_t i32LTRIdx = enc_in->gopConfig.pGopPicSpecialCfg[i].refPics[k].ref_pic;
                if ((IS_LONG_TERM_REF_DELTAPOC(i32LTRIdx))
                    && ((enc_in->gopConfig.pGopPicSpecialCfg[i].i32Ltr - 1) == LONG_TERM_REF_DELTAPOC2ID(i32LTRIdx))) {
                    enc_in->bIsPeriodUsingLTR = 1;
                }
            }
        }
    }

    memset(enc_in->bLTR_need_update, 0, sizeof(bool) * VCENC_MAX_LT_REF_FRAMES);
    enc_in->bIsIDR = 1;  // for vs, it must be set one time for idr interval

    i32_poc = 0;
    /* check current picture encoded as LTR*/
    enc_in->u8IdxEncodedAsLTR = 0;
    for (j = 0; j < enc_in->gopConfig.special_size; j++) {
        if (enc_in->bIsPeriodUsingLTR == 0) break;

        if ((enc_in->gopConfig.pGopPicSpecialCfg[j].i32Interval <= 0)
            || (enc_in->gopConfig.pGopPicSpecialCfg[j].i32Ltr == 0))
            continue;

        i32_poc = i32_poc - enc_in->gopConfig.pGopPicSpecialCfg[j].i32Offset;

        if (i32_poc < 0) {
            i32_poc += i32_maxpic_order_cnt_lsb;
            if (i32_poc > (i32_maxpic_order_cnt_lsb >> 1)) i32_poc = -1;
        }

        if ((i32_poc >= 0) && (i32_poc % enc_in->gopConfig.pGopPicSpecialCfg[j].i32Interval == 0)) {
            /* more than one LTR at the same frame position */
            if (0 != enc_in->u8IdxEncodedAsLTR) {
                // reuse the same POC LTR
                enc_in->bLTR_need_update[enc_in->gopConfig.pGopPicSpecialCfg[j].i32Ltr - 1] = 1;
                continue;
            }

            enc_in->gopCurrPicConfig.codingType =
                ((int32_t)enc_in->gopConfig.pGopPicSpecialCfg[j].codingType == FRAME_TYPE_RESERVED)
                    ? enc_in->gopCurrPicConfig.codingType
                    : enc_in->gopConfig.pGopPicSpecialCfg[j].codingType;
            enc_in->gopCurrPicConfig.numRefPics =
                ((int32_t)enc_in->gopConfig.pGopPicSpecialCfg[j].numRefPics == NUMREFPICS_RESERVED)
                    ? enc_in->gopCurrPicConfig.numRefPics
                    : enc_in->gopConfig.pGopPicSpecialCfg[j].numRefPics;
            enc_in->gopCurrPicConfig.QpFactor = (enc_in->gopConfig.pGopPicSpecialCfg[j].QpFactor == QPFACTOR_RESERVED)
                                                    ? enc_in->gopCurrPicConfig.QpFactor
                                                    : enc_in->gopConfig.pGopPicSpecialCfg[j].QpFactor;
            enc_in->gopCurrPicConfig.QpOffset = (enc_in->gopConfig.pGopPicSpecialCfg[j].QpOffset == QPOFFSET_RESERVED)
                                                    ? enc_in->gopCurrPicConfig.QpOffset
                                                    : enc_in->gopConfig.pGopPicSpecialCfg[j].QpOffset;
            enc_in->gopCurrPicConfig.temporalId =
                (enc_in->gopConfig.pGopPicSpecialCfg[j].temporalId == TEMPORALID_RESERVED)
                    ? enc_in->gopCurrPicConfig.temporalId
                    : enc_in->gopConfig.pGopPicSpecialCfg[j].temporalId;

            if (((int32_t)enc_in->gopConfig.pGopPicSpecialCfg[j].numRefPics != NUMREFPICS_RESERVED)) {
                for (k = 0; k < (int32_t)enc_in->gopCurrPicConfig.numRefPics; k++) {
                    enc_in->gopCurrPicConfig.refPics[k].ref_pic =
                        enc_in->gopConfig.pGopPicSpecialCfg[j].refPics[k].ref_pic;
                    enc_in->gopCurrPicConfig.refPics[k].used_by_cur =
                        enc_in->gopConfig.pGopPicSpecialCfg[j].refPics[k].used_by_cur;
                }
            }

            enc_in->u8IdxEncodedAsLTR = enc_in->gopConfig.pGopPicSpecialCfg[j].i32Ltr;
            enc_in->bLTR_need_update[enc_in->u8IdxEncodedAsLTR - 1] = 1;
        }
    }
}

/* add for ssim statistic */
static void vsv_encode_report(AVCodecContext *avctx) {
#ifndef BUILD_CMODEL
    // Transcoder_t * trans = (Transcoder_t *)trans_handle;
    ESEncVidContext *ctx = (ESEncVidContext *)avctx->priv_data;
    ESEncVidInternalContext *in_ctx = (ESEncVidInternalContext *)&ctx->in_ctx;

    if (ctx) {
        struct statistic enc_statistic = {0};
        int j = 0;
        av_log(avctx,
               AV_LOG_INFO,
               ":::ENC[%d] : %d frames, SSIM %.4f, %d Cycles/MB, %d us/frame, %.2f fps, %u bps\n",
               in_ctx->enc_index,
               enc_statistic.frame_count,
               enc_statistic.ssim_avg,
               enc_statistic.cycle_mb_avg_total,
               enc_statistic.hw_real_time_avg,
               (enc_statistic.hw_real_time_avg == 0) ? 0.0 : 1000000.0 / ((double)enc_statistic.hw_real_time_avg),
               enc_statistic.bitrate_avg);

        if (enc_statistic.cycle_mb_avg_p1) {
            av_log(avctx, AV_LOG_INFO, "\tPass 1 : %d Cycles/MB\n", enc_statistic.cycle_mb_avg_p1);
            av_log(avctx, AV_LOG_INFO, "\tPass 2 : %d Cycles/MB\n", enc_statistic.cycle_mb_avg);
        }

        if (enc_statistic.hw_real_time_avg > enc_statistic.hw_real_time_avg_remove_overlap + 10) {
            av_log(avctx,
                   AV_LOG_INFO,
                   "\tremove overlap : %d us/frame, %.2f fps\n",
                   enc_statistic.hw_real_time_avg_remove_overlap,
                   (enc_statistic.hw_real_time_avg_remove_overlap == 0)
                       ? 0.0
                       : 1000000.0 / ((double)enc_statistic.hw_real_time_avg_remove_overlap));
        }

        av_log(avctx, AV_LOG_INFO, ":::ENC[%d] Multi-core usage statistics:\n", in_ctx->enc_index);

        if (enc_statistic.total_usage == 0) enc_statistic.total_usage = 1;

        for (j = 0; j < 2; j++) {
            if (enc_statistic.core_usage_counts[2] || enc_statistic.core_usage_counts[3])
                av_log(avctx,
                       AV_LOG_INFO,
                       "\tPass 1 Slice[%d] used %6d times (%2d%%)\n",
                       j,
                       enc_statistic.core_usage_counts[2 + j],
                       (enc_statistic.core_usage_counts[2 + j] * 100) / enc_statistic.total_usage);
        }
        for (j = 0; j < 2; j++) {
            av_log(avctx,
                   AV_LOG_INFO,
                   "\tSlice[%d] used %6d times (%2d%%)\n",
                   j,
                   enc_statistic.core_usage_counts[j],
                   (enc_statistic.core_usage_counts[j] * 100) / enc_statistic.total_usage);
        }
    }
#endif
}

static int esenc_vid_alloc_output_buffer(ESEncVidInternalContext *in_ctx) {
    if (!in_ctx) return -1;

    int ret = 0;
    int picture_size = in_ctx->picture_size;
    int size = picture_size / in_ctx->compress_rate;
    av_log(NULL,
           AV_LOG_INFO,
           "venc alloc outbuf size[%d] is 1/%d of YUV[%d] \n",
           size,
           in_ctx->compress_rate,
           in_ctx->picture_size);

    // adjust buffer size, vs required >= VCENC_STREAM_MIN_BUF0_SIZE
    if (size < VCENC_STREAM_MIN_BUF0_SIZE) {
        in_ctx->compress_rate = picture_size / (VCENC_STREAM_MIN_BUF0_SIZE);
        if (picture_size % (VCENC_STREAM_MIN_BUF0_SIZE)) {
            in_ctx->compress_rate--;
            if (in_ctx->compress_rate < 2) in_ctx->compress_rate = 1;
        }

        size = picture_size / in_ctx->compress_rate;
        av_log(NULL, AV_LOG_INFO, "venc realloc outbuf size: %d is 1/%d of YUV \n", size, in_ctx->compress_rate);
    }

    for (int core_idx = 0; core_idx < in_ctx->parallel_core_num; core_idx++) {
        for (int i_buf = 0; i_buf < in_ctx->stream_buf_num; i_buf++) {
            in_ctx->outbuf_mem_factory[core_idx][i_buf].mem_type = VPU_WR | CPU_WR | CPU_RD | EWL_MEM_TYPE_SLICE;
            ret = EWLMallocLinear(
                in_ctx->ewl, size, in_ctx->input_alignment, &in_ctx->outbuf_mem_factory[core_idx][i_buf]);
            if (ret != EWL_OK) {
                in_ctx->outbuf_mem_factory[core_idx][i_buf].virtualAddress = NULL;
                av_log(NULL, AV_LOG_ERROR, "venc alloc outbuf size: %d fail\n", size);
                return -1;
            }

            av_log(NULL, AV_LOG_INFO, "venc EWL outbuf size: %d\n", in_ctx->outbuf_mem_factory[core_idx][i_buf].size);
        }
    }

    return 0;
}

static int esenc_vid_free_output_buffer(ESEncVidInternalContext *in_ctx) {
    if (!in_ctx) return -1;

    // release current buffer
    for (int core_idx = 0; core_idx < in_ctx->parallel_core_num; core_idx++) {
        for (int i_buf = 0; i_buf < in_ctx->stream_buf_num; i_buf++) {
            if (in_ctx->outbuf_mem_factory[core_idx][i_buf].virtualAddress != NULL) {
                EWLFreeLinear(in_ctx->ewl, &in_ctx->outbuf_mem_factory[core_idx][i_buf]);
                in_ctx->outbuf_mem_factory[core_idx][i_buf].virtualAddress = NULL;
            }
        }
    }

    return 0;
}

static int esenc_vid_realloc_output_buffer(ESEncVidInternalContext *in_ctx) {
    if (!in_ctx) return -1;

    if (esenc_vid_free_output_buffer(in_ctx)) {
        av_log(NULL, AV_LOG_ERROR, "venc free out buf fail\n");
        return -1;
    }

    // choose decompress rate
    in_ctx->compress_rate--;
    if (in_ctx->compress_rate < 2) in_ctx->compress_rate = 1;

    if (esenc_vid_alloc_output_buffer(in_ctx)) {
        av_log(NULL, AV_LOG_ERROR, "venc alloc out buf fail\n");
        return -1;
    }

    return 0;
}

static int esenc_vid_alloc_input_picture_buffers(ESEncVidInternalContext *in_ctx) {
    int ret = 0;
    if (!in_ctx) return -1;

    for (int core_idx = 0; core_idx < in_ctx->buffer_cnt; core_idx++) {
        in_ctx->picture_mem_factory[core_idx].mem_type = EXT_WR | VPU_RD | EWL_MEM_TYPE_DPB;
        ret = EWLMallocLinear(
            in_ctx->ewl, in_ctx->picture_size, in_ctx->input_alignment, &in_ctx->picture_mem_factory[core_idx]);
        if (ret != EWL_OK) {
            av_log(NULL, AV_LOG_ERROR, "EWLMallocLinear picture_mem_factory fail\n");
            in_ctx->picture_mem_factory[core_idx].virtualAddress = NULL;
            return -1;
        }

        // update picture_mem_status
        in_ctx->picture_mem_status[core_idx] = 0;
    }

    return 0;
}

static int esenc_vid_free_input_picture_buffer(ESEncVidInternalContext *in_ctx) {
    if (!in_ctx) return -1;

    for (int core_idx = 0; core_idx < in_ctx->buffer_cnt; core_idx++) {
        if (in_ctx->picture_mem_factory[core_idx].virtualAddress != NULL) {
            EWLFreeLinear(in_ctx->ewl, &in_ctx->picture_mem_factory[core_idx]);
        }
    }

    return 0;
}

static int esenc_vid_alloc_input_roi_qp_map_buffers(ESEncVidContext *enc_ctx,
                                                    VCEncInst enc,
                                                    ESEncVidInternalContext *in_ctx) {
    uint32_t block_size;
    int32_t total_size;
    uint32_t roi_map_delta_qp_mem_size = 0;
    uint32_t core_idx = 0;
    int ret = 0;

    if (!in_ctx || !enc || !enc_ctx) return -1;

    // allocate delta qp map memory.
    // 4 bits per block.
    block_size = ((enc_ctx->width + enc_ctx->max_cu_size - 1) & (~(enc_ctx->max_cu_size - 1)))
                 * ((enc_ctx->height + enc_ctx->max_cu_size - 1) & (~(enc_ctx->max_cu_size - 1))) / (8 * 8 * 2);
    // 8 bits per block if ipcm map/absolute roi qp is supported
    if (((struct vcenc_instance *)enc)->asic.regs.asicCfg.roiMapVersion >= 1) block_size *= 2;
    block_size = ((block_size + 63) & (~63));

    in_ctx->roi_map_delta_qp_mem_factory[0].mem_type = EXT_WR | VPU_RD | EWL_MEM_TYPE_VPU_WORKING;
    roi_map_delta_qp_mem_size = block_size * in_ctx->buffer_cnt + ROIMAP_PREFETCH_EXT_SIZE;

    if (EWLMallocLinear(in_ctx->ewl, roi_map_delta_qp_mem_size, 0, &in_ctx->roi_map_delta_qp_mem_factory[0])
        != EWL_OK) {
        in_ctx->roi_map_delta_qp_mem_factory[0].virtualAddress = NULL;
        return -1;
    }

    total_size = in_ctx->roi_map_delta_qp_mem_factory[0].size;
    for (core_idx = 0; core_idx < in_ctx->buffer_cnt; core_idx++) {
        in_ctx->roi_map_delta_qp_mem_factory[core_idx].virtualAddress =
            (uint32_t *)((ptr_t)in_ctx->roi_map_delta_qp_mem_factory[0].virtualAddress + core_idx * block_size);
        in_ctx->roi_map_delta_qp_mem_factory[core_idx].busAddress =
            in_ctx->roi_map_delta_qp_mem_factory[0].busAddress + core_idx * block_size;
        in_ctx->roi_map_delta_qp_mem_factory[core_idx].size =
            (core_idx < in_ctx->buffer_cnt - 1 ? block_size : total_size - (in_ctx->buffer_cnt - 1) * block_size);
        memset(in_ctx->roi_map_delta_qp_mem_factory[core_idx].virtualAddress, 0, block_size);
    }

    for (int core_idx = 0; core_idx < in_ctx->buffer_cnt; core_idx++) {
        in_ctx->picture_mem_factory[core_idx].mem_type = EXT_WR | VPU_RD | EWL_MEM_TYPE_DPB;
        ret = EWLMallocLinear(
            in_ctx->ewl, in_ctx->picture_size, in_ctx->input_alignment, &in_ctx->picture_mem_factory[core_idx]);
        if (ret != EWL_OK) {
            av_log(NULL, AV_LOG_ERROR, "EWLMallocLinear picture_mem_factory fail\n");
            in_ctx->picture_mem_factory[core_idx].virtualAddress = NULL;
            return -1;
        }

        // update picture_mem_status
        in_ctx->picture_mem_status[core_idx] = 0;
    }

    return 0;
}

static int esenc_vid_free_input_roi_qp_map_buffer(ESEncVidInternalContext *in_ctx) {
    if (!in_ctx) return -1;

    if (in_ctx->roi_map_delta_qp_mem_factory[0].virtualAddress != NULL) {
        EWLFreeLinear(in_ctx->ewl, &in_ctx->roi_map_delta_qp_mem_factory[0]);
    }

    return 0;
}
/*------------------------------------------------------------------------------

    vsv_encode_alloc_res

    Allocation of the physical memories used by both SW and HW:
    the input pictures and the output stream buffer.

    NOTE! The implementation uses the EWL instance from the encoder
          for OS independence. This is not recommended in final environment
          because the encoder will release the EWL instance in case of error.
          Instead, the memories should be allocated from the OS the same way
          as inside EWLMallocLinear().

------------------------------------------------------------------------------*/
static int vsv_encode_alloc_res(ESEncVidContext *enc_ctx, VCEncInst enc, ESEncVidInternalContext *in_ctx) {
    uint32_t picture_size = 0;
    uint32_t luma_size = 0, chroma_size = 0;
    uint32_t alignment = 0;

    alignment = in_ctx->input_alignment;
    getAlignedPicSizebyFormat(enc_ctx->input_format,
                              enc_ctx->lum_width_src,
                              enc_ctx->lum_height_src,
                              alignment,
                              &luma_size,
                              &chroma_size,
                              &picture_size);
    in_ctx->picture_size = picture_size;

    av_log(enc_ctx, AV_LOG_INFO, "alloc res, picture_size: %d\n", picture_size);

    if (esenc_vid_alloc_output_buffer(in_ctx)) {
        return -1;
    }

    if (esenc_vid_alloc_input_roi_qp_map_buffers(enc_ctx, enc, in_ctx)) {
        return -1;
    }

    return 0;
}

/*------------------------------------------------------------------------------

    vsv_encode_free_res

    Release all resources allcoated byt vsv_encode_alloc_res()

------------------------------------------------------------------------------*/
static void vsv_encode_free_res(ESEncVidInternalContext *in_ctx) {
    esenc_vid_free_input_roi_qp_map_buffer(in_ctx);

    if (in_ctx->picture_buffer_allocated) {
        av_log(NULL, AV_LOG_INFO, "not hwaccel mode, free input picture buffers\n");
        esenc_vid_free_input_picture_buffer(in_ctx);
        in_ctx->picture_buffer_allocated = 0;
    }
    esenc_vid_free_output_buffer(in_ctx);
}

/*------------------------------------------------------------------------------

    vsv_init_input_line_buffer
    -get line buffer params for IRQ handle
    -get address of input line buffer
------------------------------------------------------------------------------*/
static int32_t vsv_init_input_line_buffer(inputLineBufferCfg *line_buf_cfg,
                                          ESEncVidContext *options,
                                          VCEncIn *enc_in,
                                          VCEncInst inst,
                                          ESEncVidInternalContext *in_ctx) {
    VCEncCodingCtrl coding_cfg;
    uint32_t stride, chroma_stride, client_type;
    VCEncGetAlignedStride(
        options->lum_width_src, options->input_format, &stride, &chroma_stride, in_ctx->input_alignment);
    VCEncGetCodingCtrl(inst, &coding_cfg);
    client_type = IS_H264(options->codec_format) ? EWL_CLIENT_TYPE_H264_ENC : EWL_CLIENT_TYPE_HEVC_ENC;

    memset(line_buf_cfg, 0, sizeof(inputLineBufferCfg));
    line_buf_cfg->depth = coding_cfg.inputLineBufDepth;
    line_buf_cfg->hwHandShake = coding_cfg.inputLineBufHwModeEn;
    line_buf_cfg->loopBackEn = coding_cfg.inputLineBufLoopBackEn;
    line_buf_cfg->amountPerLoopBack = coding_cfg.amountPerLoopBack;
    line_buf_cfg->initSegNum = 0;
    line_buf_cfg->inst = (void *)inst;
    line_buf_cfg->wrCnt = 0;
    line_buf_cfg->inputFormat = options->input_format;
    line_buf_cfg->lumaStride = stride;
    line_buf_cfg->chromaStride = chroma_stride;
    line_buf_cfg->encWidth = options->width;
    line_buf_cfg->encHeight = options->height;
    line_buf_cfg->srcHeight = options->lum_height_src;
    line_buf_cfg->srcVerOffset = options->ver_offset_src;
    line_buf_cfg->getMbLines = &VCEncGetEncodedMbLines;
    line_buf_cfg->setMbLines = &VCEncSetInputMBLines;
    line_buf_cfg->ctbSize = IS_H264(options->codec_format) ? 16 : 64;
    line_buf_cfg->lumSrc = in_ctx->lum;
    line_buf_cfg->cbSrc = in_ctx->cb;
    line_buf_cfg->crSrc = in_ctx->cr;
    line_buf_cfg->client_type = client_type;

    if (VCEncInitInputLineBuffer(line_buf_cfg)) {
        av_log(NULL, AV_LOG_ERROR, "VCEncInitInputLineBuffer fail\n");
        return -1;
    }

    /* loopback mode */
    if (line_buf_cfg->loopBackEn && line_buf_cfg->lumBuf.buf) {
        VCEncPreProcessingCfg preProcCfg;
        enc_in->busLuma = line_buf_cfg->lumBuf.busAddress;
        enc_in->busChromaU = line_buf_cfg->cbBuf.busAddress;
        enc_in->busChromaV = line_buf_cfg->crBuf.busAddress;

        /* In loop back mode, data in line buffer start from the line to be encoded*/
        VCEncGetPreProcessing(inst, &preProcCfg);
        preProcCfg.yOffset = 0;
        VCEncSetPreProcessing(inst, &preProcCfg);
    }

    return 0;
}

/**
 *  Callback function called by the encoder SW after "segment ready"
 *  interrupt from HW. Note that this function is called after every segment is ready.
 */
static void EncStreamSegmentReady(void *cb_data) {
    // uint8_t *stream_base;
    SegmentCtl *ctl = (SegmentCtl *)cb_data;

    if (ctl->stream_multi_seg_en) {
        // stream_base = ctl->stream_base + (ctl->streamRDCounter % ctl->segment_amount) * ctl->segment_size;

        if (ctl->output_byte_stream == 0 && ctl->start_code_done == 0) {
            const uint8_t start_code_prefix[4] = {0x0, 0x0, 0x0, 0x1};
            fwrite(start_code_prefix, 1, 4, ctl->out_stream_file);
            ctl->start_code_done = 1;
        }
        printf("<----receive segment irq %d\n", ctl->stream_rd_counter);
        //    WriteStrm(ctl->outStreamFile, (uint32_t *)stream_base, ctl->segment_size, 0); TODO

        ctl->stream_rd_counter++;
    }
}

static int vsv_process_frame(AVCodecContext *avctx, AVPacket *avpkt, int *stream_size) {
    ESEncVidContext *ctx = (ESEncVidContext *)avctx->priv_data;
    ESEncVidContext *options = ctx;
    ESEncVidInternalContext *in_ctx = (ESEncVidInternalContext *)&ctx->in_ctx;
    VCEncIn *enc_in = (VCEncIn *)&(in_ctx->enc_in);
    VCEncOut *enc_out = (VCEncOut *)&ctx->encOut;
    uint8_t *ptr;
    int pkt_size = 0;

    if (options->lookahead_depth && enc_out->codingType == VCENC_INTRA_FRAME) ctx->frame_cnt_output = 0;

    VCEncGetRateCtrl(ctx->encoder, (VCEncRateCtrl *)&ctx->rc);

    if (enc_out->streamSize != 0) {
        // multi-core: output bitstream has (in_ctx->parallel_core_num-1) delay
        int32_t core_idx = (in_ctx->picture_enc_cnt - 1 - (in_ctx->frame_delay - 1)) % in_ctx->parallel_core_num;
        int32_t i_buf;
        double ssim;
        EWLLinearMem_t mem;
        for (i_buf = 0; i_buf < in_ctx->stream_buf_num; i_buf++)
            in_ctx->outbuf_mem[i_buf] = &(in_ctx->outbuf_mem_factory[core_idx][i_buf]);

        mem = *in_ctx->outbuf_mem[0];
        pkt_size = enc_out->streamSize;
        if (ctx->encoder_is_start == 1) {
            pkt_size += avctx->extradata_size;
        }

        if (av_new_packet(avpkt, pkt_size)) return -1;

        ptr = avpkt->data;
        if (ctx->encoder_is_start == 1) {
            memcpy(ptr, avctx->extradata, avctx->extradata_size);
            ptr += avctx->extradata_size;
            ctx->encoder_is_start = 0;
        }
        memcpy(ptr, (uint8_t *)mem.virtualAddress, enc_out->streamSize);
        // fill avpacket info
        if (enc_out->codingType == VCENC_INTRA_FRAME) {
            avpkt->flags = AV_PKT_FLAG_KEY;
        }

#if 0
        avpkt->pts = av_rescale_q_rnd(
            avpkt->pts, avctx->time_base, (AVRational){1, 90000}, (AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        avpkt->dts = av_rescale_q_rnd(
            avpkt->dts, avctx->time_base, (AVRational){1, 90000}, (AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        avpkt->duration = av_rescale_q(avpkt->duration, avctx->time_base, (AVRational){1, 90000});
#endif

        av_log(NULL, AV_LOG_DEBUG, "avpkt timebase, pts: %" PRId64 ", dts: %" PRId64 "\n", avpkt->pts, avpkt->dts);

        *stream_size = pkt_size;
        enc_in->timeIncrement = in_ctx->output_rate_denom;

        // dump packet
        if (ctx->dump_pkt_enable) {
            int ret = 0;
            ret = ff_codec_dump_bytes_to_file(avpkt->data, pkt_size, ctx->dump_pkt_hnd);
            if (ret == ERR_TIMEOUT) {
                av_log(NULL, AV_LOG_INFO, "pkt dump timeout\n");
                ff_codec_dump_file_close(&ctx->dump_pkt_hnd);
                ctx->dump_pkt_enable = 0;
                av_log(NULL, AV_LOG_INFO, "closed dump packet handle\n");
            } else if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "write packet into file failed\n");
            }
        }

#if 0
        ctx->total_bits += enc_out->streamSize * 8;
        in_ctx->valid_encoded_frame_number++;
        ff_ma_add_frame(&ctx->ma, enc_out->streamSize * 8);
        in_ctx->hwcycle_acc += VCEncGetPerformance(ctx->encoder);
        av_log(
            NULL,
            AV_LOG_INFO,
            "=== Encoded frame%i bits=%d TotalBits=%lu averagebitrate=%lu HWCycles=%d maxSliceBytes=%d codingType=%d\n",
            in_ctx->picture_encoded_cnt,
            enc_out->streamSize * 8,
            ctx->total_bits,
            (ctx->total_bits * in_ctx->output_rate_numer)
                / ((in_ctx->picture_encoded_cnt + 1) * in_ctx->output_rate_denom),
            VCEncGetPerformance(ctx->encoder),
            enc_out->maxSliceStreamSize,
            enc_out->codingType);

        ssim = enc_out->ssim[0] * 0.8 + 0.1 * (enc_out->ssim[1] + enc_out->ssim[2]);
        av_log(NULL,
               AV_LOG_INFO,
               "    SSIM %.4f SSIM Y %.4f U %.4f V %.4f\n",
               ssim,
               enc_out->ssim[0],
               enc_out->ssim[1],
               enc_out->ssim[2]);
        in_ctx->ssim_acc += ssim;

        if ((options->pic_rc == 1) && (in_ctx->valid_encoded_frame_number >= ctx->ma.length)) {
            in_ctx->number_square_of_error++;
            if (in_ctx->max_error_over_target < (ff_ma(&ctx->ma) - options->bit_per_second))
                in_ctx->max_error_over_target = (ff_ma(&ctx->ma) - options->bit_per_second);
            if (in_ctx->max_error_under_target < (options->bit_per_second - ff_ma(&ctx->ma)))
                in_ctx->max_error_under_target = (options->bit_per_second - ff_ma(&ctx->ma));
            in_ctx->sum_square_of_error +=
                ((float)(ABS(ff_ma(&ctx->ma) - options->bit_per_second)) * 100 / options->bit_per_second);
            in_ctx->average_square_of_error = (in_ctx->sum_square_of_error / in_ctx->number_square_of_error);
            av_log(
                NULL,
                AV_LOG_INFO,
                "    RateControl(movingBitrate=%d MaxOvertarget=%d%% MaxUndertarget=%d%% AveDeviationPerframe=%f%%)\n",
                ff_ma(&ctx->ma),
                in_ctx->max_error_over_target * 100 / options->bit_per_second,
                in_ctx->max_error_under_target * 100 / options->bit_per_second,
                in_ctx->average_square_of_error);
        }
#endif
    }

    return 0;
}

static inline void timestamp_queue_enqueue(AVFifoBuffer *queue, int64_t timestamp) {
    av_fifo_generic_write(queue, &timestamp, sizeof(timestamp), NULL);
}

static inline int64_t timestamp_queue_dequeue(AVFifoBuffer *queue) {
    int64_t timestamp = AV_NOPTS_VALUE;
    if (av_fifo_size(queue) > 0) av_fifo_generic_read(queue, &timestamp, sizeof(timestamp), NULL);

    return timestamp;
}

static int esenc_vid_fill_picture_buffer(AVCodecContext *avctx,
                                         AVFrame *pict,
                                         int8_t share_fd_buf,
                                         unsigned long share_vpa) {
    ESEncVidContext *ctx = (ESEncVidContext *)avctx->priv_data;
    EWLLinearMem_t *yuv_frame_mem = ctx->in_ctx.picture_mem;
    uint32_t alignment = 0, lumaSize = 0, chromaSize = 0, pictureSize = 0;
    int ret = 0;
    int i = 0;
    u32 luma_stride = 0;
    u32 chroma_stride = 0;
    uint8_t *data[3];
    int linesize[3];
    VCEncIn *enc_in = (VCEncIn *)&(ctx->in_ctx.enc_in);

    if (!pict || !enc_in) return -1;

    alignment = ctx->in_ctx.input_alignment;
    VCEncGetAlignedStride(ctx->lum_width_src, ctx->input_format, &luma_stride, &chroma_stride, alignment);

    getAlignedPicSizebyFormat(
        ctx->input_format, ctx->lum_width_src, ctx->lum_height_src, alignment, &lumaSize, &chromaSize, &pictureSize);

    av_log(NULL,
           AV_LOG_DEBUG,
           "w: %d, h: %d, format: %d, linesize: %d, %d, %d, data: %p, %p, %p\n",
           pict->width,
           pict->height,
           pict->format,
           pict->linesize[0],
           pict->linesize[1],
           pict->linesize[2],
           pict->data[0],
           pict->data[1],
           pict->data[2]);

    av_log(NULL,
           AV_LOG_DEBUG,
           "luma_stride: %d, chroma_stride: %d, pictureSize: %d, lumaSize: %d, chromaSize: %d\n",
           luma_stride,
           chroma_stride,
           pictureSize,
           lumaSize,
           chromaSize);

    // 1. zero frame buffer
    if (!share_fd_buf) memset(yuv_frame_mem->virtualAddress, 0, pictureSize);

    // 2. fill data as components
    switch (ctx->input_format) {
        case VCENC_YUV420_PLANAR:
        case VCENC_YUV420_PLANAR_10BIT_I010:
            if (!share_fd_buf) {
                data[0] = yuv_frame_mem->virtualAddress;
                data[1] = data[0] + lumaSize;
                data[2] = data[1] + chromaSize / 2;
                linesize[0] = luma_stride;
                linesize[1] = chroma_stride;
                linesize[2] = chroma_stride;
            }
            enc_in->busLuma = share_fd_buf ? share_vpa : yuv_frame_mem->busAddress;
            enc_in->busChromaU = enc_in->busLuma + lumaSize;
            enc_in->busChromaV = enc_in->busChromaU + chromaSize / 2;
            break;
        case VCENC_YUV420_SEMIPLANAR:
        case VCENC_YUV420_SEMIPLANAR_VU:
        case VCENC_YUV420_PLANAR_10BIT_P010:
            if (!share_fd_buf) {
                data[0] = yuv_frame_mem->virtualAddress;
                data[1] = data[0] + lumaSize;
                data[2] = NULL;
                linesize[0] = luma_stride;
                linesize[1] = chroma_stride;
                linesize[2] = 0;
            }
            enc_in->busLuma = share_fd_buf ? share_vpa : yuv_frame_mem->busAddress;
            enc_in->busChromaU = enc_in->busLuma + lumaSize;
            enc_in->busChromaV = 0;
            break;

        case VCENC_YUV422_INTERLEAVED_UYVY:
        case VCENC_YUV422_INTERLEAVED_YUYV:
            if (!share_fd_buf) {
                data[0] = yuv_frame_mem->virtualAddress;
                data[1] = NULL;
                data[2] = NULL;
                linesize[0] = luma_stride;
                linesize[1] = 0;
                linesize[2] = 0;
            }
            enc_in->busLuma = share_fd_buf ? share_vpa : yuv_frame_mem->busAddress;
            enc_in->busChromaU = 0;
            enc_in->busChromaV = 0;
            break;
        default:
            av_log(NULL, AV_LOG_ERROR, "not support format: %d\n", ctx->input_format);
            return -1;
    }

    if (share_fd_buf) {
        // av_log(NULL, AV_LOG_WARNING, "share fd, enc_in->busLuma: 0x%lx, share_vpa: 0x%lx\n", enc_in->busLuma,
        // share_vpa);
        //  check stride
        if (luma_stride != pict->linesize[0]) {
            av_log(NULL,
                   AV_LOG_ERROR,
                   "for share buffer, venc alignment[%d] !=  linesize[%d] failed\n",
                   luma_stride,
                   pict->linesize[0]);
            return -1;
        }
        return 0;
    } else {
        int height = 0;
        for (i = 0; i < FF_ARRAY_ELEMS(data) && data[i]; i++) {
            height = !i ? pict->height : pict->height >> 1;
            if (linesize[i] == pict->linesize[i]) {
                ff_es_codec_memcpy_block(pict->data[i], data[i], pict->linesize[i] * height);
            } else {
                ff_es_codec_memcpy_by_line(pict->data[i], data[i], pict->linesize[i], linesize[i], height);
            }
        }

        if (EWLSyncMemData(yuv_frame_mem, 0, pictureSize, HOST_TO_DEVICE) != EWL_OK) {
            av_log(NULL, AV_LOG_ERROR, "Sync pictureMem Data fail!\n");
        }
    }

    // dump yuv
    if (ctx->dump_frame_enable) {
        int ret = 0;
        if (ctx->dump_frame_hnd) {
            ret = ff_codec_dump_bytes_to_file((void *)yuv_frame_mem->virtualAddress, pictureSize, ctx->dump_frame_hnd);
            if (ret == ERR_TIMEOUT) {
                av_log(NULL, AV_LOG_INFO, "frame dump timeout\n");
                ff_codec_dump_file_close(&ctx->dump_frame_hnd);
                ctx->dump_frame_enable = 0;
            } else if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "write file error\n");
            }
        } else {
            av_log(NULL, AV_LOG_ERROR, "fp is not inited\n");
        }
    }

    return ret;
}

/*    Callback function called by the encoder SW after "slice ready"
    interrupt from HW. Note that this function is not necessarily called
    after every slice i.e. it is possible that two or more slices are
    completed between callbacks.
------------------------------------------------------------------------------*/
static void vsv_slice_ready(VCEncSliceReady *slice) {
    uint32_t i;
    uint32_t stream_size;
    uint32_t pos;
    SliceCtl *ctl = (SliceCtl *)slice->pAppData;
    AVPacket *out_pkt;
    /* Here is possible to implement low-latency streaming by
     * sending the complete slices before the whole frame is completed. */
    if (ctl->multislice_encoding && (ENCH2_SLICE_READY_INTERRUPT)) {
        pos = slice->slicesReadyPrev ? ctl->stream_pos : /* Here we store the slice pointer */
                  0;                                     /* Pointer to beginning of frame */
        stream_size = 0;
        for (i = slice->nalUnitInfoNumPrev; i < slice->nalUnitInfoNum; i++) {
            stream_size += *(slice->sliceSizes + i);
        }

        out_pkt = av_packet_alloc();
        if (out_pkt == NULL) return;
        av_init_packet(out_pkt);

        if (av_packet_from_data(out_pkt, (uint8_t *)&slice->streamBufs, stream_size)) return;

        pos += stream_size;
        /* Store the slice pointer for next callback */
        ctl->stream_pos = pos;
    }
}

static int vsv_encode_send_fd_by_avpacket(AVPacket *pkt, int64_t mem_fd) {
    if (!pkt) {
        av_log(NULL, AV_LOG_ERROR, "vsv_encode_send_fd_by_avpacket, invalid pointers\n");
        return -1;
    }

    // dma fd
    uint8_t *buf = av_packet_new_side_data(pkt, SIDE_DATA_TYPE_MEM_FRAME_FD_RELEASE, sizeof(mem_fd));
    int64_t *fd = (int64_t *)buf;
    *fd = mem_fd;

    av_log(NULL, AV_LOG_INFO, "encoded one frame, release pkt with dma fd[%lx]\n", mem_fd);

    return 0;
}

static int vsv_encode_release_input_buffer(ESEncVidContext *enc_ctx, VCEncOut *enc_out, AVPacket *avpkt) {
    if (!enc_ctx || !enc_out || !avpkt) return -1;

    ESEncVidInternalContext *in_ctx = &enc_ctx->in_ctx;
    unsigned long vir_addr = enc_out->consumedAddr.inputbufBusAddr;
    MemInfo *mem_info = ff_get_mem_info_by_vpa(in_ctx, vir_addr);

    if (in_ctx->share_fd_buf && mem_info) {
        // compare vpa to find the fd
        // av_log(NULL, AV_LOG_WARNING, "release share fd, enc_out->consumedAddr.inputbufBusAddr: 0x%lx\n",
        // enc_out->consumedAddr.inputbufBusAddr);

        // unref dma buf
#ifdef SUPPORT_DMA_HEAP
        EWLPutIovaByFd(in_ctx->ewl, mem_info->dma_fd);
#endif
        // fill fd to avpacket side data
        vsv_encode_send_fd_by_avpacket(avpkt, mem_info->dma_fd);
    } else {
        ff_release_input_picture_buffer(in_ctx, vir_addr);
    }

    ff_release_input_roi_qp_map_buffer(in_ctx, enc_out->consumedAddr.roiMapDeltaQpBusAddr);

    if (mem_info) {
        // update avpkt pts & dts
        avpkt->pts = mem_info->frame->pts;
        avpkt->dts = ff_get_and_del_min_dts_from_queue(in_ctx);
        // remove from queue
        ff_remove_mem_info_from_queue(in_ctx, mem_info);
    }

    return 0;
}

static int vsv_encode_flush(AVCodecContext *avctx, AVPacket *avpkt, int *stream_size) {
    ESEncVidContext *ctx = (ESEncVidContext *)avctx->priv_data;
    int32_t ret = OK;
    ESEncVidInternalContext *in_ctx = (ESEncVidInternalContext *)&ctx->in_ctx;
    VCEncIn *enc_in = (VCEncIn *)&(in_ctx->enc_in);
    VCEncOut *enc_out = (VCEncOut *)&ctx->encOut;
    /* IO buffer */
    ff_setup_slice_ctl(in_ctx);
    ff_get_output_buffer(in_ctx, enc_in);
    ret = VCEncFlush(ctx->encoder, enc_in, enc_out, &vsv_slice_ready, in_ctx->slice_ctl);
    switch (ret) {
        case VCENC_FRAME_READY:
            if (enc_out->streamSize == 0) {
                if (enc_out->codingType != VCENC_NOTCODED_FRAME) {
                    in_ctx->picture_encoded_cnt++;
                }
                break;
            }
            vsv_process_frame(avctx, avpkt, stream_size);
            break;
        default:
            break;
    }

    vsv_encode_release_input_buffer(ctx, enc_out, avpkt);

    av_log(avctx,
           AV_LOG_INFO,
           "flush done, nal_type = %d, size = %d, ret =  %d, pts = %ld, dts = %ld\n",
           enc_out->codingType,
           enc_out->streamSize,
           ret,
           avpkt->pts,
           avpkt->dts);

    return ret;
}

static int vsv_encode_end(AVCodecContext *avctx, AVPacket *avpkt, int *stream_size, int bneed_end) {
    ESEncVidContext *ctx = (ESEncVidContext *)avctx->priv_data;
    int32_t ret = OK;
    ESEncVidInternalContext *in_ctx = (ESEncVidInternalContext *)&ctx->in_ctx;
    VCEncIn *enc_in = (VCEncIn *)&(in_ctx->enc_in);
    VCEncOut *enc_out = (VCEncOut *)&ctx->encOut;

    if (ctx->encoder_is_end) return 0;

    ff_get_output_buffer(in_ctx, enc_in);

    ret = VCEncStrmEnd(ctx->encoder, enc_in, enc_out);
    av_log(NULL, AV_LOG_DEBUG, "VCEncStrmEnd, streamSize = %d, ret = %d\n", enc_out->streamSize, ret);
    if (bneed_end == 1 && ret == VCENC_OK) {
        int32_t core_idx = (in_ctx->picture_enc_cnt - 1 - (in_ctx->frame_delay - 1)) % in_ctx->parallel_core_num;
        int32_t i_buf;
        EWLLinearMem_t mem;
        for (i_buf = 0; i_buf < in_ctx->stream_buf_num; i_buf++)
            in_ctx->outbuf_mem[i_buf] = &(in_ctx->outbuf_mem_factory[core_idx][i_buf]);

        mem = *in_ctx->outbuf_mem[0];

        // av_log(NULL, AV_LOG_DEBUG, "%s %d encOut->streamSize = %d\n", __FILE__, __LINE__, encOut->streamSize);

        if (av_new_packet(avpkt, enc_out->streamSize)) return -1;

        memcpy(avpkt->data, (uint8_t *)mem.virtualAddress, enc_out->streamSize);

        *stream_size = enc_out->streamSize;

        vsv_encode_release_input_buffer(ctx, enc_out, avpkt);
    }

    return 0;
}

static int vsv_encode_config_roi_areas(ESEncVidContext *options, VCEncCodingCtrl *coding_cfg) {
    if (!options || !coding_cfg) {
        av_log(NULL, AV_LOG_ERROR, "vsv_encode_config_roi_areas, invalid pointer\n");
        return -1;
    }

    venc_print_roi(&options->roi_tbl);

    for (int index = 0; index < options->roi_tbl.num_of_roi; index++) {
        RoiAttr *roi_attr = &options->roi_tbl.roi_attr[index];

        if (!roi_attr->enable) continue;

        switch (roi_attr->index) {
            case 0:
                coding_cfg->roi1Area.left = RESOLUTION_TO_CTB(roi_attr->x, options->max_cu_size);
                coding_cfg->roi1Area.top = RESOLUTION_TO_CTB(roi_attr->y, options->max_cu_size);
                coding_cfg->roi1Area.right = RESOLUTION_TO_CTB((roi_attr->x + roi_attr->width), options->max_cu_size);
                coding_cfg->roi1Area.bottom = RESOLUTION_TO_CTB((roi_attr->y + roi_attr->height), options->max_cu_size);
                if (roi_attr->is_absQp) {
                    // absolute QP
                    coding_cfg->roi1DeltaQp = 0;
                    coding_cfg->roi1Qp = roi_attr->qp;
                } else {
                    // relative QP
                    coding_cfg->roi1DeltaQp = roi_attr->qp;
                    coding_cfg->roi1Qp = -1;
                }

                if (vsv_check_area(&coding_cfg->roi1Area, options)
                    && (coding_cfg->roi1DeltaQp || (coding_cfg->roi1Qp >= 0)))
                    coding_cfg->roi1Area.enable = 1;
                else {
                    coding_cfg->roi1Area.enable = 0;
                    av_log(NULL, AV_LOG_WARNING, "roi1 is illegal, force disable\n");
                }
                break;
            case 1:
                coding_cfg->roi2Area.left = RESOLUTION_TO_CTB(roi_attr->x, options->max_cu_size);
                coding_cfg->roi2Area.top = RESOLUTION_TO_CTB(roi_attr->y, options->max_cu_size);
                coding_cfg->roi2Area.right = RESOLUTION_TO_CTB((roi_attr->x + roi_attr->width), options->max_cu_size);
                coding_cfg->roi2Area.bottom = RESOLUTION_TO_CTB((roi_attr->y + roi_attr->height), options->max_cu_size);
                if (roi_attr->is_absQp) {
                    // absolute QP
                    coding_cfg->roi2DeltaQp = 0;
                    coding_cfg->roi2Qp = roi_attr->qp;
                } else {
                    // relative QP
                    coding_cfg->roi2DeltaQp = roi_attr->qp;
                    coding_cfg->roi2Qp = -1;
                }

                if (vsv_check_area(&coding_cfg->roi2Area, options)
                    && (coding_cfg->roi2DeltaQp || (coding_cfg->roi2Qp >= 0)))
                    coding_cfg->roi2Area.enable = 1;
                else {
                    coding_cfg->roi2Area.enable = 0;
                    av_log(NULL, AV_LOG_WARNING, "roi2 is illegal, force disable\n");
                }
                break;
            case 2:
                coding_cfg->roi3Area.left = RESOLUTION_TO_CTB(roi_attr->x, options->max_cu_size);
                coding_cfg->roi3Area.top = RESOLUTION_TO_CTB(roi_attr->y, options->max_cu_size);
                coding_cfg->roi3Area.right = RESOLUTION_TO_CTB((roi_attr->x + roi_attr->width), options->max_cu_size);
                coding_cfg->roi3Area.bottom = RESOLUTION_TO_CTB((roi_attr->y + roi_attr->height), options->max_cu_size);
                if (roi_attr->is_absQp) {
                    // absolute QP
                    coding_cfg->roi3DeltaQp = 0;
                    coding_cfg->roi3Qp = roi_attr->qp;
                } else {
                    // relative QP
                    coding_cfg->roi3DeltaQp = roi_attr->qp;
                    coding_cfg->roi3Qp = -1;
                }

                if (vsv_check_area(&coding_cfg->roi3Area, options)
                    && (coding_cfg->roi3DeltaQp || (coding_cfg->roi3Qp >= 0)))
                    coding_cfg->roi3Area.enable = 1;
                else {
                    coding_cfg->roi3Area.enable = 0;
                    av_log(NULL, AV_LOG_WARNING, "roi3 is illegal, force disable\n");
                }
                break;
            case 3:
                coding_cfg->roi4Area.left = RESOLUTION_TO_CTB(roi_attr->x, options->max_cu_size);
                coding_cfg->roi4Area.top = RESOLUTION_TO_CTB(roi_attr->y, options->max_cu_size);
                coding_cfg->roi4Area.right = RESOLUTION_TO_CTB((roi_attr->x + roi_attr->width), options->max_cu_size);
                coding_cfg->roi4Area.bottom = RESOLUTION_TO_CTB((roi_attr->y + roi_attr->height), options->max_cu_size);
                if (roi_attr->is_absQp) {
                    // absolute QP
                    coding_cfg->roi4DeltaQp = 0;
                    coding_cfg->roi4Qp = roi_attr->qp;
                } else {
                    // relative QP
                    coding_cfg->roi4DeltaQp = roi_attr->qp;
                    coding_cfg->roi4Qp = -1;
                }

                if (vsv_check_area(&coding_cfg->roi4Area, options)
                    && (coding_cfg->roi4DeltaQp || (coding_cfg->roi4Qp >= 0)))
                    coding_cfg->roi4Area.enable = 1;
                else {
                    coding_cfg->roi4Area.enable = 0;
                    av_log(NULL, AV_LOG_WARNING, "roi4 is illegal, force disable\n");
                }
                break;
            case 4:
                coding_cfg->roi5Area.left = RESOLUTION_TO_CTB(roi_attr->x, options->max_cu_size);
                coding_cfg->roi5Area.top = RESOLUTION_TO_CTB(roi_attr->y, options->max_cu_size);
                coding_cfg->roi5Area.right = RESOLUTION_TO_CTB((roi_attr->x + roi_attr->width), options->max_cu_size);
                coding_cfg->roi5Area.bottom = RESOLUTION_TO_CTB((roi_attr->y + roi_attr->height), options->max_cu_size);
                if (roi_attr->is_absQp) {
                    // absolute QP
                    coding_cfg->roi5DeltaQp = 0;
                    coding_cfg->roi5Qp = roi_attr->qp;
                } else {
                    // relative QP
                    coding_cfg->roi5DeltaQp = roi_attr->qp;
                    coding_cfg->roi5Qp = -1;
                }

                if (vsv_check_area(&coding_cfg->roi5Area, options)
                    && (coding_cfg->roi5DeltaQp || (coding_cfg->roi5Qp >= 0)))
                    coding_cfg->roi5Area.enable = 1;
                else {
                    coding_cfg->roi5Area.enable = 0;
                    av_log(NULL, AV_LOG_WARNING, "roi5 is illegal, force disable\n");
                }
                break;
            case 5:
                coding_cfg->roi6Area.left = RESOLUTION_TO_CTB(roi_attr->x, options->max_cu_size);
                coding_cfg->roi6Area.top = RESOLUTION_TO_CTB(roi_attr->y, options->max_cu_size);
                coding_cfg->roi6Area.right = RESOLUTION_TO_CTB((roi_attr->x + roi_attr->width), options->max_cu_size);
                coding_cfg->roi6Area.bottom = RESOLUTION_TO_CTB((roi_attr->y + roi_attr->height), options->max_cu_size);
                if (roi_attr->is_absQp) {
                    // absolute QP
                    coding_cfg->roi6DeltaQp = 0;
                    coding_cfg->roi6Qp = roi_attr->qp;
                } else {
                    // relative QP
                    coding_cfg->roi6DeltaQp = roi_attr->qp;
                    coding_cfg->roi6Qp = -1;
                }

                if (vsv_check_area(&coding_cfg->roi6Area, options)
                    && (coding_cfg->roi6DeltaQp || (coding_cfg->roi6Qp >= 0)))
                    coding_cfg->roi6Area.enable = 1;
                else {
                    coding_cfg->roi6Area.enable = 0;
                    av_log(NULL, AV_LOG_WARNING, "roi6 is illegal, force disable\n");
                }
                break;
            case 6:
                coding_cfg->roi7Area.left = RESOLUTION_TO_CTB(roi_attr->x, options->max_cu_size);
                coding_cfg->roi7Area.top = RESOLUTION_TO_CTB(roi_attr->y, options->max_cu_size);
                coding_cfg->roi7Area.right = RESOLUTION_TO_CTB((roi_attr->x + roi_attr->width), options->max_cu_size);
                coding_cfg->roi7Area.bottom = RESOLUTION_TO_CTB((roi_attr->y + roi_attr->height), options->max_cu_size);
                if (roi_attr->is_absQp) {
                    // absolute QP
                    coding_cfg->roi7DeltaQp = 0;
                    coding_cfg->roi7Qp = roi_attr->qp;
                } else {
                    // relative QP
                    coding_cfg->roi7DeltaQp = roi_attr->qp;
                    coding_cfg->roi7Qp = -1;
                }

                if (vsv_check_area(&coding_cfg->roi7Area, options)
                    && (coding_cfg->roi7DeltaQp || (coding_cfg->roi7Qp >= 0)))
                    coding_cfg->roi7Area.enable = 1;
                else {
                    coding_cfg->roi7Area.enable = 0;
                    av_log(NULL, AV_LOG_WARNING, "roi7 is illegal, force disable\n");
                }
                break;
            case 7:
                coding_cfg->roi8Area.left = RESOLUTION_TO_CTB(roi_attr->x, options->max_cu_size);
                coding_cfg->roi8Area.top = RESOLUTION_TO_CTB(roi_attr->y, options->max_cu_size);
                coding_cfg->roi8Area.right = RESOLUTION_TO_CTB((roi_attr->x + roi_attr->width), options->max_cu_size);
                coding_cfg->roi8Area.bottom = RESOLUTION_TO_CTB((roi_attr->y + roi_attr->height), options->max_cu_size);
                if (roi_attr->is_absQp) {
                    // absolute QP
                    coding_cfg->roi8DeltaQp = 0;
                    coding_cfg->roi8Qp = roi_attr->qp;
                } else {
                    // relative QP
                    coding_cfg->roi8DeltaQp = roi_attr->qp;
                    coding_cfg->roi8Qp = -1;
                }

                if (vsv_check_area(&coding_cfg->roi8Area, options)
                    && (coding_cfg->roi8DeltaQp || (coding_cfg->roi8Qp >= 0)))
                    coding_cfg->roi8Area.enable = 1;
                else {
                    coding_cfg->roi8Area.enable = 0;
                    av_log(NULL, AV_LOG_WARNING, "roi8 is illegal, force disable\n");
                }
                break;
            default:
                av_log(NULL, AV_LOG_ERROR, "roi index = %d is overflow\n", roi_attr->index);
                break;
        }
    }
    return 0;
}

static int vsv_encode_config_ipcm_areas(ESEncVidContext *options, VCEncCodingCtrl *coding_cfg) {
    if (!options || !coding_cfg) {
        av_log(NULL, AV_LOG_ERROR, "vsv_encode_config_ipcm_areas, invalid pointer\n");
        return -1;
    }

    venc_print_ipcm(&options->ipcm_tbl);

    for (int index = 0; index < options->ipcm_tbl.num_of_ipcm; index++) {
        IpcmAttr *ipcm_attr = &options->ipcm_tbl.ipcm_attr[index];

        if (!ipcm_attr->enable) continue;

        switch (ipcm_attr->index) {
            case 0:
                coding_cfg->ipcm1Area.left = RESOLUTION_TO_CTB(ipcm_attr->x, options->max_cu_size);
                coding_cfg->ipcm1Area.top = RESOLUTION_TO_CTB(ipcm_attr->y, options->max_cu_size);
                coding_cfg->ipcm1Area.right =
                    RESOLUTION_TO_CTB((ipcm_attr->x + ipcm_attr->width), options->max_cu_size);
                coding_cfg->ipcm1Area.bottom =
                    RESOLUTION_TO_CTB((ipcm_attr->y + ipcm_attr->height), options->max_cu_size);

                if (vsv_check_area(&coding_cfg->ipcm1Area, options))
                    coding_cfg->ipcm1Area.enable = 1;
                else {
                    coding_cfg->ipcm1Area.enable = 0;
                    av_log(NULL, AV_LOG_WARNING, "ipcm1 is illegal, force disable\n");
                }
                break;
            case 1:
                coding_cfg->ipcm2Area.left = RESOLUTION_TO_CTB(ipcm_attr->x, options->max_cu_size);
                coding_cfg->ipcm2Area.top = RESOLUTION_TO_CTB(ipcm_attr->y, options->max_cu_size);
                coding_cfg->ipcm2Area.right =
                    RESOLUTION_TO_CTB((ipcm_attr->x + ipcm_attr->width), options->max_cu_size);
                coding_cfg->ipcm2Area.bottom =
                    RESOLUTION_TO_CTB((ipcm_attr->y + ipcm_attr->height), options->max_cu_size);

                if (vsv_check_area(&coding_cfg->ipcm2Area, options))
                    coding_cfg->ipcm2Area.enable = 1;
                else {
                    coding_cfg->ipcm2Area.enable = 0;
                    av_log(NULL, AV_LOG_WARNING, "ipcm2 is illegal, force disable\n");
                }
                break;
            case 2:
                coding_cfg->ipcm3Area.left = RESOLUTION_TO_CTB(ipcm_attr->x, options->max_cu_size);
                coding_cfg->ipcm3Area.top = RESOLUTION_TO_CTB(ipcm_attr->y, options->max_cu_size);
                coding_cfg->ipcm3Area.right =
                    RESOLUTION_TO_CTB((ipcm_attr->x + ipcm_attr->width), options->max_cu_size);
                coding_cfg->ipcm3Area.bottom =
                    RESOLUTION_TO_CTB((ipcm_attr->y + ipcm_attr->height), options->max_cu_size);

                if (vsv_check_area(&coding_cfg->ipcm3Area, options))
                    coding_cfg->ipcm3Area.enable = 1;
                else {
                    coding_cfg->ipcm3Area.enable = 0;
                    av_log(NULL, AV_LOG_WARNING, "ipcm3 is illegal, force disable\n");
                }
                break;
            case 3:
                coding_cfg->ipcm4Area.left = RESOLUTION_TO_CTB(ipcm_attr->x, options->max_cu_size);
                coding_cfg->ipcm4Area.top = RESOLUTION_TO_CTB(ipcm_attr->y, options->max_cu_size);
                coding_cfg->ipcm4Area.right =
                    RESOLUTION_TO_CTB((ipcm_attr->x + ipcm_attr->width), options->max_cu_size);
                coding_cfg->ipcm4Area.bottom =
                    RESOLUTION_TO_CTB((ipcm_attr->y + ipcm_attr->height), options->max_cu_size);

                if (vsv_check_area(&coding_cfg->ipcm4Area, options))
                    coding_cfg->ipcm4Area.enable = 1;
                else {
                    coding_cfg->ipcm4Area.enable = 0;
                    av_log(NULL, AV_LOG_WARNING, "ipcm4 is illegal, force disable\n");
                }
                break;
            case 4:
                coding_cfg->ipcm5Area.left = RESOLUTION_TO_CTB(ipcm_attr->x, options->max_cu_size);
                coding_cfg->ipcm5Area.top = RESOLUTION_TO_CTB(ipcm_attr->y, options->max_cu_size);
                coding_cfg->ipcm5Area.right =
                    RESOLUTION_TO_CTB((ipcm_attr->x + ipcm_attr->width), options->max_cu_size);
                coding_cfg->ipcm5Area.bottom =
                    RESOLUTION_TO_CTB((ipcm_attr->y + ipcm_attr->height), options->max_cu_size);

                if (vsv_check_area(&coding_cfg->ipcm5Area, options))
                    coding_cfg->ipcm5Area.enable = 1;
                else {
                    coding_cfg->ipcm5Area.enable = 0;
                    av_log(NULL, AV_LOG_WARNING, "ipcm5 is illegal, force disable\n");
                }
                break;
            case 5:
                coding_cfg->ipcm6Area.left = RESOLUTION_TO_CTB(ipcm_attr->x, options->max_cu_size);
                coding_cfg->ipcm6Area.top = RESOLUTION_TO_CTB(ipcm_attr->y, options->max_cu_size);
                coding_cfg->ipcm6Area.right =
                    RESOLUTION_TO_CTB((ipcm_attr->x + ipcm_attr->width), options->max_cu_size);
                coding_cfg->ipcm6Area.bottom =
                    RESOLUTION_TO_CTB((ipcm_attr->y + ipcm_attr->height), options->max_cu_size);

                if (vsv_check_area(&coding_cfg->ipcm6Area, options))
                    coding_cfg->ipcm6Area.enable = 1;
                else {
                    coding_cfg->ipcm6Area.enable = 0;
                    av_log(NULL, AV_LOG_WARNING, "ipcm6 is illegal, force disable\n");
                }
                break;
            case 6:
                coding_cfg->ipcm7Area.left = RESOLUTION_TO_CTB(ipcm_attr->x, options->max_cu_size);
                coding_cfg->ipcm7Area.top = RESOLUTION_TO_CTB(ipcm_attr->y, options->max_cu_size);
                coding_cfg->ipcm7Area.right =
                    RESOLUTION_TO_CTB((ipcm_attr->x + ipcm_attr->width), options->max_cu_size);
                coding_cfg->ipcm7Area.bottom =
                    RESOLUTION_TO_CTB((ipcm_attr->y + ipcm_attr->height), options->max_cu_size);

                if (vsv_check_area(&coding_cfg->ipcm7Area, options))
                    coding_cfg->ipcm7Area.enable = 1;
                else {
                    coding_cfg->ipcm7Area.enable = 0;
                    av_log(NULL, AV_LOG_WARNING, "ipcm7 is illegal, force disable\n");
                }
                break;
            case 7:
                coding_cfg->ipcm8Area.left = RESOLUTION_TO_CTB(ipcm_attr->x, options->max_cu_size);
                coding_cfg->ipcm8Area.top = RESOLUTION_TO_CTB(ipcm_attr->y, options->max_cu_size);
                coding_cfg->ipcm8Area.right =
                    RESOLUTION_TO_CTB((ipcm_attr->x + ipcm_attr->width), options->max_cu_size);
                coding_cfg->ipcm8Area.bottom =
                    RESOLUTION_TO_CTB((ipcm_attr->y + ipcm_attr->height), options->max_cu_size);

                if (vsv_check_area(&coding_cfg->ipcm8Area, options))
                    coding_cfg->ipcm8Area.enable = 1;
                else {
                    coding_cfg->ipcm8Area.enable = 0;
                    av_log(NULL, AV_LOG_WARNING, "ipcm8 is illegal, force disable\n");
                }
                break;
            default:
                av_log(NULL, AV_LOG_ERROR, "ipcm index = %d is overflow\n", ipcm_attr->index);
                break;
        }
    }
    coding_cfg->pcm_enabled_flag =
        (coding_cfg->ipcm1Area.enable || coding_cfg->ipcm2Area.enable
         || coding_cfg->ipcm3Area.enable | coding_cfg->ipcm4Area.enable || coding_cfg->ipcm5Area.enable
         || coding_cfg->ipcm6Area.enable || coding_cfg->ipcm7Area.enable | coding_cfg->ipcm8Area.enable);
    return 0;
}

static int esenc_vid_init_ewl_inst(ESEncVidInternalContext *in_ctx) {
    EWLInitParam_t param;
    void *ewl_inst = NULL;

    param.context = NULL;
    param.clientType = EWL_CLIENT_TYPE_MEM;  // buffer operation
    param.slice_idx = 0;
    if ((in_ctx->ewl = EWLInit(&param)) == NULL) {
        av_log(NULL, AV_LOG_ERROR, "EWLInit failed.\n");
        return -1;
    }

    return 0;
}

/*------------------------------------------------------------------------------

    OpenEncoder
        Create and configure an encoder instance.

    Params:
        options     - processed comand line options
        pEnc    - place where to save the new encoder instance
    Return:
        0   - for success
        -1  - error

------------------------------------------------------------------------------*/
static int vsv_encode_open_encoder(ESEncVidContext *options, VCEncInst *enc, ESEncVidInternalContext *in_ctx) {
    VCEncRet ret = -1;
    VCEncConfig cfg;
    VCEncCodingCtrl coding_cfg;
    VCEncRateCtrl rc_cfg;
    VCEncPreProcessingCfg pre_proc_cfg;
    VCEncInst encoder = NULL;
    int32_t i;
    // EWLInitParam_t param;

    // init parameters.
    memset(&cfg, 0, sizeof(VCEncConfig));
    memset(&coding_cfg, 0, sizeof(VCEncCodingCtrl));
    memset(&rc_cfg, 0, sizeof(VCEncRateCtrl));
    memset(&pre_proc_cfg, 0, sizeof(VCEncPreProcessingCfg));

    ff_change_cml_customized_format(options);

    /*cfg.ctb_size = options->max_cu_size;*/
    if (options->rotation && options->rotation != 3) {
        cfg.width = options->height;
        cfg.height = options->width;
    } else {
        cfg.width = options->width;
        cfg.height = options->height;
    }

    cfg.frameRateDenom = options->output_rate_denom;
    cfg.frameRateNum = options->output_rate_numer;

    /* intra tools in sps and pps */
    cfg.strongIntraSmoothing = options->strong_intra_smoothing_enabled_flag;

    cfg.streamType = (options->byte_stream) ? VCENC_BYTE_STREAM : VCENC_NAL_UNIT_STREAM;

    cfg.profile = options->profile;
    cfg.level = (VCEncLevel)options->level;
    cfg.tier = (VCEncTier)options->tier;

    cfg.codecFormat = options->codec_format;
    cfg.gopSize = options->gop_size;

    cfg.bitDepthLuma = cfg.bitDepthChroma = options->bitdepth;
    // just support 8bit/10bit, luma and chroma must have same bit depth
    if ((cfg.bitDepthLuma != 8 && cfg.bitDepthLuma != 10) || (cfg.bitDepthChroma != 8 && cfg.bitDepthChroma != 10)
        || cfg.bitDepthLuma != cfg.bitDepthChroma) {
        goto error_exit;
    }

    if ((options->interlaced_frame && options->gop_size != 1) || IS_H264(options->codec_format)) {
        options->interlaced_frame = 0;
    }

    // DEFAULT maxTLayer
    if (options->max_TLayers > options->gop_size) {
        av_log(NULL,
               AV_LOG_WARNING,
               "max_TLayers: %d is overflow, force to %d\n",
               options->max_TLayers,
               options->gop_size);
        cfg.maxTLayers = options->gop_size;
    } else {
        cfg.maxTLayers = options->max_TLayers;
    }

    /* Find the max number of reference frame */
    if (options->intra_pic_rate == 1) {
        cfg.refFrameAmount = 0;
    } else {
        uint32_t maxRefPics = 0;
        uint32_t maxTemporalId = 0;
        int idx;
        for (idx = 0; idx < in_ctx->enc_in.gopConfig.size; idx++) {
            VCEncGopPicConfig *gop_cfg = &(in_ctx->enc_in.gopConfig.pGopPicCfg[idx]);
            if (gop_cfg->codingType != VCENC_INTRA_FRAME) {
                if (maxRefPics < gop_cfg->numRefPics) maxRefPics = gop_cfg->numRefPics;
                // config TID according maxTLayers
                gop_cfg->temporalId = idx % cfg.maxTLayers;
                // if (maxTemporalId < gop_cfg->temporalId) maxTemporalId = gop_cfg>temporalId;
            }
            av_log(NULL,
                   AV_LOG_INFO,
                   "gop size: %d, maxTLayers: %d, codingType: %d, temporalId: %d\n",
                   in_ctx->enc_in.gopConfig.size,
                   cfg.maxTLayers,
                   gop_cfg->codingType,
                   gop_cfg->temporalId);
        }
        cfg.refFrameAmount = maxRefPics + options->interlaced_frame + in_ctx->enc_in.gopConfig.ltrcnt;
        // cfg.maxTLayers = maxTemporalId + 1;
    }

    cfg.compressor = options->compressor;
    av_log(NULL, AV_LOG_DEBUG, "%s cfg.compressor = %d\n", __FUNCTION__, cfg.compressor);

    cfg.interlacedFrame = options->interlaced_frame;
    cfg.enableOutputCuInfo = (options->enable_output_cu_info > 0) ? 1 : 0;
    cfg.rdoLevel = CLIP3(1, 3, options->rdo_level) - 1;
    cfg.verbose = options->verbose;
    cfg.exp_of_input_alignment = options->exp_of_input_alignment;
    cfg.exp_of_ref_alignment = options->exp_of_ref_alignment;
    cfg.exp_of_ref_ch_alignment = options->exp_of_ref_ch_alignment;
    cfg.exp_of_aqinfo_alignment = cfg.exp_of_ref_ch_alignment;
    cfg.exteralReconAlloc = 0;
    cfg.P010RefEnable = options->p010_ref_enable;
    cfg.enableSsim = options->ssim;
    cfg.ctbRcMode = (options->ctb_rc != DEFAULT) ? options->ctb_rc : 0;
    cfg.parallelCoreNum = options->parallel_core_num;
    cfg.pass = (options->lookahead_depth ? 2 : 0);
    cfg.bPass1AdaptiveGop = (options->gop_size == 0);
    cfg.picOrderCntType = options->pic_order_cnt_type;
    cfg.dumpRegister = options->dump_register;
    cfg.rasterscan = options->rasterscan;
    cfg.log2MaxPicOrderCntLsb = options->log2_max_pic_order_cnt_lsb;
    cfg.log2MaxFrameNum = options->log2_max_frame_num;
    cfg.lookaheadDepth = options->lookahead_depth;
    cfg.extDSRatio = (options->lookahead_depth ? 1 : 0);
    if (options->parallel_core_num > 1 && cfg.width * cfg.height < 256 * 256) {
        cfg.parallelCoreNum = options->parallel_core_num = 1;
    }
    cfg.codedChromaIdc = VCENC_CHROMA_IDC_420;
    cfg.tune = VCENC_TUNE_PSNR;
    cfg.cuInfoVersion = -1;
#if 0
    cfg.extSramLumHeightBwd = IS_H264(options->codec_format) ? 12 : (IS_HEVC(options->codec_format) ? 16 : 0);
    cfg.extSramChrHeightBwd = IS_H264(options->codec_format) ? 6 : (IS_HEVC(options->codec_format) ? 8 : 0);
    cfg.extSramLumHeightFwd = IS_H264(options->codec_format) ? 12 : (IS_HEVC(options->codec_format) ? 16 : 0);
    cfg.extSramChrHeightFwd = IS_H264(options->codec_format) ? 6 : (IS_HEVC(options->codec_format) ? 8 : 0);

    cfg.AXIAlignment = 0;
    cfg.irqTypeMask = 0x01f0;
    cfg.irqTypeCutreeMask = 0x01f0;

    cfg.TxTypeSearchEnable = 0;
    cfg.av1InterFiltSwitch = 1;
    cfg.burstMaxLength = ENCH2_DEFAULT_BURST_LENGTH;
    cfg.enableTMVP = 0;
    cfg.bIOBufferBinding = 0;
#endif
    cfg.writeReconToDDR = 1;
    cfg.enablePsnr = 1;

    if ((ret = VCEncInit(&cfg, enc, NULL)) != VCENC_OK) {
        av_log(NULL, AV_LOG_ERROR, "VCEncInit fail\n");
        //        encoder = *enc;
        goto error_exit;
    }
    encoder = *enc;

    av_log(NULL, AV_LOG_DEBUG, "VCEncInit OK\n");

    if (esenc_vid_init_ewl_inst(&options->in_ctx)) {
        return -1;
    }

    /* Encoder setup: coding control */
    if ((ret = VCEncGetCodingCtrl(encoder, &coding_cfg)) != VCENC_OK) {
        av_log(NULL, AV_LOG_ERROR, "VCEncGetCodingCtrl failed\n");
        goto error_exit;
    } else {
        av_log(NULL,
               AV_LOG_INFO,
               "GetCodingCtrl, sliceSize %2d,sei %2d,disable-deblocking %2d,enableCabac %2d,enableSao "
               "%2d\n",
               coding_cfg.sliceSize,
               coding_cfg.seiMessages,
               coding_cfg.disableDeblockingFilter,
               coding_cfg.enableCabac,
               coding_cfg.enableSao);

        if (options->slice_size != DEFAULT) coding_cfg.sliceSize = options->slice_size;
        if (IS_H264(options->codec_format)) {
            coding_cfg.enableCabac = options->enable_cabac;
        }

        coding_cfg.disableDeblockingFilter = !options->enable_deblocking;
        coding_cfg.tc_Offset = options->tc_offset;
        coding_cfg.beta_Offset = options->beta_offset;
        if (IS_HEVC(options->codec_format)) {
            coding_cfg.enableSao = options->enable_sao;
        }
        coding_cfg.enableDeblockOverride = options->enable_deblock_override;
        coding_cfg.deblockOverride = options->deblock_override;

        coding_cfg.seiMessages = options->enable_sei;

        coding_cfg.gdrDuration = options->gdr_duration;
        coding_cfg.fieldOrder = options->field_order;

        coding_cfg.cirStart = options->cir_start;
        coding_cfg.cirInterval = options->cir_interval;

        if (coding_cfg.gdrDuration == 0) {
            coding_cfg.intraArea.top = options->intra_area_top;
            coding_cfg.intraArea.left = options->intra_area_left;
            coding_cfg.intraArea.bottom = options->intra_area_bottom;
            coding_cfg.intraArea.right = options->intra_area_right;
            coding_cfg.intraArea.enable = vsv_check_area(&coding_cfg.intraArea, options);
        } else {
            // intraArea will be used by GDR, customer can not use intraArea when GDR is enabled.
            coding_cfg.intraArea.enable = 0;
        }

        coding_cfg.pcm_loop_filter_disabled_flag = options->pcm_loop_filter_disabled_flag;

        vsv_encode_config_ipcm_areas(options, &coding_cfg);

        // roi cfg
        vsv_encode_config_roi_areas(options, &coding_cfg);

        if (coding_cfg.cirInterval)
            av_log(NULL, AV_LOG_INFO, "  CIR: %d %d\n", coding_cfg.cirStart, coding_cfg.cirInterval);

        if (coding_cfg.intraArea.enable)
            av_log(NULL,
                   AV_LOG_INFO,
                   "  IntraArea: %dx%d-%dx%d\n",
                   coding_cfg.intraArea.left,
                   coding_cfg.intraArea.top,
                   coding_cfg.intraArea.right,
                   coding_cfg.intraArea.bottom);

        if (coding_cfg.ipcm1Area.enable)
            av_log(NULL,
                   AV_LOG_INFO,
                   "  IPCM1Area: %dx%d-%dx%d\n",
                   coding_cfg.ipcm1Area.left,
                   coding_cfg.ipcm1Area.top,
                   coding_cfg.ipcm1Area.right,
                   coding_cfg.ipcm1Area.bottom);

        if (coding_cfg.ipcm2Area.enable)
            av_log(NULL,
                   AV_LOG_INFO,
                   "  IPCM2Area: %dx%d-%dx%d\n",
                   coding_cfg.ipcm2Area.left,
                   coding_cfg.ipcm2Area.top,
                   coding_cfg.ipcm2Area.right,
                   coding_cfg.ipcm2Area.bottom);

        if (coding_cfg.ipcm3Area.enable)
            av_log(NULL,
                   AV_LOG_INFO,
                   "  IPCM3Area: %dx%d-%dx%d\n",
                   coding_cfg.ipcm3Area.left,
                   coding_cfg.ipcm3Area.top,
                   coding_cfg.ipcm3Area.right,
                   coding_cfg.ipcm3Area.bottom);

        if (coding_cfg.ipcm4Area.enable)
            av_log(NULL,
                   AV_LOG_INFO,
                   "  IPCM4Area: %dx%d-%dx%d\n",
                   coding_cfg.ipcm4Area.left,
                   coding_cfg.ipcm4Area.top,
                   coding_cfg.ipcm4Area.right,
                   coding_cfg.ipcm4Area.bottom);

        if (coding_cfg.ipcm5Area.enable)
            av_log(NULL,
                   AV_LOG_INFO,
                   "  IPCM5Area: %dx%d-%dx%d\n",
                   coding_cfg.ipcm5Area.left,
                   coding_cfg.ipcm5Area.top,
                   coding_cfg.ipcm5Area.right,
                   coding_cfg.ipcm5Area.bottom);

        if (coding_cfg.ipcm6Area.enable)
            av_log(NULL,
                   AV_LOG_INFO,
                   "  IPCM6Area: %dx%d-%dx%d\n",
                   coding_cfg.ipcm6Area.left,
                   coding_cfg.ipcm6Area.top,
                   coding_cfg.ipcm6Area.right,
                   coding_cfg.ipcm6Area.bottom);

        if (coding_cfg.ipcm7Area.enable)
            av_log(NULL,
                   AV_LOG_INFO,
                   "  IPCM7Area: %dx%d-%dx%d\n",
                   coding_cfg.ipcm7Area.left,
                   coding_cfg.ipcm7Area.top,
                   coding_cfg.ipcm7Area.right,
                   coding_cfg.ipcm7Area.bottom);

        if (coding_cfg.ipcm8Area.enable)
            av_log(NULL,
                   AV_LOG_INFO,
                   "  IPCM8Area: %dx%d-%dx%d\n",
                   coding_cfg.ipcm8Area.left,
                   coding_cfg.ipcm8Area.top,
                   coding_cfg.ipcm8Area.right,
                   coding_cfg.ipcm8Area.bottom);

        if (coding_cfg.roi1Area.enable)
            av_log(NULL,
                   AV_LOG_INFO,
                   "  ROI 1: %s %d  %dx%d-%dx%d\n",
                   coding_cfg.roi1Qp >= 0 ? "QP" : "QP Delta",
                   coding_cfg.roi1Qp >= 0 ? coding_cfg.roi1Qp : coding_cfg.roi1DeltaQp,
                   coding_cfg.roi1Area.left,
                   coding_cfg.roi1Area.top,
                   coding_cfg.roi1Area.right,
                   coding_cfg.roi1Area.bottom);

        if (coding_cfg.roi2Area.enable)
            av_log(NULL,
                   AV_LOG_INFO,
                   "  ROI 2: %s %d  %dx%d-%dx%d\n",
                   coding_cfg.roi2Qp >= 0 ? "QP" : "QP Delta",
                   coding_cfg.roi2Qp >= 0 ? coding_cfg.roi2Qp : coding_cfg.roi2DeltaQp,
                   coding_cfg.roi2Area.left,
                   coding_cfg.roi2Area.top,
                   coding_cfg.roi2Area.right,
                   coding_cfg.roi2Area.bottom);

        if (coding_cfg.roi3Area.enable)
            av_log(NULL,
                   AV_LOG_INFO,
                   "  ROI 3: %s %d  %dx%d-%dx%d\n",
                   coding_cfg.roi3Qp >= 0 ? "QP" : "QP Delta",
                   coding_cfg.roi3Qp >= 0 ? coding_cfg.roi3Qp : coding_cfg.roi3DeltaQp,
                   coding_cfg.roi3Area.left,
                   coding_cfg.roi3Area.top,
                   coding_cfg.roi3Area.right,
                   coding_cfg.roi3Area.bottom);

        if (coding_cfg.roi4Area.enable)
            av_log(NULL,
                   AV_LOG_INFO,
                   "  ROI 4: %s %d  %dx%d-%dx%d\n",
                   coding_cfg.roi4Qp >= 0 ? "QP" : "QP Delta",
                   coding_cfg.roi4Qp >= 0 ? coding_cfg.roi4Qp : coding_cfg.roi4DeltaQp,
                   coding_cfg.roi4Area.left,
                   coding_cfg.roi4Area.top,
                   coding_cfg.roi4Area.right,
                   coding_cfg.roi4Area.bottom);

        if (coding_cfg.roi5Area.enable)
            av_log(NULL,
                   AV_LOG_INFO,
                   "  ROI 5: %s %d  %dx%d-%dx%d\n",
                   coding_cfg.roi5Qp >= 0 ? "QP" : "QP Delta",
                   coding_cfg.roi5Qp >= 0 ? coding_cfg.roi5Qp : coding_cfg.roi5DeltaQp,
                   coding_cfg.roi5Area.left,
                   coding_cfg.roi5Area.top,
                   coding_cfg.roi5Area.right,
                   coding_cfg.roi5Area.bottom);

        if (coding_cfg.roi6Area.enable)
            av_log(NULL,
                   AV_LOG_INFO,
                   "  ROI 6: %s %d  %dx%d-%dx%d\n",
                   coding_cfg.roi6Qp >= 0 ? "QP" : "QP Delta",
                   coding_cfg.roi6Qp >= 0 ? coding_cfg.roi6Qp : coding_cfg.roi6DeltaQp,
                   coding_cfg.roi6Area.left,
                   coding_cfg.roi6Area.top,
                   coding_cfg.roi6Area.right,
                   coding_cfg.roi6Area.bottom);

        if (coding_cfg.roi7Area.enable)
            av_log(NULL,
                   AV_LOG_INFO,
                   "  ROI 7: %s %d  %dx%d-%dx%d\n",
                   coding_cfg.roi7Qp >= 0 ? "QP" : "QP Delta",
                   coding_cfg.roi7Qp >= 0 ? coding_cfg.roi7Qp : coding_cfg.roi7DeltaQp,
                   coding_cfg.roi7Area.left,
                   coding_cfg.roi7Area.top,
                   coding_cfg.roi7Area.right,
                   coding_cfg.roi7Area.bottom);

        if (coding_cfg.roi8Area.enable)
            av_log(NULL,
                   AV_LOG_INFO,
                   "  ROI 8: %s %d  %dx%d-%dx%d\n",
                   coding_cfg.roi8Qp >= 0 ? "QP" : "QP Delta",
                   coding_cfg.roi8Qp >= 0 ? coding_cfg.roi8Qp : coding_cfg.roi8DeltaQp,
                   coding_cfg.roi8Area.left,
                   coding_cfg.roi8Area.top,
                   coding_cfg.roi8Area.right,
                   coding_cfg.roi8Area.bottom);

        coding_cfg.roiMapDeltaQpEnable = options->roi_map_delta_qp_enable;
        coding_cfg.roiMapDeltaQpBlockUnit = options->roi_map_delta_qp_block_unit;

        coding_cfg.RoimapCuCtrl_index_enable = (options->roimap_cu_ctrl_index_bin_file != NULL);
        coding_cfg.RoimapCuCtrl_enable = (options->roimap_cu_ctrl_info_bin_file != NULL);
        // coding_cfg.roiMapDeltaQpEnable = (options->roi_map_info_bin_file != NULL);
        coding_cfg.RoimapCuCtrl_ver = options->roi_cu_ctrl_ver;
        coding_cfg.RoiQpDelta_ver = options->roi_qp_delta_ver;

        /* SKIP map */
        coding_cfg.skipMapEnable = options->skip_map_enable;

        coding_cfg.enableScalingList = options->enable_scaling_list;
        coding_cfg.chroma_qp_offset = options->chroma_qp_offset;

        /* low latency */
        coding_cfg.inputLineBufEn = (options->input_line_buf_mode > 0) ? 1 : 0;
        coding_cfg.inputLineBufLoopBackEn =
            (options->input_line_buf_mode == 1 || options->input_line_buf_mode == 2) ? 1 : 0;
        if (options->input_line_buf_depth != DEFAULT) coding_cfg.inputLineBufDepth = options->input_line_buf_depth;
        coding_cfg.amountPerLoopBack = options->amount_per_loop_back;
        coding_cfg.inputLineBufHwModeEn =
            (options->input_line_buf_mode == 2 || options->input_line_buf_mode == 4) ? 1 : 0;
        coding_cfg.inputLineBufCbFunc = VCEncInputLineBufDone;
        coding_cfg.inputLineBufCbData = &(in_ctx->input_ctb_line_buf);

        /*stream multi-segment*/
        coding_cfg.streamMultiSegmentMode = options->stream_multi_segment_mode;
        coding_cfg.streamMultiSegmentAmount = options->stream_multi_segment_amount;
        coding_cfg.streamMultiSegCbFunc = &EncStreamSegmentReady;
        coding_cfg.streamMultiSegCbData = &(in_ctx->stream_seg_ctl);

        /* tile */
        coding_cfg.tiles_enabled_flag = options->tiles_enabled_flag && !IS_H264(options->codec_format);
        coding_cfg.num_tile_columns = options->num_tile_columns;
        coding_cfg.num_tile_rows = options->num_tile_rows;
        coding_cfg.loop_filter_across_tiles_enabled_flag = options->loop_filter_across_tiles_enabled_flag;

        /* HDR10 */
        coding_cfg.Hdr10Display.hdr10_display_enable = options->hdr10_display.hdr10_display_enable;
        if (options->hdr10_display.hdr10_display_enable) {
            coding_cfg.Hdr10Display.hdr10_dx0 = options->hdr10_display.hdr10_dx0;
            coding_cfg.Hdr10Display.hdr10_dy0 = options->hdr10_display.hdr10_dy0;
            coding_cfg.Hdr10Display.hdr10_dx1 = options->hdr10_display.hdr10_dx1;
            coding_cfg.Hdr10Display.hdr10_dy1 = options->hdr10_display.hdr10_dy1;
            coding_cfg.Hdr10Display.hdr10_dx2 = options->hdr10_display.hdr10_dx2;
            coding_cfg.Hdr10Display.hdr10_dy2 = options->hdr10_display.hdr10_dy2;
            coding_cfg.Hdr10Display.hdr10_wx = options->hdr10_display.hdr10_wx;
            coding_cfg.Hdr10Display.hdr10_wy = options->hdr10_display.hdr10_wy;
            coding_cfg.Hdr10Display.hdr10_maxluma = options->hdr10_display.hdr10_maxluma;
            coding_cfg.Hdr10Display.hdr10_minluma = options->hdr10_display.hdr10_minluma;
        }

        coding_cfg.Hdr10LightLevel.hdr10_lightlevel_enable = options->hdr10_light.hdr10_lightlevel_enable;
        if (options->hdr10_light.hdr10_lightlevel_enable) {
            coding_cfg.Hdr10LightLevel.hdr10_maxlight = options->hdr10_light.hdr10_maxlight;
            coding_cfg.Hdr10LightLevel.hdr10_avglight = options->hdr10_light.hdr10_avglight;
        }

        if (coding_cfg.Hdr10Display.hdr10_display_enable)
            av_log(NULL,
                   AV_LOG_INFO,
                   "HDR10 Display: dx0=%d, dy0=%d, dx1=%d, dy1=%d, dx2=%d, dy2=%d, wx=%d, wy=%d, maxluma=%d, "
                   "minluma=%d .\n",
                   coding_cfg.Hdr10Display.hdr10_dx0,
                   coding_cfg.Hdr10Display.hdr10_dy0,
                   coding_cfg.Hdr10Display.hdr10_dx1,
                   coding_cfg.Hdr10Display.hdr10_dy1,
                   coding_cfg.Hdr10Display.hdr10_dx2,
                   coding_cfg.Hdr10Display.hdr10_dy2,
                   coding_cfg.Hdr10Display.hdr10_wx,
                   coding_cfg.Hdr10Display.hdr10_wy,
                   coding_cfg.Hdr10Display.hdr10_maxluma,
                   coding_cfg.Hdr10Display.hdr10_minluma);

        if (coding_cfg.Hdr10LightLevel.hdr10_lightlevel_enable)
            av_log(NULL,
                   AV_LOG_INFO,
                   "HDR10 Light: maxlight=%d, avglight=%d .\n",
                   coding_cfg.Hdr10LightLevel.hdr10_maxlight,
                   coding_cfg.Hdr10LightLevel.hdr10_avglight);

        coding_cfg.vuiColorDescription.vuiColorDescripPresentFlag = options->hdr10_color.hdr10_color_enable;
        coding_cfg.vuiVideoSignalTypePresentFlag = coding_cfg.vuiColorDescription.vuiColorDescripPresentFlag;
        if (options->hdr10_color.hdr10_color_enable) {
            coding_cfg.vuiColorDescription.vuiMatrixCoefficients = options->hdr10_color.hdr10_matrix;
            coding_cfg.vuiColorDescription.vuiColorPrimaries = options->hdr10_color.hdr10_primary;

            if (options->hdr10_color.hdr10_transfer == 1)
                coding_cfg.vuiColorDescription.vuiTransferCharacteristics = VCENC_HDR10_ST2084;
            else if (options->hdr10_color.hdr10_transfer == 2)
                coding_cfg.vuiColorDescription.vuiTransferCharacteristics = VCENC_HDR10_STDB67;
            else
                coding_cfg.vuiColorDescription.vuiTransferCharacteristics = VCENC_HDR10_BT2020;
        }

        if (coding_cfg.vuiColorDescription.vuiColorDescripPresentFlag)
            av_log(NULL,
                   AV_LOG_INFO,
                   "HDR10 Color: primary=%d, transfer=%d, matrix=%d .\n",
                   coding_cfg.vuiColorDescription.vuiColorPrimaries,
                   coding_cfg.vuiColorDescription.vuiMatrixCoefficients,
                   coding_cfg.vuiColorDescription.vuiTransferCharacteristics);

        coding_cfg.RpsInSliceHeader = options->rps_in_slice_header;

        av_log(NULL,
               AV_LOG_INFO,
               "SetCodingCtrl, sliceSize %2d,sei %2d,disable-deblocking %2d,enableCabac %2d,enableSao "
               "%2d roiMapDeltaQpEnable %2d roiMapDeltaQpBlockUnit %2d\n",
               coding_cfg.sliceSize,
               coding_cfg.seiMessages,
               coding_cfg.disableDeblockingFilter,
               coding_cfg.enableCabac,
               coding_cfg.enableSao,
               coding_cfg.roiMapDeltaQpEnable,
               coding_cfg.roiMapDeltaQpBlockUnit);

        if ((ret = VCEncSetCodingCtrl(encoder, &coding_cfg)) != VCENC_OK) {
            av_log(NULL, AV_LOG_ERROR, "VCEncSetCodingCtrl failed\n");
            goto error_exit;
        }
    }

    /* Encoder setup: rate control */
    if ((ret = VCEncGetRateCtrl(encoder, &rc_cfg)) != VCENC_OK) {
        av_log(NULL, AV_LOG_ERROR, "VCEncGetRateCtrl failed\n");
        goto error_exit;
    } else {
        av_log(NULL,
               AV_LOG_INFO,
               "Get rate control: qp %2d qpRange I[%2d, %2d] PB[%2d, %2d] %8d bps  "
               "pic %d skip %d  hrd %d  cpbSize %d bitrateWindow %d "
               "intraQpDelta %2d rcMode %2d\n",
               rc_cfg.qpHdr,
               rc_cfg.qpMinI,
               rc_cfg.qpMaxI,
               rc_cfg.qpMinPB,
               rc_cfg.qpMaxPB,
               rc_cfg.bitPerSecond,
               rc_cfg.pictureRc,
               rc_cfg.pictureSkip,
               rc_cfg.hrd,
               rc_cfg.hrdCpbSize,
               rc_cfg.bitrateWindow,
               rc_cfg.intraQpDelta,
               rc_cfg.rcMode);

        // do not modify qpHdr
        if (options->qp_hdr != DEFAULT)
            rc_cfg.qpHdr = options->qp_hdr;
        else
            rc_cfg.qpHdr = -1;
        rc_cfg.blockRCSize = 0;
        rc_cfg.rcQpDeltaRange = 10;
        rc_cfg.rcBaseMBComplexity = 15;
        rc_cfg.monitorFrames =
            (options->output_rate_numer + options->output_rate_denom - 1) / options->output_rate_denom;
        options->monitor_frames =
            (options->output_rate_numer + options->output_rate_denom - 1) / options->output_rate_denom;

        rc_cfg.u32StaticSceneIbitPercent = options->u32_static_scene_ibit_percent;

        // RC setting
        rc_cfg.rcMode = options->rc_mode;
        switch (rc_cfg.rcMode) {
            case VCE_RC_CVBR:
                rc_cfg.bitrateWindow = options->bitrate_window;
                rc_cfg.qpMinI = rc_cfg.qpMinPB = options->qp_min;
                rc_cfg.qpMaxI = rc_cfg.qpMaxPB = options->qp_max;
                if (options->bit_per_second != DEFAULT) {
                    rc_cfg.bitPerSecond = options->bit_per_second;
                }
                break;
            case VCE_RC_CBR:
                rc_cfg.bitrateWindow = options->bitrate_window;
                if (options->bit_per_second != DEFAULT) {
                    rc_cfg.bitPerSecond = options->bit_per_second;
                }
                rc_cfg.hrdCpbSize = 2 * rc_cfg.bitPerSecond;
                break;
            case VCE_RC_VBR:
                rc_cfg.bitrateWindow = options->bitrate_window;
                rc_cfg.qpMinI = rc_cfg.qpMinPB = options->qp_min;
                if (options->bit_per_second != DEFAULT) {
                    rc_cfg.bitPerSecond = options->bit_per_second;
                }
                break;
            case VCE_RC_ABR:
                if (options->bit_per_second != DEFAULT) {
                    rc_cfg.bitPerSecond = options->bit_per_second;
                }
                break;
            case VCE_RC_CQP:
                rc_cfg.fixedIntraQp = options->fixed_qp_I;
                // fixing, how to set B/P
                // rc_cfg.fixedIntraQp = options->fixed_qp_P;
                // rc_cfg.fixedIntraQp = options->fixed_qp_B;
                break;
            default:
                av_log(NULL, AV_LOG_ERROR, "This version is not support CRF\n");
                ret = -1;
                goto error_exit;
                break;
        }

        av_log(NULL,
               AV_LOG_INFO,
               "Set rate control: qp %2d qpRange I[%2d, %2d] PB[%2d, %2d] %9d bps  "
               "pic %d skip %d  hrd %d"
               "  cpbSize %d bitrateWindow %d intraQpDelta %2d "
               "fixedIntraQp %2d rcMode %2d\n",
               rc_cfg.qpHdr,
               rc_cfg.qpMinI,
               rc_cfg.qpMaxI,
               rc_cfg.qpMinPB,
               rc_cfg.qpMaxPB,
               rc_cfg.bitPerSecond,
               rc_cfg.pictureRc,
               rc_cfg.pictureSkip,
               rc_cfg.hrd,
               rc_cfg.hrdCpbSize,
               rc_cfg.bitrateWindow,
               rc_cfg.intraQpDelta,
               rc_cfg.fixedIntraQp,
               rc_cfg.rcMode);

        if ((ret = VCEncSetRateCtrl(encoder, &rc_cfg)) != VCENC_OK) {
            av_log(NULL, AV_LOG_ERROR, "VCEncSetRateCtrl failed\n");
            goto error_exit;
        }
    }
    /* PreP setup */
    if ((ret = VCEncGetPreProcessing(encoder, &pre_proc_cfg)) != VCENC_OK) {
        av_log(NULL, AV_LOG_ERROR, "VCEncGetPreProcessing failed\n");
        goto error_exit;
    }

    av_log(NULL,
           AV_LOG_INFO,
           "Get PreP: input %4dx%d : offset %4dx%d : format %d : rotation %d"
           " : mirror %d : cc %d : scaling %d\n",
           pre_proc_cfg.origWidth,
           pre_proc_cfg.origHeight,
           pre_proc_cfg.xOffset,
           pre_proc_cfg.yOffset,
           pre_proc_cfg.inputType,
           pre_proc_cfg.rotation,
           pre_proc_cfg.mirror,
           pre_proc_cfg.colorConversion.type,
           pre_proc_cfg.scaledOutput);

    pre_proc_cfg.inputType = (VCEncPictureType)options->input_format;
    pre_proc_cfg.rotation = (VCEncPictureRotation)options->rotation;
    pre_proc_cfg.mirror = (VCEncPictureMirror)options->mirror;

    pre_proc_cfg.origWidth = options->lum_width_src;
    pre_proc_cfg.origHeight = options->lum_height_src;
    if (options->interlaced_frame) pre_proc_cfg.origHeight /= 2;
    pre_proc_cfg.xOffset = options->hor_offset_src;
    pre_proc_cfg.yOffset = options->ver_offset_src;
    if (options->color_conversion != DEFAULT)
        pre_proc_cfg.colorConversion.type = (VCEncColorConversionType)options->color_conversion;
    if (pre_proc_cfg.colorConversion.type == VCENC_RGBTOYUV_USER_DEFINED) {
        pre_proc_cfg.colorConversion.coeffA = 20000;
        pre_proc_cfg.colorConversion.coeffB = 44000;
        pre_proc_cfg.colorConversion.coeffC = 5000;
        pre_proc_cfg.colorConversion.coeffE = 35000;
        pre_proc_cfg.colorConversion.coeffF = 38000;
        pre_proc_cfg.colorConversion.coeffG = 35000;
        pre_proc_cfg.colorConversion.coeffH = 38000;
        pre_proc_cfg.colorConversion.LumaOffset = 0;
    }

#ifdef USE_OLD_DRV
    pre_proc_cfg.virtualAddressScaledBuff = in_ctx->scaled_picture_mem.virtualAddress;
#endif
    pre_proc_cfg.input_alignment = 1 << options->exp_of_input_alignment;

    av_log(NULL,
           AV_LOG_INFO,
           "Set PreP: input %4dx%d : offset %4dx%d : format %d : rotation %d"
           " : mirror %d : cc %d \n",
           pre_proc_cfg.origWidth,
           pre_proc_cfg.origHeight,
           pre_proc_cfg.xOffset,
           pre_proc_cfg.yOffset,
           pre_proc_cfg.inputType,
           pre_proc_cfg.rotation,
           pre_proc_cfg.mirror,
           pre_proc_cfg.colorConversion.type);

    /* constant chroma control */
    pre_proc_cfg.constChromaEn = options->const_chroma_en;
    if (options->const_cb != DEFAULT) pre_proc_cfg.constCb = options->const_cb;
    if (options->const_cr != DEFAULT) pre_proc_cfg.constCr = options->const_cr;

    ff_change_to_customized_format(options, &pre_proc_cfg);

    ff_change_format_for_fb(in_ctx, options, &pre_proc_cfg);

    if ((ret = VCEncSetPreProcessing(encoder, &pre_proc_cfg)) != VCENC_OK) {
        av_log(NULL, AV_LOG_ERROR, "VCEncSetPreProcessing failed\n");
        goto error_exit;
    }
    ret = 0;

error_exit:
    return ret;
}

/*------------------------------------------------------------------------------

    close_encoder
       Release an encoder insatnce.

   Params:
        encoder - the instance to be released
------------------------------------------------------------------------------*/
static void vsv_encode_close_encoder(VCEncInst encoder, ESEncVidInternalContext *in_ctx) {
    if (in_ctx->ewl) {
        EWLRelease(in_ctx->ewl);
        in_ctx->ewl = NULL;
    }

    if (encoder) {
        if (VCEncRelease(encoder) != VCENC_OK) {
            av_log(NULL, AV_LOG_ERROR, "VCEncRelease failed\n");
        }
    }

    if (in_ctx->two_pass_ewl) {
        EWLRelease(in_ctx->two_pass_ewl);
        in_ctx->two_pass_ewl = NULL;
    }
}

static int venc_get_extradata(AVCodecContext *avctx) {
    if (!avctx) return -1;

    ESEncVidContext *h26xCtx = (ESEncVidContext *)avctx->priv_data;
    uint32_t stream_size = h26xCtx->encOut.streamSize;

    av_log(NULL, AV_LOG_INFO, "venc_get_extradata, steam_size: %d\n", stream_size);
    /* send start data */
    if (stream_size) {
        ESEncVidInternalContext *in_ctx = &h26xCtx->in_ctx;
        // multi-core: output bitstream has (in_ctx->parallel_core_num-1) delay
        int32_t core_idx = (in_ctx->picture_enc_cnt - 1 - (in_ctx->frame_delay - 1)) % in_ctx->parallel_core_num;
        int32_t i_buf;
        EWLLinearMem_t mem;
        for (i_buf = 0; i_buf < in_ctx->stream_buf_num; i_buf++)
            in_ctx->outbuf_mem[i_buf] = &(in_ctx->outbuf_mem_factory[core_idx][i_buf]);

        mem = *in_ctx->outbuf_mem[0];

        if ((avctx->extradata = av_realloc(avctx->extradata, stream_size)) == NULL) return AVERROR(ENOMEM);

        memcpy(avctx->extradata, (uint8_t *)mem.virtualAddress, stream_size);
        avctx->extradata_size = stream_size;
        h26xCtx->encoder_is_start = 1;
    }

    return 0;
}

static int vsv_encode_start(AVCodecContext *avctx) {
    ESEncVidContext *ctx = (ESEncVidContext *)avctx->priv_data;
    ESEncVidContext *options = ctx;
    ESEncVidInternalContext *in_ctx = (ESEncVidInternalContext *)&ctx->in_ctx;
    VCEncIn *enc_in = (VCEncIn *)&(in_ctx->enc_in);
    VCEncOut *enc_out = (VCEncOut *)&ctx->encOut;
    int stream_size = 0;

    int32_t p = 0;
    int cnt = 1;

    uint32_t gop_size = options->gop_size;
    ctx->adaptive_gop = (gop_size == 0);

    memset(&ctx->agop, 0, sizeof(ctx->agop));
    ctx->agop.last_gop_size = MAX_ADAPTIVE_GOP_SIZE;

    // enc_in->gopSize = gop_size;

    ctx->ma.pos = ctx->ma.count = 0;
    ctx->ma.frame_rate_numer = options->output_rate_numer;
    ctx->ma.frame_rate_denom = options->output_rate_denom;
    if (options->output_rate_denom)
        ctx->ma.length = MAX(LEAST_MONITOR_FRAME, MIN(options->monitor_frames, MOVING_AVERAGE_FRAMES));
    else
        ctx->ma.length = MOVING_AVERAGE_FRAMES;

    enc_in->timeIncrement = 0;
    enc_in->vui_timing_info_enable = options->enable_vui;

    ff_get_output_buffer(in_ctx, enc_in);

    enc_in->hashType = options->hashtype;
    ff_init_slice_ctl(in_ctx, options);
    ff_init_stream_segment_ctl(in_ctx, options);

    if (options->input_line_buf_mode) {
        if (vsv_init_input_line_buffer(&(in_ctx->input_ctb_line_buf), options, enc_in, ctx->encoder, in_ctx)) {
            av_log(NULL,
                   AV_LOG_ERROR,
                   "Fail to Init Input Line Buffer: virt_addr=%p, bus_addr=%08x\n",
                   in_ctx->input_ctb_line_buf.sram,
                   (uint32_t)(in_ctx->input_ctb_line_buf.sramBusAddr));
            goto error;
        }
    }

    /* before VCEncStrmStart called */
    vsv_init_pic_config(enc_in);
    /* Video, sequence and picture parameter sets */
    for (p = 0; p < cnt; p++) {
        if (VCEncStrmStart(ctx->encoder, enc_in, enc_out)) {
            av_log(NULL, AV_LOG_ERROR, "VCEncStrmStart failed\n");
            goto error;
        }

        ctx->total_bits += enc_out->streamSize * 8;
        stream_size += enc_out->streamSize;
    }

    venc_get_extradata(avctx);

    enc_in->poc = 0;

    // default gop size as IPPP
    // enc_in->gopSize = ctx->next_gop_size = (ctx->adaptive_gop ? (options->lookahead_depth ? 4 : 1) : gop_size);
    av_log(NULL, AV_LOG_DEBUG, "%s %d pEncIn->gopSize = %d\n", __FILE__, __LINE__, enc_in->gopSize);

    VCEncGetRateCtrl(ctx->encoder, (VCEncRateCtrl *)&ctx->rc);

    /* Allocate a buffer for user data and read data from file */
    ctx->p_user_data = ff_read_user_data(ctx->encoder, options->user_data);

    in_ctx->valid_encoded_frame_number = 0;

    return 0;

error:
    av_log(NULL, AV_LOG_ERROR, "%s error\n", __FUNCTION__);
    return -1;
}

static int vsv_encode_got_mem_fd(const AVFrame *frame, int64_t *mem_fd) {
    if (!frame) return -1;
    if (!frame->nb_side_data) return -1;

    AVFrameSideData *sd = NULL;
    // mem fd
    sd = av_frame_get_side_data(frame, SIDE_DATA_TYPE_MEM_FRAME_FD);
    if (sd) {
        *mem_fd = *((int64_t *)sd->data);
        av_log(NULL, AV_LOG_INFO, "got mem_fd: %lx\n", *mem_fd);
        return 0;
    }

    return -1;
}

static int vsv_encode_encode(AVCodecContext *avctx, AVPacket *avpkt, const AVFrame *pict, int *stream_size) {
    ESEncVidContext *ctx = (ESEncVidContext *)avctx->priv_data;
    ESEncVidContext *options = ctx;
    int32_t ret = OK;
    ESEncVidInternalContext *in_ctx = (ESEncVidInternalContext *)&ctx->in_ctx;
    VCEncIn *enc_in = (VCEncIn *)&(in_ctx->enc_in);
    VCEncOut *enc_out = (VCEncOut *)&ctx->encOut;
    uint32_t i, tmp;
    int64_t mem_fd = 0;
    unsigned long vir_addr = NULL;
    int8_t share_fd_buf = 0;

    av_log(avctx, AV_LOG_DEBUG, "%s in\n", __FUNCTION__);

    if (!vsv_encode_got_mem_fd(pict, &mem_fd)) {
        // get dma buffer virtual addr
#ifdef SUPPORT_DMA_HEAP
        EWLGetIovaByFd(in_ctx->ewl, mem_fd, &vir_addr);
#else
        vir_addr = mem_fd;
#endif
        // av_log(NULL, AV_LOG_WARNING, "dma_buf,mem_fd = 0x%lx, vir_addr = 0x%lx\n", mem_fd, vir_addr);
        // set flag
        share_fd_buf = 1;
        in_ctx->share_fd_buf = share_fd_buf;
    } else {
        if (!in_ctx->picture_buffer_allocated) {
            av_log(avctx, AV_LOG_INFO, "not hwaccel mode, alloc input picture buffers\n");
            if (esenc_vid_alloc_input_picture_buffers(in_ctx)) {
                return -1;
            }
            in_ctx->picture_buffer_allocated = 1;
        }
        /* input picture buffer */
        ff_get_input_picture_buffer(in_ctx);
        vir_addr = (unsigned long)in_ctx->picture_mem->busAddress;
    }

    // roi_qp_map buffer
    ff_get_input_roi_qp_map_buffer(in_ctx);

    // av_log(NULL, AV_LOG_WARNING, "vir_addr = 0x%lx, pict: %p\n", vir_addr, pict);
    //  add mem_fd into fd-vpa list
    av_log(avctx, AV_LOG_INFO, "received avframe, pts = %ld, pkt_dts = %ld\n", pict->pts, pict->pkt_dts);

    MemInfo *mem_info = ff_alloc_and_fill_mem_info(mem_fd, vir_addr, pict);
    ff_push_mem_info_into_queue(in_ctx, mem_info);

    DtsInfo *dts_info = ff_alloc_and_fill_dts_info(pict->pkt_dts);
    ff_push_dts_info_into_queue(in_ctx, dts_info);

    ff_setup_slice_ctl(in_ctx);
    ff_get_output_buffer(in_ctx, enc_in);

    if (esenc_vid_fill_picture_buffer(avctx, pict, share_fd_buf, vir_addr) < 0) {
        av_log(NULL, AV_LOG_ERROR, "esenc_vid_fill_picture_buffer fail\n");
        return -1;
    }

    ctx->frame_cnt_total++;
    enc_in->bIsIDR = options->force_IDR;
    enc_in->sendAUD = options->insert_AUD;

    if (in_ctx->picture_enc_cnt && options->insert_SPS_PPS) {
        enc_in->resendSPS = enc_in->resendPPS = 0;
        if (enc_in->bIsIDR) {
            enc_in->resendSPS = enc_in->resendPPS = options->insert_SPS_PPS;
            in_ctx->last_idr_picture_cnt = in_ctx->picture_enc_cnt;
        } else if (in_ctx->idr_interval) {
            u8 resend = ((in_ctx->picture_enc_cnt - in_ctx->last_idr_picture_cnt) % in_ctx->idr_interval) == 0;
            enc_in->resendSPS = enc_in->resendPPS = resend;
        }
    }

    // av_log(NULL, AV_LOG_INFO, "=== Encoding frame%i...\n", in_ctx->picture_enc_cnt);

    /* 4. SetupROI-Map */
    ff_fill_roi_qp_map_buffer(in_ctx, options, enc_in, ctx->encoder);

    /* 5. encoding specific frame from user: all CU/MB are SKIP*/
    enc_in->bSkipFrame = options->skip_frame_enabled_flag && (enc_in->poc == options->skip_frame_poc);

    /* 6. low latency */
    if (options->input_line_buf_mode) {
        enc_in->lineBufWrCnt = VCEncStartInputLineBuffer(&(in_ctx->input_ctb_line_buf), 1);

        if (in_ctx->input_ctb_line_buf.loopBackEn && in_ctx->input_ctb_line_buf.lumBuf.buf) {
            VCEncPreProcessingCfg preProcCfg;
//[fpga] when loop-back enable, inputCtbLineBuf is used by api, orignal frame buffer is not used inside api.
#if 0
            i32 inputPicBufIndex = FindInputPicBufIdByBusAddr(tb, enc_in->busLuma, options->formatCustomizedType!=-1);
            if (NOK == ReturnBufferById(tb->inputMemFlags, tb->buffer_cnt, inputPicBufIndex) )
            return NOK;
#endif
            enc_in->busLuma = in_ctx->input_ctb_line_buf.lumBuf.busAddress;
            enc_in->busChromaU = in_ctx->input_ctb_line_buf.cbBuf.busAddress;
            enc_in->busChromaV = in_ctx->input_ctb_line_buf.crBuf.busAddress;

            /* In loop back mode, data in line buffer start from the line to be encoded*/
            VCEncGetPreProcessing(ctx->encoder, &preProcCfg);
            preProcCfg.yOffset = 0;
            VCEncSetPreProcessing(ctx->encoder, &preProcCfg);
        }

        in_ctx->input_ctb_line_buf.lumSrc = in_ctx->lum;
        in_ctx->input_ctb_line_buf.cbSrc = in_ctx->cb;
        in_ctx->input_ctb_line_buf.crSrc = in_ctx->cr;
        in_ctx->input_ctb_line_buf.wrCnt = 0;
        enc_in->lineBufWrCnt = VCEncStartInputLineBuffer(&(in_ctx->input_ctb_line_buf), HANTRO_TRUE);
        enc_in->initSegNum = in_ctx->input_ctb_line_buf.initSegNum;
    }

reenc:
    ret = VCEncStrmEncode(ctx->encoder, enc_in, enc_out, &vsv_slice_ready, in_ctx->slice_ctl);
    switch (ret) {
        case VCENC_FRAME_ENQUEUE:
            in_ctx->picture_enc_cnt++;
            enc_in->picture_cnt++;
            enc_in->timeIncrement = in_ctx->output_rate_denom;
            break;
        case VCENC_FRAME_READY:
            if (enc_out->codingType != VCENC_NOTCODED_FRAME) {
                in_ctx->picture_enc_cnt++;
                if (enc_out->streamSize == 0) {
                    in_ctx->picture_encoded_cnt++;
                }
            }

            if (enc_out->streamSize == 0) {
                enc_in->picture_cnt++;
                break;
            }

            if (options->input_line_buf_mode) VCEncUpdateInitSegNum(&(in_ctx->input_ctb_line_buf));

            vsv_process_frame(avctx, avpkt, stream_size);
            enc_in->picture_cnt++;
            if (enc_out->codingType != VCENC_NOTCODED_FRAME) {
                in_ctx->picture_encoded_cnt++;
            }

            if (ctx->p_user_data) {
                /* We want the user data to be written only once so
                 * we disable the user data and free the memory after
                 * first frame has been encoded. */
                VCEncSetSeiUserData(ctx->encoder, NULL, 0);
                free(ctx->p_user_data);
                ctx->p_user_data = NULL;
            }

#ifdef DEBUG_CU_INFO
            VCEncCuInfo cu_info;
            if (VCEncGetCuInfo(ctx->encoder, &enc_out->cuOutData, 0, 0, &cu_info) != VCENC_OK) {
                av_log(NULL, AV_LOG_ERROR, "VCEncGetCuInfo fail\n");
            } else {
                av_log(NULL,
                       AV_LOG_INFO,
                       "cuinfo, X=%d, Y=%d, cuSize=%d, cuMode=%d, qp=%d\n",
                       cu_info.cuLocationX,
                       cu_info.cuLocationY,
                       cu_info.cuSize,
                       cu_info.cuMode,
                       cu_info.qp);
            }
#endif
            break;
        case VCENC_OUTPUT_BUFFER_OVERFLOW:
            av_log(NULL, AV_LOG_WARNING, "VCEncStrmEncode ret = VCENC_OUTPUT_BUFFER_OVERFLOW\n");
            enc_in->picture_cnt++;
            esenc_vid_realloc_output_buffer(in_ctx);
            ff_get_output_buffer(in_ctx, enc_in);
            goto reenc;
            break;
        default:
            av_log(NULL, AV_LOG_ERROR, "VCEncStrmEncode ret is %d\n", ret);
            goto error;
            break;
    }

    vsv_encode_release_input_buffer(ctx, enc_out, avpkt);

    if (options->force_IDR) options->force_IDR = 0;

    av_log(avctx,
           AV_LOG_INFO,
           "encoded done, nal_type = %d, size = %d, ret =  %d, pts = %ld, dts = %ld\n",
           enc_out->codingType,
           enc_out->streamSize,
           ret,
           avpkt->pts,
           avpkt->dts);

    return ret;
error:
    return -1;
}

static av_cold int vsv_encode_open(AVCodecContext *avctx) {
    ESEncVidContext *ctx = (ESEncVidContext *)avctx->priv_data;
    ESEncVidContext *options = ctx;
    int32_t ret = OK;
    ESEncVidInternalContext *in_ctx = (ESEncVidInternalContext *)&ctx->in_ctx;

    if (ctx->encoder_is_open) return 0;

    /* we set vce param according first decoded pic */
    ret = vsv_encode_set_vceparam(avctx);
    if (ret != 0) {
        av_log(NULL, AV_LOG_ERROR, "vsv_encode_set_vceparam error.\n");
        goto error_exit;
    }

    // av_log(avctx, AV_LOG_DEBUG, "+++ [ %s ]vsv_encode_get_options\n", __FUNCTION__);
    // vsv_encode_get_options(avctx);
    if ((ret = vsv_encode_open_encoder(options, (VCEncInst *)&ctx->encoder, in_ctx)) != 0) {
        av_log(NULL, AV_LOG_ERROR, "OpenEncoder failed. ret is %d\n", ret);
        goto error_exit;
    }

    ctx->picture_cnt_bk = 0;
    in_ctx->output_rate_numer = options->output_rate_numer;
    in_ctx->output_rate_denom = options->output_rate_denom;
    in_ctx->width = options->width;
    in_ctx->height = options->height;
    in_ctx->input_alignment = (options->exp_of_input_alignment == 0 ? 0 : (1 << options->exp_of_input_alignment));
    in_ctx->ref_alignment = (options->exp_of_ref_alignment == 0 ? 0 : (1 << options->exp_of_ref_alignment));
    in_ctx->ref_ch_alignment = (options->exp_of_ref_ch_alignment == 0 ? 0 : (1 << options->exp_of_ref_ch_alignment));
    in_ctx->format_customized_type = options->format_customized_type;
    in_ctx->idr_interval = options->intra_pic_rate;
    in_ctx->byte_stream = options->byte_stream;
    in_ctx->interlaced_frame = options->interlaced_frame;
    in_ctx->parallel_core_num = options->parallel_core_num;
    in_ctx->buffer_cnt = VCEncGetEncodeMaxDelay(ctx->encoder) + 1;
    in_ctx->frame_delay = in_ctx->parallel_core_num;
    if (options->lookahead_depth) {
        int32_t delay = CUTREE_BUFFER_CNT(options->lookahead_depth, MAX_GOP_SIZE) - 1;
        in_ctx->frame_delay += MIN(delay, (int64_t)in_ctx->last_pic - in_ctx->first_pic + 1); /* lookahead depth */
        /* consider gop8->gop4 reorder: 8 4 2 1 3 6 5 7 -> 4 2 1 3 8 6 5 7
         * at least 4 more buffers are needed to avoid buffer overwrite in pass1 before consumed in pass2*/
        in_ctx->buffer_cnt = in_ctx->frame_delay + 4;
    }
    av_log(NULL, AV_LOG_DEBUG, "alloc in_ctx->buffer_cnt: %d\n", in_ctx->buffer_cnt);
    in_ctx->enc_in.gopConfig.idr_interval = in_ctx->idr_interval;
    in_ctx->enc_in.gopConfig.firstPic = in_ctx->first_pic;
    in_ctx->enc_in.gopConfig.lastPic = INT_MAX;
    in_ctx->enc_in.gopConfig.outputRateNumer = in_ctx->output_rate_numer; /* Output frame rate numerator */
    in_ctx->enc_in.gopConfig.outputRateDenom = in_ctx->output_rate_denom; /* Output frame rate denominator */
    in_ctx->enc_in.gopConfig.inputRateNumer = in_ctx->enc_in.gopConfig.outputRateNumer; /* Input frame rate numerator */
    in_ctx->enc_in.gopConfig.inputRateDenom =
        in_ctx->enc_in.gopConfig.outputRateDenom; /* Input frame rate denominator */
    in_ctx->enc_in.gopConfig.gopLowdelay = options->gop_lowdelay;
    in_ctx->enc_in.gopConfig.interlacedFrame = in_ctx->interlaced_frame;

    /* Set the test ID for internal testing,
     * the SW must be compiled with testing flags */
    VCEncSetTestId(ctx->encoder, options->test_id);

    /* Allocate input and output buffers */
    if ((ret = vsv_encode_alloc_res(options, ctx->encoder, in_ctx)) != 0) {
        av_log(NULL, AV_LOG_ERROR, "vsv_encode_alloc_res failed.\n");
        goto error_exit;
    }

    /* start the encoding thread */

    ctx->encoder_is_open = 1;
    return 0;

error_exit:
    if (ret != 0) {
        av_log(NULL, AV_LOG_ERROR, "encode init fails, ret is %d\n", ret);
    }

    // Transcode_consume_flush(options->trans_handle, options->internal_enc_index);

    if (ctx->encoder) {
        vsv_encode_free_res(in_ctx);
        vsv_encode_close_encoder(ctx->encoder, in_ctx);
    }

    return ret;
}

static int vsv_init_hwcontext(AVCodecContext *avctx) {
    ESEncVidContext *ctx = (ESEncVidContext *)avctx->priv_data;
    int nb_frames;

    /* hw device & frame init */
    if (avctx->hw_frames_ctx) {
        AVHWFramesContext *hwframe;

        hwframe = (AVHWFramesContext *)avctx->hw_frames_ctx->data;
        ctx->hwdevice = (AVVSVDeviceContext *)((AVHWDeviceContext *)hwframe->device_ref->data)->hwctx;
        if (!ctx->hwdevice) {
            return AVERROR(ENOMEM);
        }
    } else if (avctx->hw_device_ctx) {
        av_log(avctx, AV_LOG_TRACE, "%s(%d) avctx->hw_device_ctx = %p\n", __FUNCTION__, __LINE__, avctx->hw_device_ctx);
        ctx->hwdevice = (AVVSVDeviceContext *)((AVHWDeviceContext *)avctx->hw_device_ctx->data)->hwctx;
        av_log(avctx, AV_LOG_TRACE, "%s(%d) ctx->hwdevice = %p\n", __FUNCTION__, __LINE__, ctx->hwdevice);
        if (!ctx->hwdevice) {
            return AVERROR(ENOMEM);
        }
    } else {
        return AVERROR(EINVAL);
    }

    if (ctx->lookahead_depth) {
        ctx->hwdevice->lookahead = 1;
        nb_frames = 17 + ctx->lookahead_depth;
    } else {
        nb_frames = 8 + 2;
    }

    if (ctx->hwdevice->nb_frames < nb_frames) {
        ctx->hwdevice->nb_frames = nb_frames;
    }

    return 0;
}

av_cold int ff_vsv_h26x_encode_init(AVCodecContext *avctx) {
    ESEncVidContext *ctx = (ESEncVidContext *)avctx->priv_data;
    ESEncVidContext *options = ctx;
    int32_t ret = OK;
    VCEncApiVersion api_ver;
    VCEncBuild encBuild;
    int32_t useVcmd = -1;
    ESEncVidInternalContext *in_ctx = (ESEncVidInternalContext *)&ctx->in_ctx;
    uint32_t client_type;

    av_log(avctx, AV_LOG_DEBUG, "%s in\n", __FUNCTION__);

    /* If you add something that can fail above this av_frame_alloc(),
     * modify ff_vsv_h26x_encode_close() accordingly. */
    options->frame = av_frame_alloc();
    if (!options->frame) {
        return AVERROR(ENOMEM);
    }

    // api version
    api_ver = VCEncGetApiVersion();
    av_log(avctx, AV_LOG_INFO, "VC9000 Encoder API version %d.%d.%d\n", api_ver.major, api_ver.minor, api_ver.clnum);

    // setting log output of SDK
    log_env_setting env_log = {LOG_STDOUT, VCENC_LOG_WARN, 0x003F, 0x0001};
    VCEncLogInit(env_log.out_dir, env_log.out_level, env_log.k_trace_map, env_log.k_check_map);

    if (options->input_line_buf_mode) {
        useVcmd = 0;
    }

    EWLAttach(NULL, 0, useVcmd);
    ret = esenc_init(&ctx->tc, avctx, &ff_vsv_h26x_encode_encode2);
    if (ret < 0) {
        goto error_exit;
    }

    ret = vsv_encode_set_default_opt(avctx);
    if (ret < 0) {
        goto error_exit;
    }

    /* preset params set */
    ret = vsv_preset_params_set(avctx);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "vsv_preset_params_set error, please check your cmd !\n");
    }

    vsv_encode_get_options(avctx);

    // init packet dump handle
    if (options->dump_pkt_enable && !options->dump_pkt_hnd) {
        DumpParas paras;
        paras.width = options->lum_width_src;
        paras.height = options->lum_height_src;
        paras.pic_stride = 0;
        paras.pic_stride_ch = 0;
        paras.prefix_name = "venc";
        if (IS_H264(options->codec_format))
            paras.suffix_name = "h264";
        else
            paras.suffix_name = "h265";
        paras.fmt = NULL;
        options->dump_pkt_hnd = ff_codec_dump_file_open(options->dump_path, options->dump_pkt_time, &paras);
    }

    if (options->dump_frame_enable && !options->dump_frame_hnd) {
        DumpParas paras;
        paras.width = options->width;
        paras.height = options->height;
        paras.pic_stride = 0;
        paras.pic_stride_ch = 0;
        paras.prefix_name = "venc";
        paras.suffix_name = "yuv";
        paras.fmt = ff_vsv_encode_get_fmt_char(avctx->pix_fmt);
        options->dump_frame_hnd = ff_codec_dump_file_open(options->dump_path, options->dump_frame_time, &paras);
    }

#ifndef ESW_FF_ENHANCEMENT
    ret = vsv_init_hwcontext(avctx);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "vsv_init_hwcontext failed.\n");
        goto error_exit;
    }

#ifdef DRV_NEW_ARCH
    options->priority = ctx->hwdevice->priority;
    options->device = ctx->hwdevice->device;
    options->mem_id = ctx->hwdevice->task_id;
#endif
#endif
    memset(in_ctx, 0, sizeof(ESEncVidInternalContext));

    // get roi map version
    in_ctx->roi_map_version = VCEncGetRoiMapVersion(0, NULL);
    av_log(NULL, AV_LOG_INFO, "roi map version: %d\n", in_ctx->roi_map_version);

    client_type = IS_H264(options->codec_format) ? EWL_CLIENT_TYPE_H264_ENC : EWL_CLIENT_TYPE_HEVC_ENC;
    encBuild = VCEncGetBuild(client_type);
    av_log(avctx,
           AV_LOG_INFO,
           "HW ID:  0x%08x\t SW Build: %u.%u.%u\n",
           encBuild.hwBuild,
           encBuild.swBuild / 1000000,
           (encBuild.swBuild / 1000) % 1000,
           encBuild.swBuild % 1000);

    // mem_fd_vpa queue
    in_ctx->in_mem_queue = es_queue_create();
    pthread_mutex_init(&in_ctx->in_mem_queue_mutex, NULL);

    // dts queue
    in_ctx->dts_queue = es_queue_create();
    pthread_mutex_init(&in_ctx->dts_queue_mutex, NULL);

#ifdef FB_SYSLOG_ENABLE
    if (avctx->codec->id == AV_CODEC_ID_HEVC) {
        sprintf(&ctx->module_name[0], "%s", "HEVCENC");
    } else if (avctx->codec->id == AV_CODEC_ID_H264) {
        sprintf(&ctx->module_name[0], "%s", "H264ENC");
    }
    in_ctx->log_header.module_name = &ctx->module_name[0];
#ifdef DRV_NEW_ARCH
    in_ctx->log_header.device_id = get_deviceId(options->device);
#else
    in_ctx->log_header.device_id = 0;
#endif
#endif

    /* the number of output stream buffers */
    in_ctx->stream_buf_num = options->stream_buf_chain ? 2 : 1;
    in_ctx->compress_rate = 10;

    /* get GOP configuration */
    in_ctx->gop_size = MIN(options->gop_size, MAX_GOP_SIZE);
    if (in_ctx->gop_size == 0 && options->gop_lowdelay) {
        in_ctx->gop_size = 4;
    }
    memset(ctx->gop_pic_cfg, 0, sizeof(ctx->gop_pic_cfg));
    in_ctx->enc_in.gopConfig.pGopPicCfg = ctx->gop_pic_cfg;
    memset(ctx->gop_pic_special_cfg, 0, sizeof(ctx->gop_pic_special_cfg));
    in_ctx->enc_in.gopConfig.pGopPicSpecialCfg = ctx->gop_pic_special_cfg;
    if ((ret = ff_init_gop_configs(
             in_ctx->gop_size, options, &(in_ctx->enc_in.gopConfig), in_ctx->enc_in.gopConfig.gopCfgOffset, 0, 0))
        != 0) {
        av_log(NULL, AV_LOG_ERROR, "init_gop_configs failed.\n");
        goto error_exit;
    }
    if (options->lookahead_depth) {
        memset(ctx->gop_pic_cfg_pass2, 0, sizeof(ctx->gop_pic_cfg_pass2));
        in_ctx->enc_in.gopConfig.pGopPicCfg = ctx->gop_pic_cfg_pass2;
        in_ctx->enc_in.gopConfig.size = 0;
        memset(ctx->gop_pic_special_cfg, 0, sizeof(ctx->gop_pic_special_cfg));
        in_ctx->enc_in.gopConfig.pGopPicSpecialCfg = ctx->gop_pic_special_cfg;
        if ((ret = ff_init_gop_configs(
                 in_ctx->gop_size, options, &(in_ctx->enc_in.gopConfig), in_ctx->enc_in.gopConfig.gopCfgOffset, 1, 0))
            != 0) {
            av_log(NULL, AV_LOG_ERROR, "init_gop_configs 2pass failed.\n");
            goto error_exit;
        }
        in_ctx->enc_in.gopConfig.pGopPicCfgPass1 = ctx->gop_pic_cfg;
        in_ctx->enc_in.gopConfig.pGopPicCfg = in_ctx->enc_in.gopConfig.pGopPicCfgPass2 = ctx->gop_pic_cfg_pass2;
    }

    ctx->output_pkt_queue = av_fifo_alloc(MAX_FIFO_DEPTH * sizeof(AVPacket));
    if (!ctx->output_pkt_queue) return AVERROR(ENOMEM);

    ctx->encoder_is_open = 0;
    ctx->encoder_is_start = 0;
    ctx->encoder_is_end = 0;

    ret = vsv_encode_open(avctx);
    if (ret != 0) {
        av_log(NULL, AV_LOG_ERROR, "vsv_encode_open error. ret = %d\n", ret);
        goto error_exit;
    }

    ret = vsv_encode_start(avctx);
    if (ret != 0) {
        av_log(NULL, AV_LOG_ERROR, "ff_vsv_encode_start error. ret = %d\n", ret);
        goto error_exit;
    }
    ctx->encoder_is_start = 1;
    av_log(avctx, AV_LOG_DEBUG, "%s, ret = 0, out\n", __FUNCTION__);
    return 0;

error_exit:
    ff_vsv_h26x_encode_close(avctx);
    av_log(avctx, AV_LOG_DEBUG, "%s, ret = %d, out\n", __FUNCTION__, ret);
    return ret;
}

av_cold int ff_vsv_h26x_encode_close(AVCodecContext *avctx) {
    ESEncVidContext *ctx = (ESEncVidContext *)avctx->priv_data;
    ESEncVidInternalContext *in_ctx = (ESEncVidInternalContext *)&ctx->in_ctx;

    av_log(avctx, AV_LOG_DEBUG, "%s in\n", __FUNCTION__);

    /* We check ctx->frame to know whether encode_init()
     * has been called and va_config/va_context initialized. */
    if (!ctx->frame) return 0;

    if (ctx->dump_frame_hnd) ff_codec_dump_file_close(&ctx->dump_frame_hnd);
    if (ctx->dump_pkt_hnd) ff_codec_dump_file_close(&ctx->dump_pkt_hnd);

    esenc_close(ctx->tc);

    vsv_encode_report(avctx);

    if (ctx->encoder && in_ctx->ewl) {
        vsv_encode_free_res(in_ctx);
        vsv_encode_close_encoder(ctx->encoder, in_ctx);
    }

    if (in_ctx->cu_map_buf) {
        free(in_ctx->cu_map_buf);
        in_ctx->cu_map_buf = NULL;
        in_ctx->cu_map_buf_len = 0;
    }

    av_fifo_freep(&ctx->output_pkt_queue);

    av_frame_free(&ctx->frame);

    // clean mem_info and dts queue
    ff_clean_mem_info_queue(in_ctx);
    ff_clean_dts_queue(in_ctx);
    es_queue_destroy(in_ctx->in_mem_queue);
    es_queue_destroy(in_ctx->dts_queue);
    pthread_mutex_destroy(&in_ctx->dts_queue_mutex);
    pthread_mutex_destroy(&in_ctx->dts_queue_mutex);

    if (avctx->extradata) av_freep(&avctx->extradata);
    av_log(avctx, AV_LOG_DEBUG, "%s out\n", __FUNCTION__);
    return 0;
}

int ff_vsv_h26x_encode_send_frame(AVCodecContext *avctx, const AVFrame *frame) {
    int ret = 0;
    ESEncVidContext *ctx = (ESEncVidContext *)avctx->priv_data;
    av_log(avctx, AV_LOG_DEBUG, "%s,frame = %p in\n", __FUNCTION__, frame);
    ret = esenc_send_frame(ctx->tc, frame);
    av_log(avctx, AV_LOG_DEBUG, "%s out\n", __FUNCTION__);
    return ret;
}

int ff_vsv_h26x_encode_receive_packet(AVCodecContext *avctx, AVPacket *avpkt) {
    ESEncVidContext *ctx = (ESEncVidContext *)avctx->priv_data;
    AVFrame *frame = ctx->frame;
    int err;

    err = ff_encode_get_frame(avctx, frame);
    if (err < 0 && err != AVERROR_EOF) {
        return err;
    }

    if (err == AVERROR_EOF) {
        av_log(avctx, AV_LOG_INFO, "send NULL frame ,eos frame\n", err);
        frame = NULL;
    }

    err = esenc_send_frame(ctx->tc, frame);
    if (err != 0) {
        av_log(avctx, AV_LOG_ERROR, "esenc_send_frame failed res=:%d\n", err);
        return err;
    }
    return esenc_receive_packet(ctx->tc, avpkt);
}

// convert cu map to ctu table
// for example, sdk just receive 8x8 block
// if block unit = 64x64, we need convert every 64x64 to 8*(sdk 8x8 block),
// every (sdk 8x8 block) has the same value
static int vsv_h26x_encode_convert_cu_map_to_ctu_table(
    ESEncVidContext *option, char *buf, int src_qp_delta, int src_seq, int blk_row, int blk_colum) {
    if (!option || !buf) return -1;

    int w = STRIDE_BY_ALIGN(option->width, option->max_cu_size) / 8;
    int h = STRIDE_BY_ALIGN(option->height, option->max_cu_size) / 8;
    int iw = 0, ih = 0;
    int start_x = 0, start_y = 0;
    int end_x = 0, end_y = 0;
    int x = 0, y = 0;
    int w_stride = w % blk_row + w;

    start_x = src_seq * blk_row % w_stride;
    start_y = src_seq * blk_row / w_stride * blk_colum;

    for (ih = 0; ih < blk_colum; ih++) {
        for (iw = 0; iw < blk_colum; iw++) {
            x = start_x + iw;
            y = start_y + ih;

            if (x < w && y < h) {
                buf[y * w + x] = src_qp_delta;
                // av_log(NULL, AV_LOG_DEBUG, "array[%d][%d]= %d, 0x%x\n", y, x, src_qp_delta, src_qp_delta);
            }
        }
    }

    return 0;
}

static void vsv_h26x_encode_print_ctu(int w, int h, char *buf) {
    for (int x = 0; x < h * w; x++) {
        printf("%3d", buf[x]);
        if ((x + 1) % w == 0) printf("\n");
    }
}

static int vsv_h26x_encode_control_dynamic(AVCodecContext *avctx, const AVFrame *frame) {
    if (!avctx) return -1;
    if (!frame) return 0;
    if (!frame->nb_side_data) return 0;

    ESEncVidContext *option = (ESEncVidContext *)avctx->priv_data;
    if (!option) return -1;

    AVFrameSideData *sd = NULL;
    SideDataSliceSize *sd_slice_size = NULL;
    SideDataForeceIdr *sd_force_idr = NULL;
    SideDataInsertSpsPps *sd_insert_sps_pps = NULL;
    SideDataRc *sd_rc = NULL;
    int call_coding_ctl = 0;
    VCEncCodingCtrl coding_cfg;

    // roi area
    sd = av_frame_get_side_data(frame, SIDE_DATA_TYPE_ROI_AREA);
    if (sd) {
        int roi_size = sizeof(RoiAttr);
        RoiAttr *roi = NULL;
        int nb_rois = sd->size / roi_size;

        av_log(NULL, AV_LOG_INFO, "received sd, nb_rois: %d\n", nb_rois);

        if (nb_rois) {
            memset(&option->roi_tbl, 0x00, sizeof(option->roi_tbl));
        }

        for (int i = 0; i < nb_rois; i++) {
            roi = (RoiAttr *)(sd->data + roi_size * i);
            memcpy(&option->roi_tbl.roi_attr[i], roi, roi_size);
            option->roi_tbl.num_of_roi++;
        }
        call_coding_ctl = 1;
    }

    // cu map
    sd = av_frame_get_side_data(frame, SIDE_DATA_TYPE_CU_MAP);
    if (sd) {
        int cu_size = sizeof(SideDataCuMap);
        int nb_cu = sd->size / cu_size;
        SideDataCuMap *cu_map = NULL;
        int buf_allocated = 0;
        int blk_row_num = 0;
        int blk_colum_num = 0;
        int32_t blk_size_w = 0;
        int32_t blk_size_h = 0;
        int32_t insert_blk_cnt = 0;
        int32_t w_align_ctu = STRIDE_BY_ALIGN(option->width, option->max_cu_size);
        int32_t h_align_ctu = STRIDE_BY_ALIGN(option->height, option->max_cu_size);

        av_log(NULL, AV_LOG_INFO, "received sd, cu count: %d\n", nb_cu);

        for (int i = 0; i < nb_cu; i++) {
            int qp_delta = 0;
            cu_map = (SideDataCuMap *)(sd->data + cu_size * i);
            qp_delta = cu_map->qp_value;
            if (cu_map->skip) qp_delta |= 0x80;
            if (!cu_map->is_abs_qp) qp_delta |= 0x40;

            if (!buf_allocated) {
                // caculate size
                if (cu_map->blk_unit == CU_BLK_64x64) {
                    blk_size_w = 64;
                    blk_size_h = 64;
                } else if (cu_map->blk_unit == CU_BLK_32x32) {
                    blk_size_w = 32;
                    blk_size_h = 32;
                } else if (cu_map->blk_unit == CU_BLK_16x16) {
                    blk_size_w = 16;
                    blk_size_h = 16;
                } else if (cu_map->blk_unit == CU_BLK_8x8) {
                    blk_size_w = 8;
                    blk_size_h = 8;
                } else {
                    av_log(NULL, AV_LOG_ERROR, "not support cu map blk_unit: %d\n", cu_map->blk_unit);
                }

                blk_row_num = blk_size_w / 8;
                blk_colum_num = blk_size_h / 8;

                option->in_ctx.cu_map_buf_len = w_align_ctu * h_align_ctu / 64;  // 64 = 8*8
                option->in_ctx.cu_map_buf = calloc(1, option->in_ctx.cu_map_buf_len);
                buf_allocated = 1;
                av_log(NULL, AV_LOG_DEBUG, "cu_map_buf_len %d\n", option->in_ctx.cu_map_buf_len);
            }

            av_log(NULL,
                   AV_LOG_INFO,
                   "cu[%d]: blk_unit=%d, skip=%d, is_abs=%d, qp=%d\n",
                   i,
                   cu_map->blk_unit,
                   cu_map->skip,
                   cu_map->is_abs_qp,
                   cu_map->qp_value);
            // av_log(NULL, AV_LOG_DEBUG, "cu[%d]: %x, %d\n", i, qp_delta, qp_delta);

            vsv_h26x_encode_convert_cu_map_to_ctu_table(
                option, option->in_ctx.cu_map_buf, qp_delta, i + insert_blk_cnt, blk_row_num, blk_colum_num);

            // where to insert the last cu
            int32_t user_w = STRIDE_BY_ALIGN(option->width, blk_size_w);
            int32_t user_h = STRIDE_BY_ALIGN(option->height, blk_size_h);
            int32_t insert_start = user_w / blk_size_w;
            int32_t insert_cnt = (w_align_ctu - user_w) / blk_size_w;

            for (int id = 0; id < insert_cnt; id++) {
                insert_blk_cnt++;
                vsv_h26x_encode_convert_cu_map_to_ctu_table(
                    option, option->in_ctx.cu_map_buf, qp_delta, i + insert_blk_cnt, blk_row_num, blk_colum_num);
            }
        }

        // vsv_h26x_encode_print_ctu(w_align_ctu / 8,
        //                           h_align_ctu / 8,
        //                           option->in_ctx.cu_map_buf);
    }

    // got slice size from side data
    sd = av_frame_get_side_data(frame, SIDE_DATA_TYPE_SLICE_SIZE);
    if (sd) {
        sd_slice_size = (SideDataSliceSize *)sd->data;
        av_log(NULL, AV_LOG_INFO, "received sd, slice_size = %d\n", sd_slice_size->slice_size);
    }

    // got force idr from side data
    sd = av_frame_get_side_data(frame, SIDE_DATA_TYPE_FORCE_IDR);
    if (sd) {
        sd_force_idr = (SideDataForeceIdr *)sd->data;
        av_log(NULL, AV_LOG_INFO, "received sd, force_idr = %d\n", sd_force_idr->force_idr);
    }

    // got slice size from side data
    sd = av_frame_get_side_data(frame, SIDE_DATA_TYPE_INSERT_SPS_PPS);
    if (sd) {
        sd_insert_sps_pps = (SideDataInsertSpsPps *)sd->data;
        av_log(NULL, AV_LOG_INFO, "received sd, insert_sps_pps = %d\n", sd_insert_sps_pps->insert_sps_pps);
    }

    // got slice size from side data
    sd = av_frame_get_side_data(frame, SIDE_DATA_TYPE_RC);
    if (sd) {
        sd_rc = (SideDataRc *)sd->data;
        av_log(NULL,
               AV_LOG_INFO,
               "received sd, rc_mode=%d, rc_window=%d, bitrate=%d, min_qp=%d, max_qp=%d, "
               "fixed_qp_i=%d,fixed_qp_p=%d, fixed_qp_b=%d\n",
               sd_rc->rc_mode,
               sd_rc->rc_window,
               sd_rc->bitrate,
               sd_rc->min_qp,
               sd_rc->max_qp,
               sd_rc->fixed_qp_i,
               sd_rc->fixed_qp_p,
               sd_rc->fixed_qp_b);
    }

    if (sd_force_idr) {
        option->force_IDR = sd_force_idr->force_idr;
    }

    if (sd_insert_sps_pps) {
        option->insert_SPS_PPS = sd_insert_sps_pps->insert_sps_pps;
    }

    if (sd_slice_size) {
        option->slice_size = sd_slice_size->slice_size;
        call_coding_ctl = 1;
    }

    // call VCEncSetCodingCtrl once to set all relative parameters
    if (call_coding_ctl) {
        if (VCEncGetCodingCtrl(option->encoder, &coding_cfg) != VCENC_OK) {
            av_log(NULL, AV_LOG_ERROR, "set slice_size, VCEncGetCodingCtrl fail\n");
            return -1;
        } else {
            // slice size changed
            if (coding_cfg.sliceSize != option->slice_size) {
                av_log(NULL, AV_LOG_INFO, "slice_size, %d---->%d\n", coding_cfg.sliceSize, option->slice_size);
                // slice size
                coding_cfg.sliceSize = option->slice_size;
            }
            // roi
            vsv_encode_config_roi_areas(option, &coding_cfg);

            VCEncStrmEnd(option->encoder, &option->in_ctx.enc_in, &option->encOut);
            if (VCEncSetCodingCtrl(option->encoder, &coding_cfg) != VCENC_OK) {
                av_log(NULL, AV_LOG_ERROR, "dynamic control, VCEncSetCodingCtrl fail\n");
                return -1;
            }

            struct VSVInternalContext *in_ctx = (struct VSVInternalContext *)&option->in_ctx;
            ff_get_output_buffer(in_ctx, &option->in_ctx.enc_in);
            VCEncStrmStart(option->encoder, &option->in_ctx.enc_in, &option->encOut);

            venc_get_extradata(avctx);

            call_coding_ctl = 0;
        }

        av_log(NULL, AV_LOG_INFO, "call VCEncSetCodingCtrl OK\n");
    }

    if (sd_rc) {
        option->rc_mode = sd_rc->rc_mode;
        option->bitrate_window = sd_rc->rc_window;
        option->bit_per_second = sd_rc->bitrate;
        option->qp_min = sd_rc->min_qp;
        option->qp_max = sd_rc->max_qp;
        option->fixed_qp_I = sd_rc->fixed_qp_i;
        option->fixed_qp_P = sd_rc->fixed_qp_p;
        option->fixed_qp_B = sd_rc->fixed_qp_b;

        VCEncRateCtrl rc_cfg;
        if (VCEncGetRateCtrl(option->encoder, &rc_cfg) != VCENC_OK) {
            av_log(NULL, AV_LOG_ERROR, "set rc, VCEncGetRateCtrl fail\n");
            return -1;
        } else {
            av_log(NULL, AV_LOG_INFO, "rc mode, %d---->%d\n", rc_cfg.rcMode, option->rc_mode);
            // RC setting
            rc_cfg.rcMode = option->rc_mode;
            switch (rc_cfg.rcMode) {
                case VCE_RC_CVBR:
                    rc_cfg.bitrateWindow = option->bitrate_window;
                    rc_cfg.qpMinI = rc_cfg.qpMinPB = option->qp_min;
                    rc_cfg.qpMaxI = rc_cfg.qpMaxPB = option->qp_max;
                    if (option->bit_per_second != DEFAULT) {
                        rc_cfg.bitPerSecond = option->bit_per_second;
                    }
                    break;
                case VCE_RC_CBR:
                    rc_cfg.bitrateWindow = option->bitrate_window;
                    if (option->bit_per_second != DEFAULT) {
                        rc_cfg.bitPerSecond = option->bit_per_second;
                    }
                    rc_cfg.hrdCpbSize = 2 * rc_cfg.bitPerSecond;
                    break;
                case VCE_RC_VBR:
                    rc_cfg.bitrateWindow = option->bitrate_window;
                    rc_cfg.qpMinI = rc_cfg.qpMinPB = option->qp_min;
                    if (option->bit_per_second != DEFAULT) {
                        rc_cfg.bitPerSecond = option->bit_per_second;
                    }
                    break;
                case VCE_RC_ABR:
                    if (option->bit_per_second != DEFAULT) {
                        rc_cfg.bitPerSecond = option->bit_per_second;
                    }
                    break;
                case VCE_RC_CQP:
                    rc_cfg.fixedIntraQp = option->fixed_qp_I;
                    // fixing, how to set B/P
                    // rc_cfg.fixedIntraQp = option->fixed_qp_P;
                    // rc_cfg.fixedIntraQp = option->fixed_qp_B;
                    break;
                default:
                    av_log(NULL, AV_LOG_WARNING, "This version is not support CRF\n");
                    return -1;
            }

            // VCEncStrmEnd(option->encoder, &option->in_ctx.enc_in, &option->encOut);
            if (VCEncSetRateCtrl(option->encoder, &rc_cfg) != VCENC_OK) {
                av_log(NULL, AV_LOG_ERROR, "set slice_size, VCEncSetRateCtrl fail\n");
                return -1;
            }
            // VCEncStrmStart(option->encoder, &option->in_ctx.enc_in, &option->encOut);

            av_log(NULL, AV_LOG_INFO, "set rc, VCEncGetRateCtrl OK\n");
        }
    }

    return 0;
}

int ff_vsv_h26x_encode_encode2(AVCodecContext *avctx, AVPacket *avpkt, const AVFrame *frame, int *got_packet) {
    ESEncVidContext *ctx = (ESEncVidContext *)avctx->priv_data;

    int ret = 0;
    int flush_ret = 0;
    int stream_size = 0;

    *got_packet = 0;

    if (frame == NULL) {
        ret = vsv_encode_flush(avctx, avpkt, &stream_size);
        // for encoding error or flush end, end encoder
        if (ret <= 0) {
            ret = vsv_encode_end(avctx, avpkt, &stream_size, 0);
            ctx->encoder_is_end = 1;
            ret = AVERROR_EOF;
        } else {
            // send a NULL frame to myself for continuing call flush to get cache data with encoder
            ff_vsv_h26x_encode_send_frame(avctx, NULL);
        }
    } else {
        vsv_h26x_encode_control_dynamic(avctx, frame);
        if ((ctx->profile == VCENC_HEVC_MAIN_STILL_PICTURE_PROFILE) && (ctx->in_ctx.picture_enc_cnt >= 1)) {
            return AVERROR_EOF;
        }

        ret = vsv_encode_encode(avctx, avpkt, frame, &stream_size);
    }

    if (stream_size) *got_packet = 1;

    return ret;
}
