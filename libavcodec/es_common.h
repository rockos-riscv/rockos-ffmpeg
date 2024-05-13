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

#ifndef AVCODEC_ES_COMMON_H
#define AVCODEC_ES_COMMON_H

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

#define ES_VID_DEC_MAX_OUT_COUNT 2

// define side_data type
enum _SideDataType {
    SIDE_DATA_TYPE_BASE = 0x12340000,
    SIDE_DATA_TYPE_SLICE_SIZE,            // slice size
    SIDE_DATA_TYPE_FORCE_IDR,             // force idr
    SIDE_DATA_TYPE_INSERT_SPS_PPS,        // insert sps pps
    SIDE_DATA_TYPE_RC,                    // RC
    SIDE_DATA_TYPE_ROI_AREA,              // roi area, 8 areas
    SIDE_DATA_TYPE_CU_MAP,                // roi cu map or skip cu map
    SIDE_DATA_TYPE_MEM_FRAME_FD,          // fd in within avframe
    SIDE_DATA_TYPE_MEM_FRAME_FD_RELEASE,  // fd release within avframe

    SIDE_DATA_TYPE_JENC_BASE = 0x12345000,
    SIDE_DATA_TYPE_JENC_THUMBNAIL,       // input thumbnail
    SIDE_DATA_TYPE_JENC_ROI_AREA,        // input regions of intereset area
    SIDE_DATA_TYPE_JENC_NON_ROI_FILTER,  // non roi filter
};

typedef enum ESDecCodec {
    ES_HEVC,
    ES_H264_H10P,
    ES_H264,
    ES_JPEG,
} ESDecCodec;

typedef struct {
    int crop_xoffset;
    int crop_yoffset;
    int crop_width;
    int crop_height;
} CropInfo;

typedef struct {
    int scale_width;
    int scale_height;
} ScaleInfo;

typedef struct {
    int width;
    int height;
    int pic_stride;    /**< picture stride for luma */
    int pic_stride_ch; /**< picture stride for chroma */
    char *ppu_channel;
    char *prefix_name;
    char *suffix_name;
    char *fmt;
} DumpParas;

typedef struct {
    int64_t stop_dump_time;
    FILE *fp;
} DumpHandle;

int ff_codec_get_crop(char *str, CropInfo *crop_info);

int ff_dec_get_scale(char *str, ScaleInfo *scale_info, int pp_idx);

int es_codec_get_crop(char *str, CropInfo *crop_info);

int es_codec_get_scale(char *str, ScaleInfo *scale_info);

DumpHandle *ff_codec_dump_file_open(const char *dump_path, int duration, DumpParas *paras);

int ff_codec_compara_timeb(int64_t end_time);

int ff_codec_dump_file_close(DumpHandle **dump_handle);

int ff_codec_dump_bytes_to_file(void *data, int size, DumpHandle *dump_handle);

const char *ff_vsv_encode_get_fmt_char(enum AVPixelFormat pix_fmt);

int ff_es_codec_add_fd_to_side_data(AVFrame *frame, uint64_t fd);

int ff_es_codec_memcpy_block(void *src, void *dst, size_t data_size);

int ff_es_codec_memcpy_by_line(uint8_t *src, uint8_t *dst, int src_linesize, int dst_linesize, int linecount);

#endif
