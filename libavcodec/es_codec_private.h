

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
#ifndef AVCODEC_VSV_PRIVATE_H
#define AVCODEC_VSV_PRIVATE_H

#include <stdint.h>
#include <stddef.h>
#include "libavutil/pixfmt.h"


struct AVBufferRef;
#define ES_DEC_MAX_OUT_COUNT 3
#define DEFAULT_FRAME_PP_INDEX  0

typedef struct _OutPutInfo {
  #define VIDEO_MAX_PLANES 4
    int32_t enabled;
    int fd;
    uint32_t width;
    uint32_t height;
    enum AVPixelFormat format;
    int32_t key_frame;
    int32_t n_planes;
    uint32_t *virtual_address;
    size_t bus_address;
    size_t size;
    size_t offset[VIDEO_MAX_PLANES];
    int32_t stride[VIDEO_MAX_PLANES];
} OutPutInfo;

typedef struct _DecPicturePri {
  uint32_t pic_count;
  int32_t default_index;
  int32_t stride_align;
  OutPutInfo *default_pic;
  void *hwpic;
  struct AVBufferRef *ctx_buf;
  OutPutInfo pictures[ES_DEC_MAX_OUT_COUNT];
} DecPicturePri;

#endif //AVCODEC_VSV_PRIVATE_H