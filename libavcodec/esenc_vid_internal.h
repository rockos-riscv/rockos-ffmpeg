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

#ifndef AVCODEC_ESENC_VIDEO_INTERNAL_H
#define AVCODEC_ESENC_VIDEO_INTERNAL_H

#include "esenc_vid.h"

void ff_change_to_customized_format(ESEncVidContext *options, VCEncPreProcessingCfg *pre_proc_cfg);

void ff_change_cml_customized_format(ESEncVidContext *options);

int32_t ff_read_gmv(ESEncVidInternalContext *tb, VCEncIn *pEncIn, ESEncVidContext *options);

uint64_t ff_next_picture(ESEncVidInternalContext *tb, int picture_cnt);

int ff_init_gop_configs(int gopSize,
                        ESEncVidContext *options,
                        VCEncGopConfig *gopCfg,
                        uint8_t *gopCfgOffset,
                        bool bPass2,
                        uint32_t hwId);

uint8_t *ff_read_user_data(VCEncInst encoder, char *name);

void ff_ma_add_frame(Ma *ma, int32_t frame_size_bits);

int32_t ff_ma(Ma *ma);

int32_t ff_get_next_gop_size(
    ESEncVidInternalContext *tb, VCEncIn *pEncIn, VCEncInst encoder, int32_t *pnextgopsize, AdapGopCtr *agop);

int32_t ff_change_format_for_fb(ESEncVidInternalContext *tb,
                                ESEncVidContext *options,
                                VCEncPreProcessingCfg *pre_proc_cfg);

FILE *ff_format_customized_yuv(ESEncVidInternalContext *tb, ESEncVidContext *options);

void ff_init_slice_ctl(ESEncVidInternalContext *tb, ESEncVidContext *options);

void ff_init_stream_segment_ctl(ESEncVidInternalContext *tb, ESEncVidContext *options);

void ff_setup_slice_ctl(ESEncVidInternalContext *tb);

unsigned int ff_utime_diff(struct timeval end, struct timeval start);

#endif
