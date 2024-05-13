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

#ifndef AVCODEC_ESVF_COMMON_H
#define AVCODEC_ESVF_COMMON_H

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <time.h>
#include <sys/timeb.h>
#include "libavutil/time.h"
#include <linux/limits.h>
#include <unistd.h>
#include <libavutil/pixfmt.h>
#include <libavutil/frame.h>
#ifndef MODEL_SIMULATION
#include <es-dma-buf.h>
#endif

#ifndef SUCCESS
#define SUCCESS (0)
#endif

#ifndef FAILURE
#define FAILURE (-1)
#endif

#ifndef ERR_TIMEOUT
#define ERR_TIMEOUT (-2)
#endif

#ifndef TRUE
#define TRUE (1)
#endif

#ifndef FALSE
#define FALSE (0)
#endif

#define MAX_OUTPUT_BUFFERS 18
#define DEFAULT_OUTPUT_BUFFERS 3


#define SIDE_DATA_TYPE_MEM_FRAME_FD 0x12340007
#define SIDE_DATA_TYPE_MEM_FRAME_FD_RELEASE 0x12340008

typedef unsigned char ES2D_ID;

typedef struct _CropInfo{
    int crop_xoffset;
    int crop_yoffset;
    int crop_width;
    int crop_height;
} CropInfo;

typedef struct _ScaleInfo{
    int scale_width;
    int scale_height;
} ScaleInfo;

typedef struct _VF2DSurface {
    void *dma_buf; //es_dma_buf structure point
    int32_t offset; //the offset based on dma_buf
    int32_t width;
    int32_t height;
    enum AVPixelFormat pixfmt;
} VF2DSurface;

typedef struct _VF2DRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} VF2DRect;

typedef enum _VF2DNormalizationMode {
    VF2D_NORMALIZATION_MIN_MAX,
    VF2D_NORMALIZATION_Z_SCORE
} VF2DNormalizationMode;

typedef struct _VF2DRGBU32 {
    uint32_t R;
    uint32_t G;
    uint32_t B;
} VF2DRgbU32;

typedef struct _VF2DNormalizationParams {
    VF2DNormalizationMode normalizationMode;
    VF2DRgbU32 minValue;
    VF2DRgbU32 maxMinReciprocal;
    VF2DRgbU32 stdReciprocal;
    VF2DRgbU32 meanValue;
    uint32_t stepReciprocal;
} VF2DNormalizationParams;

int vf_is_simulation(void);

int vf_add_fd_to_side_data(AVFrame *frame, uint64_t fd);

int esvf_get_fd(const AVFrame *frame, int64_t *fd);

void esvf_memcpy_block(void *src, void *dst, size_t data_size);

int esvf_memcpy_by_line(uint8_t *src, uint8_t *dst, int src_linesize, int dst_linesize, int linecount);

FILE *vf_dump_file_open(const char *dump_path, const char* filename);

int vf_dump_bytes_to_file(void *data, int size, FILE *fp);

int vf_para_crop(char *str, CropInfo *crop_info);

int vf_para_scale(char *str, ScaleInfo *scale_info);

int vf_para_normalization_rgb(char *str, VF2DRgbU32 *RGB_info);

#ifndef MODEL_SIMULATION
int vf_2D_init(ES2D_ID *id);

int vf_2D_unint(ES2D_ID *id);

int vf_2D_work(ES2D_ID *id,
               VF2DSurface *srcSurface,
               VF2DSurface *dstSurface,
               VF2DNormalizationParams *normalizationParas,
               VF2DRect *srcRect,
               VF2DRect *dstRect);
#endif

#endif