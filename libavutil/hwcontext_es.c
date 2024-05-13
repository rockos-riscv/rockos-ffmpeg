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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "buffer.h"
#include "common.h"
#include "hwcontext.h"
#include "hwcontext_internal.h"
#include "hwcontext_es.h"
#include "pixfmt.h"
#include "pixdesc.h"
#include "imgutils.h"

#define ES_FRAME_ALIGNMENT 1

struct DWLLinearMem;

static const enum AVPixelFormat supported_formats[] = {
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_NV21,
    AV_PIX_FMT_GRAY8,
    AV_PIX_FMT_P010,
    AV_PIX_FMT_RGB24,
    AV_PIX_FMT_BGR24,
    AV_PIX_FMT_ARGB,
    AV_PIX_FMT_ABGR,
    AV_PIX_FMT_0RGB,
    AV_PIX_FMT_0BGR,
#ifdef ESW_FF_ENHANCEMENT
    AV_PIX_FMT_B16G16R16I,
    AV_PIX_FMT_B16G16R16I_PLANAR,
    AV_PIX_FMT_B8G8R8_PLANAR,
    AV_PIX_FMT_B8G8R8I,
    AV_PIX_FMT_B8G8R8I_PLANAR,
    AV_PIX_FMT_B16G16R16F,
    AV_PIX_FMT_B16G16R16F_PLANAR,
    AV_PIX_FMT_B32G32R32F,
    AV_PIX_FMT_B32G32R32F_PLANAR,
#endif
};

typedef struct {
    int shift_width;
    int shift_height;
} ESFramesContext;

static void es_memcpy_block(void *src, void *dst, size_t data_size) {
    memcpy(dst, src, data_size);
}

static int es_memcpy_by_line(uint8_t *src, uint8_t *dst, int src_linesize, int dst_linesize, int linecount) {
    int copy_size;
    if (!src || !dst) {
        return AVERROR(EINVAL);
    }

    copy_size = FFMIN(src_linesize, dst_linesize);
    for (int i = 0; i < linecount; i++) {
        memcpy(dst + i * dst_linesize, src + i * src_linesize, copy_size);
    }

    return 0;
}

static int es_frames_get_constraints(AVHWDeviceContext *device_ctx,
                                      const void *hwconfig,
                                      AVHWFramesConstraints *constraints) {
    av_log(device_ctx, AV_LOG_TRACE, "%s(%d)\n", __FUNCTION__, __LINE__);

    constraints->valid_sw_formats =
        av_malloc_array(FF_ARRAY_ELEMS(supported_formats) + 1, sizeof(*constraints->valid_sw_formats));
    if (!constraints->valid_sw_formats) {
        return AVERROR(ENOMEM);
    }

    for (int i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++) {
        constraints->valid_sw_formats[i] = supported_formats[i];
    }
    constraints->valid_sw_formats[FF_ARRAY_ELEMS(supported_formats)] = AV_PIX_FMT_NONE;

    constraints->valid_hw_formats = av_malloc_array(2, sizeof(*constraints->valid_hw_formats));
    if (!constraints->valid_hw_formats) {
        return AVERROR(ENOMEM);
    }

    constraints->valid_hw_formats[0] = AV_PIX_FMT_ES;
    constraints->valid_hw_formats[1] = AV_PIX_FMT_NONE;

    return 0;
}

static void es_memory_free(void *opaque, uint8_t *data) {
    AVHWFramesContext *ctx = opaque;
    AVHWDeviceContext *device_ctx = ctx->device_ctx;
    AVVSVDeviceContext *hwctx = device_ctx->hwctx;
    const void *dwl_inst = hwctx->dwl_inst;

#ifndef ESW_FF_ENHANCEMENT
    if (data) {
        DWLFreeLinear(dwl_inst, (struct DWLLinearMem *)data);
        av_free(data);
    }
#endif
}

static AVBufferRef *es_pool_alloc(void *opaque, int size) {
    AVHWFramesContext *ctx = opaque;
    AVHWDeviceContext *device_ctx = ctx->device_ctx;
    AVVSVDeviceContext *hwctx = device_ctx->hwctx;
    const void *dwl_inst = hwctx->dwl_inst;
    AVBufferRef *ret = NULL;
    struct DWLLinearMem *memory = NULL;

#ifndef ESW_FF_ENHANCEMENT
    memory = av_mallocz(sizeof(*memory));
    if (!memory) {
        return NULL;
    }

    memory->mem_type = DWL_MEM_TYPE_DPB;
    if (DWLMallocLinear(dwl_inst, size, memory)) {
        av_free(memory);
        return NULL;
    }
#endif
    ret = av_buffer_create((uint8_t *)memory, size, es_memory_free, ctx, 0);
    if (!ret) {
        es_memory_free(ctx, (uint8_t *)memory);
        av_log(ctx, AV_LOG_ERROR, "dwl malloc failed: size %d\n", size);
    }

    return ret;
}

