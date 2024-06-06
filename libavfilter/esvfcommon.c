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

#include "esvfcommon.h"
#include <libavutil/pixfmt.h>
#ifndef MODEL_SIMULATION
#include <es2d_api.h>
#endif

int vf_is_simulation(void) {
    int result;
#ifndef MODEL_SIMULATION
    result = FALSE;
#else
    result = TRUE;
#endif

    av_log(NULL, AV_LOG_INFO, "is simulation: %d\n", result);
    return result;
}

int vf_add_fd_to_side_data(AVFrame *frame, uint64_t fd) {
    int ret = SUCCESS;
    AVFrameSideData *sd = NULL;

    if (!frame) {
        av_log(NULL, AV_LOG_ERROR, "frame is null\n");
        return FAILURE;
    }

    av_log(NULL, AV_LOG_DEBUG, "convert_esadd side data: fd = %lx\n", fd);
    sd = av_frame_new_side_data(frame, SIDE_DATA_TYPE_MEM_FRAME_FD, sizeof(fd));
    if (sd && sd->data) {
        memcpy(sd->data, &fd, sizeof(fd));
    } else {
        ret = FAILURE;
        av_log(NULL, AV_LOG_ERROR, "av_frame_new_side_data faild sd: %p\n", sd);
    }

    return ret;
}

int esvf_get_fd(const AVFrame *frame, int64_t *fd) {
    AVFrameSideData *sd = NULL;
    if (!frame) return -1;
    if (!frame->nb_side_data) return -1;

    sd = av_frame_get_side_data(frame, SIDE_DATA_TYPE_MEM_FRAME_FD);
    if (sd) {
        *fd = *((int64_t *)sd->data);
        av_log(NULL, AV_LOG_INFO, "convert_es got mem_fd: %lx\n", *fd);
        return 0;
    }

    return -1;
}

void esvf_memcpy_block(void *src, void *dst, size_t data_size) {
    memcpy(dst, src, data_size);
}

