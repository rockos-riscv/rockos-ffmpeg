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

#ifndef AVUTIL_HWCONTEXT_VSV_H
#define AVUTIL_HWCONTEXT_VSV_H
// #ifndef ESW_FF_ENHANCEMENT
// #include "dectypes.h"
// #include "dwl.h"
// #endif
/**
 * VAAPI connection details.
 *
 * Allocated as AVHWDeviceContext.hwctx
 */
typedef struct AVVSVDeviceContext {
    char *device;
    unsigned int dec_client_type;
    unsigned int enc_client_type;

    int priority;
    int fbloglevel;
    int tile_enable;
    int planar_enable;

    int fd_mem;
    int task_id;

    int lookahead;
    int nb_frames;
    const void *dwl_inst;
} AVVSVDeviceContext;

typedef enum AVVSVFormat {
    AVVSV_FORMAT_YUV420_SEMIPLANAR,
} AVVSVFormat;

/**
 * VAAPI-specific data associated with a frame pool.
 *
 * Allocated as AVHWFramesContext.hwctx.
 */
typedef struct AVVSVFramesContext {
    int task_id;
    struct {
        int enabled;
        AVVSVFormat format;
        int width;
        int height;
        struct {
            int enabled;
            int x;
            int y;
            int w;
            int h;
        } crop;
        struct {
            int enabled;
            int w;
            int h;
        } scale;
    } pic_info[5];
// #ifndef ESW_FF_ENHANCEMENT
//     struct DWLLinearMem* buffer;
// #endif
} AVVSVFramesContext;

typedef struct VSVFramePriv {
    int pp_sel;
} VSVFramePriv;

#endif