static int es_frames_init(AVHWFramesContext *hw_frames_ctx) {
    int i;
    ESFramesContext *priv = hw_frames_ctx->internal->priv;

    av_log(hw_frames_ctx, AV_LOG_TRACE, "%s\n", __FUNCTION__);

    for (i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++) {
        if (hw_frames_ctx->sw_format == supported_formats[i]) {
            break;
        }
    }
    if (i == FF_ARRAY_ELEMS(supported_formats)) {
        av_log(hw_frames_ctx,
               AV_LOG_ERROR,
               "Pixel format '%s' is not supported\n",
               av_get_pix_fmt_name(hw_frames_ctx->sw_format));
        return AVERROR(ENOSYS);
    }

    av_pix_fmt_get_chroma_sub_sample(hw_frames_ctx->sw_format, &priv->shift_width, &priv->shift_height);

    if (!hw_frames_ctx->pool) {
        int size = av_image_get_buffer_size(
            hw_frames_ctx->sw_format, hw_frames_ctx->width, hw_frames_ctx->height, ES_FRAME_ALIGNMENT);
        if (size < 0) {
            return size;
        }

        hw_frames_ctx->internal->pool_internal = av_buffer_pool_init2(size, hw_frames_ctx, es_pool_alloc, NULL);
        if (!hw_frames_ctx->internal->pool_internal) {
            return AVERROR(ENOMEM);
        }
    }

    return 0;
}

static int es_frames_get_buffer(AVHWFramesContext *hw_frames_ctx, AVFrame *frame) {
    int res;

    frame->buf[0] = av_buffer_pool_get(hw_frames_ctx->pool);
    if (!frame->buf[0]) {
        return AVERROR(ENOMEM);
    }

    res = av_image_fill_arrays(frame->data,
                               frame->linesize,
                               frame->buf[0]->data,
                               hw_frames_ctx->sw_format,
                               hw_frames_ctx->width,
                               hw_frames_ctx->height,
                               ES_FRAME_ALIGNMENT);
    if (res < 0) {
        return res;
    }

    frame->format = AV_PIX_FMT_ES;
    frame->width = hw_frames_ctx->width;
    frame->height = hw_frames_ctx->height;
    return 0;
}

static int es_transfer_get_formats(AVHWFramesContext *hw_frames_ctx,
                                    enum AVHWFrameTransferDirection dir,
                                    enum AVPixelFormat **formats) {
    enum AVPixelFormat *fmts;

    fmts = av_malloc_array(2, sizeof(*fmts));
    if (!fmts) return AVERROR(ENOMEM);

    fmts[0] = hw_frames_ctx->sw_format;
    fmts[1] = AV_PIX_FMT_NONE;

    *formats = fmts;
    return 0;
}

static int es_transfer_data_from(AVHWFramesContext *hw_frames_ctx, AVFrame *dst, const AVFrame *src) {
    int height;
    if (!src || !dst) {
        av_log(hw_frames_ctx, AV_LOG_INFO, "es_transfer_data_from failed EINVAL\n");
        return AVERROR(EINVAL);
    } else {
        av_log(hw_frames_ctx, AV_LOG_INFO, "src data[0]: %p\n", src->data[0]);
    }

    for (int i = 0; i < FF_ARRAY_ELEMS(src->data) && src->data[i]; i++) {
        height = src->height * (i ? 1.0 / 2 : 1);
        if (src->linesize[i] == dst->linesize[i]) {
            es_memcpy_block(src->data[i], dst->data[i], src->linesize[i] * height);
        } else {
            es_memcpy_by_line(src->data[i], dst->data[i], src->linesize[i], dst->linesize[i], height);
        }
    }

    return 0;
}

static int es_transfer_data_to(AVHWFramesContext *hw_frames_ctx, AVFrame *dst, const AVFrame *src) {
    return 0;
}

static void es_device_uninit(AVHWDeviceContext *device_ctx) {
}

static int es_device_init(AVHWDeviceContext *device_ctx) {
    av_log(device_ctx, AV_LOG_INFO, "%s(%d) device_ctx: %p\n", __FUNCTION__, __LINE__, device_ctx);

    return 0;
}

static int es_device_create(AVHWDeviceContext *device_ctx, const char *device, AVDictionary *opts, int flags) {
    av_log(device_ctx, AV_LOG_INFO, "%s(%d) hwctx = %p\n", __FUNCTION__, __LINE__, device_ctx);

    if (device) {
        av_log(device_ctx, AV_LOG_INFO, "device(%s)\n", device);
    } else {
        av_log(device_ctx, AV_LOG_WARNING, "device null\n");
    }

    return 0;
}

const HWContextType ff_hwcontext_type_es = {
    .type = AV_HWDEVICE_TYPE_ES,
    .name = "ES",

    .device_hwctx_size = sizeof(AVVSVDeviceContext),
    .frames_priv_size = sizeof(AVVSVFramesContext),

    .device_create = es_device_create,
    .device_init = es_device_init,
    .device_uninit = es_device_uninit,
    .frames_get_constraints = es_frames_get_constraints,
    .frames_init = es_frames_init,
    .frames_get_buffer = es_frames_get_buffer,
    .transfer_get_formats = es_transfer_get_formats,
    .transfer_data_to = es_transfer_data_to,
    .transfer_data_from = es_transfer_data_from,

    .pix_fmts = (const enum AVPixelFormat[]){AV_PIX_FMT_ES, AV_PIX_FMT_NONE},
};