int esvf_memcpy_by_line(uint8_t *src, uint8_t *dst, int src_linesize, int dst_linesize, int linecount) {
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

FILE *vf_dump_file_open(const char *dump_path, const char* filename) {
    FILE *dump_handle = NULL;
    char file_path[PATH_MAX];
    int64_t time_ms;
    int ret;

    if (!dump_path || !filename) {
        av_log(NULL, AV_LOG_ERROR, "error !!! dump path or filename is null\n");
        return NULL;
    }

    ret = access(dump_path, 0);
    if (ret == -1) {
        av_log(NULL, AV_LOG_INFO, "dump_path: %s does not exist\n", dump_path);
        if (mkdir(dump_path, 0731) == -1) {
            av_log(NULL, AV_LOG_ERROR, "create dump_path: %s failed errno: %d\n", dump_path, errno);
            return NULL;
        }
    } else {
        av_log(NULL, AV_LOG_INFO, "dump_path: %s exist\n", dump_path);
    }

    time_ms =  av_gettime();

    snprintf(file_path, sizeof(file_path), "%s/%lld_%s", dump_path, time_ms, filename);

    dump_handle = fopen(file_path, "ab+");
    if (dump_handle) {
        av_log(NULL, AV_LOG_INFO, "open %s success\n", file_path);
    } else {
        dump_handle = NULL;
        av_log(NULL, AV_LOG_ERROR, "open %s failed\n", file_path);
        return NULL;
    }

    return dump_handle;
}

int vf_dump_bytes_to_file(void *data, int size, FILE *fp) {
    int len = 0;

    if (!fp) {
        av_log(NULL, AV_LOG_ERROR, " invaild file handle\n");
        return FAILURE;
    }

    if (!data || size <= 0) {
        return FAILURE;
    }

    len = fwrite(data, 1, size, fp);
    fflush(fp);
    if (len != size) {
        av_log(NULL, AV_LOG_ERROR, "write data to file error !!! len: %d, data size: %d\n", len, size);
    }

    return len;
}

int vf_para_crop(char *str, CropInfo *crop_info) {
    if (!str || !crop_info) {
        return FAILURE;
    }

    if (sscanf(str,
               "%dx%dx%dx%d",
               &crop_info->crop_xoffset,
               &crop_info->crop_yoffset,
               &crop_info->crop_width,
               &crop_info->crop_height) != 4) {
        return FAILURE;
    }

    return SUCCESS;
}

int vf_para_scale(char *str, ScaleInfo *scale_info) {
    if (!str || !scale_info) {
        return FAILURE;
    }

    if (sscanf(str,
              "%dx%d",
               &scale_info->scale_width,
               &scale_info->scale_height) != 2) {
        return FAILURE;
    }

    return SUCCESS;
}

int vf_para_normalization_rgb(char *str, VF2DRgbU32 *RGB_info) {
    if (!str || !RGB_info) {
        return FAILURE;
    }

    if (sscanf(str,
              "%x/%x/%x",
               &RGB_info->R,
               &RGB_info->G,
               &RGB_info->B) != 3) {
        return FAILURE;
    }

    return SUCCESS;
}

#ifndef MODEL_SIMULATION
typedef struct {
    enum AVPixelFormat pixfmt;
    ES2D_PIXEL_FMT picfmt;
} AvFmtTo2Dfmt;

static const AvFmtTo2Dfmt fmtto2dfmttable[] = {
    {AV_PIX_FMT_NV12, ES2D_FMT_NV12},
    {AV_PIX_FMT_NV21, ES2D_FMT_NV21},
    {AV_PIX_FMT_YUV420P, ES2D_FMT_I420},
    {AV_PIX_FMT_GRAY8, ES2D_FMT_GRAY8},
    {AV_PIX_FMT_GRAYF32LE, ES2D_FMT_GRAY32F},
    {AV_PIX_FMT_YUV420P10LE, ES2D_FMT_I010_LSB},
    {AV_PIX_FMT_P010LE, ES2D_FMT_P010},
    {AV_PIX_FMT_YVYU422, ES2D_FMT_YVYU},
    {AV_PIX_FMT_YUYV422, ES2D_FMT_YUY2},
    {AV_PIX_FMT_UYVY422, ES2D_FMT_UYVY},
    {AV_PIX_FMT_NV16, ES2D_FMT_NV16},
    {AV_PIX_FMT_BGR24, ES2D_FMT_R8G8B8},
    {AV_PIX_FMT_ARGB, ES2D_FMT_B8G8R8A8},
    {AV_PIX_FMT_ABGR, ES2D_FMT_R8G8B8A8},
    {AV_PIX_FMT_BGRA, ES2D_FMT_A8R8G8B8},
    {AV_PIX_FMT_RGBA, ES2D_FMT_A8B8G8R8},
    {AV_PIX_FMT_B16G16R16I, ES2D_FMT_R16G16B16I},
    {AV_PIX_FMT_B16G16R16I_PLANAR, ES2D_FMT_R16G16B16I_PLANAR},
    {AV_PIX_FMT_B8G8R8_PLANAR, ES2D_FMT_R8G8B8_PLANAR},
    {AV_PIX_FMT_B8G8R8I, ES2D_FMT_R8G8B8I},
    {AV_PIX_FMT_B8G8R8I_PLANAR, ES2D_FMT_R8G8B8I_PLANAR},
    {AV_PIX_FMT_B16G16R16F, ES2D_FMT_R16G16B16F},
    {AV_PIX_FMT_B16G16R16F_PLANAR, ES2D_FMT_R16G16B16F_PLANAR},
    {AV_PIX_FMT_B32G32R32F, ES2D_FMT_R32G32B32F},
    {AV_PIX_FMT_B32G32R32F_PLANAR, ES2D_FMT_R32G32B32F_PLANAR},
};

static enum AVPixelFormat vf_2dfmt_to_pixfmt(ES2D_PIXEL_FMT picfmt) {
    int i;

    for (i = 0; i < sizeof(fmtto2dfmttable) / sizeof(AvFmtTo2Dfmt); i++)
        if (fmtto2dfmttable[i].picfmt == picfmt) return fmtto2dfmttable[i].pixfmt;

    return AV_PIX_FMT_NONE;
}

static ES2D_PIXEL_FMT vf_pixfmt_to_2dfmt(enum AVPixelFormat pixfmt) {
    int i;

    for (i = 0; i < sizeof(fmtto2dfmttable) / sizeof(AvFmtTo2Dfmt); i++)
        if (fmtto2dfmttable[i].pixfmt == pixfmt) return fmtto2dfmttable[i].picfmt;

    return AV_PIX_FMT_NONE;
}

int vf_2D_init(ES2D_ID *id) {
    if (!id) {
        av_log(NULL, AV_LOG_ERROR, "vf_2D_init invaild id: %p\n", id);
        return FAILURE;
    }

    if (es2d_init(id) != ES2D_STATUS_OK) {
        av_log(NULL, AV_LOG_ERROR, "vf_2D_init failed!\n");
        return FAILURE;
    } else {
        av_log(NULL, AV_LOG_INFO, "vf_2D_init ...ok\n");
    }

    return SUCCESS;
}

int vf_2D_unint(ES2D_ID *id) {
    if (!id) {
        av_log(NULL, AV_LOG_ERROR, "vf_2D_unint invaild id: %p\n", id);
        return FAILURE;
    }

    if (es2d_destroy(*id) != ES2D_STATUS_OK) {
        av_log(NULL, AV_LOG_ERROR, "es2d_destroy failed!\n");
        return FAILURE;
    } else {
        av_log(NULL, AV_LOG_INFO, "es2d_destroy ...ok\n");
    }

    return SUCCESS;
}

static void vf_printf_2Dsurface(ES2D_SURFACE *surface, const char *name) {
     if (!surface || !name) {
        av_log(NULL, AV_LOG_ERROR, "invaild paras, surface: %p, name: %p!\n", surface, name);
        return;
    }

    av_log(NULL, AV_LOG_DEBUG,
           "%s: surface.dma_buf: %p, surface.offset: %d, "
           "surface.width: %d, surface.height: %d, surface.format: %d\n",
           name,
           surface->dma_buf,
           surface->offset,
           surface->width,
           surface->height,
           surface->format);
}

static void vf_printf_2Drect(ES2D_RECT *rect, const char *name) {
    if (!rect || !name) {
        av_log(NULL, AV_LOG_ERROR, "invaild paras, rect: %p, name: %p!\n", rect, name);
        return;
    }

    av_log(NULL, AV_LOG_DEBUG,
           "%s: rect.left: %d, rect.top: %d, "
           "rect.right: %d, rect.bottom: %d\n",
           name,
           rect->left,
           rect->top,
           rect->right,
           rect->bottom);
}

static void vf_printf_normalization_params(ES2D_NORMALIZATION_PARAMS *paras) {
    if (!paras) {
        av_log(NULL, AV_LOG_ERROR, "normalization paras is null!\n");
        return;
    }

    av_log(NULL, AV_LOG_DEBUG,
           "normalizationMode: %d,stepReciprocal: %x\n",
           paras->normalizationMode,
           paras->stepReciprocal);
    av_log(NULL, AV_LOG_DEBUG,
           "minValue.R: %x, minValue.G: %x, minValue.B: %x\n",
           paras->minValue.R,
           paras->minValue.G,
           paras->minValue.B);
    av_log(NULL, AV_LOG_DEBUG,
           "maxMinReciprocal.R: %x, maxMinReciprocal.G: %x, maxMinReciprocal.B: %x\n",
           paras->maxMinReciprocal.R,
           paras->maxMinReciprocal.G,
           paras->maxMinReciprocal.B);
    av_log(NULL, AV_LOG_DEBUG,
           "meanValue.R: %x, meanValue.G: %x, meanValue.B: %x\n",
           paras->meanValue.R,
           paras->meanValue.G,
           paras->meanValue.B);
    av_log(NULL, AV_LOG_DEBUG,
           "stdReciprocal.R: %x, stdReciprocal.G: %x, stdReciprocal.B: %x\n",
           paras->stdReciprocal.R,
           paras->stdReciprocal.G,
           paras->stdReciprocal.B);
}

int vf_2D_work(ES2D_ID *id,
               VF2DSurface *srcSurface,
               VF2DSurface *dstSurface,
               VF2DNormalizationParams *normalizationParas,
               VF2DRect *srcRect,
               VF2DRect *dstRect) {
    ES2D_SURFACE src_surface;
    ES2D_SURFACE dst_surface;
    ES2D_RECT src_rect;
    ES2D_RECT dst_rect;
    ES2D_NORMALIZATION_PARAMS normalization_paras;
    int is_normalization = 0;
    int ret = FAILURE;

    if (!id || !srcSurface || !dstSurface || !srcRect || !dstRect) {
        av_log(NULL, AV_LOG_ERROR,
               "vf_2D_work invaild id: %p,"
               " srcSurface: %p, dstSurface: %p srcRect: %p, dstRect: %p\n",
               id,
               srcSurface,
               dstSurface,
               srcRect,
               dstRect);
        return FAILURE;
    }

    memset(&src_surface, 0, sizeof(ES2D_SURFACE));
    memset(&dst_surface, 0, sizeof(ES2D_SURFACE));
    memset(&src_rect, 0, sizeof(ES2D_RECT));
    memset(&dst_rect, 0, sizeof(ES2D_RECT));

    src_surface.width = srcSurface->width;
    src_surface.height = srcSurface->height;
    src_surface.format = vf_pixfmt_to_2dfmt(srcSurface->pixfmt);
    src_surface.dma_buf = srcSurface->dma_buf;

    dst_surface.width = dstSurface->width;
    dst_surface.height = dstSurface->height;
    dst_surface.format = vf_pixfmt_to_2dfmt(dstSurface->pixfmt);
    dst_surface.dma_buf = dstSurface->dma_buf;

    src_rect.bottom = srcRect->bottom;
    src_rect.left = srcRect->left;
    src_rect.right = srcRect->right;
    src_rect.top = srcRect->top;

    dst_rect.bottom = dstRect->bottom;
    dst_rect.left = dstRect->left;
    dst_rect.right = dstRect->right;
    dst_rect.top = dstRect->top;

    vf_printf_2Dsurface(&src_surface, "src_surface");
    vf_printf_2Dsurface(&dst_surface, "dst_surface");
    vf_printf_2Drect(&src_rect, "src_rect");
    vf_printf_2Drect(&dst_rect, "dst_rect");

    if (normalizationParas) {
        is_normalization = 1;
        // for mode
        normalization_paras.normalizationMode =  normalizationParas->normalizationMode;
        // for min-max
        normalization_paras.minValue.R = normalizationParas->minValue.R;
        normalization_paras.minValue.G = normalizationParas->minValue.G;
        normalization_paras.minValue.B = normalizationParas->minValue.B;
        normalization_paras.maxMinReciprocal.R = normalizationParas->maxMinReciprocal.R;
        normalization_paras.maxMinReciprocal.G = normalizationParas->maxMinReciprocal.G;
        normalization_paras.maxMinReciprocal.B = normalizationParas->maxMinReciprocal.B;
        // for z-score
        normalization_paras.meanValue.R = normalizationParas->meanValue.R;
        normalization_paras.meanValue.G = normalizationParas->meanValue.G;
        normalization_paras.meanValue.B = normalizationParas->meanValue.B;
        normalization_paras.stdReciprocal.R = normalizationParas->stdReciprocal.R;
        normalization_paras.stdReciprocal.G = normalizationParas->stdReciprocal.G;
        normalization_paras.stdReciprocal.B = normalizationParas->stdReciprocal.B;
        // for stepReciprocal
        normalization_paras.stepReciprocal = normalizationParas->stepReciprocal;

        vf_printf_normalization_params(&normalization_paras);
    }

    if (es2d_work(*id,
                  &src_surface,
                  &dst_surface,
                  OP_STRECHBLIT,
                  is_normalization ? &normalization_paras : NULL,
                  &src_rect,
                  &dst_rect) == ES2D_STATUS_OK) {
        ret = SUCCESS;
    }

    return ret;
}

#endif