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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tools.h"
#include "esenc_vid_internal.h"

#define MAX_LINE_LENGTH_BLOCK 512 * 8

//           Type POC QPoffset  QPfactor  num_ref_pics ref_pics  used_by_cur
const char *RpsDefault_GOPSize_1[] = {
    "Frame1:  P    1   0        0.578     0      1        -1         1",
    NULL,
};

const char *RpsDefault_V60_GOPSize_1[] = {
    "Frame1:  P    1   0        0.8     0      1        -1         1",
    NULL,
};

const char *RpsDefault_H264_GOPSize_1[] = {
    "Frame1:  P    1   0        0.4     0      1        -1         1",
    NULL,
};

const char *RpsDefault_GOPSize_2[] = {
    "Frame1:  P        2   0        0.6     0      1        -2         1",
    "Frame2:  nrefB    1   0        0.68    0      2        -1 1       1 1",
    NULL,
};

const char *RpsDefault_GOPSize_3[] = {
    "Frame1:  P        3   0        0.5     0      1        -3         1   ",
    "Frame2:  B        1   0        0.5     0      2        -1 2       1 1 ",
    "Frame3:  nrefB    2   0        0.68    0      2        -1 1       1 1 ",
    NULL,
};

const char *RpsDefault_GOPSize_4[] = {
    "Frame1:  P        4   0        0.5      0     1       -4         1 ",
    "Frame2:  B        2   0        0.3536   0     2       -2 2       1 1",
    "Frame3:  nrefB    1   0        0.5      0     3       -1 1 3     1 1 0",
    "Frame4:  nrefB    3   0        0.5      0     2       -1 1       1 1 ",
    NULL,
};

const char *RpsDefault_GOPSize_5[] = {
    "Frame1:  P        5   0        0.442    0     1       -5         1 ",
    "Frame2:  B        2   0        0.3536   0     2       -2 3       1 1",
    "Frame3:  nrefB    1   0        0.68     0     3       -1 1 4     1 1 0",
    "Frame4:  B        3   0        0.3536   0     2       -1 2       1 1 ",
    "Frame5:  nrefB    4   0        0.68     0     2       -1 1       1 1 ",
    NULL,
};

const char *RpsDefault_GOPSize_6[] = {
    "Frame1:  P        6   0        0.442    0     1       -6         1 ",
    "Frame2:  B        3   0        0.3536   0     2       -3 3       1 1",
    "Frame3:  B        1   0        0.3536   0     3       -1 2 5     1 1 0",
    "Frame4:  nrefB    2   0        0.68     0     3       -1 1 4     1 1 0",
    "Frame5:  B        4   0        0.3536   0     2       -1 2       1 1 ",
    "Frame6:  nrefB    5   0        0.68     0     2       -1 1       1 1 ",
    NULL,
};

const char *RpsDefault_GOPSize_7[] = {
    "Frame1:  P        7   0        0.442    0     1       -7         1 ",
    "Frame2:  B        3   0        0.3536   0     2       -3 4       1 1",
    "Frame3:  B        1   0        0.3536   0     3       -1 2 6     1 1 0",
    "Frame4:  nrefB    2   0        0.68     0     3       -1 1 5     1 1 0",
    "Frame5:  B        5   0        0.3536   0     2       -2 2       1 1 ",
    "Frame6:  nrefB    4   0        0.68     0     3       -1 1 3     1 1 0",
    "Frame7:  nrefB    6   0        0.68     0     2       -1 1       1 1 ",
    NULL,
};

const char *RpsDefault_GOPSize_8[] = {
    "Frame1:  P        8   0        0.442    0  1           -8        1 ",
    "Frame2:  B        4   0        0.3536   0  2           -4 4      1 1 ",
    "Frame3:  B        2   0        0.3536   0  3           -2 2 6    1 1 0 ",
    "Frame4:  nrefB    1   0        0.68     0  4           -1 1 3 7  1 1 0 0",
    "Frame5:  nrefB    3   0        0.68     0  3           -1 1 5    1 1 0",
    "Frame6:  B        6   0        0.3536   0  2           -2 2      1 1",
    "Frame7:  nrefB    5   0        0.68     0  3           -1 1 3    1 1 0",
    "Frame8:  nrefB    7   0        0.68     0  2           -1 1      1 1",
    NULL,
};

const char *RpsDefault_GOPSize_16[] = {
    "Frame1:  P       16   0        0.6      0  1           -16                   1",
    "Frame2:  B        8   0        0.2      0  2           -8   8                1   1",
    "Frame3:  B        4   0        0.33     0  3           -4   4  12            1   1   0",
    "Frame4:  B        2   0        0.33     0  4           -2   2   6  14        1   1   0   0",
    "Frame5:  nrefB    1   0        0.4      0  5           -1   1   3   7  15    1   1   0   0   0",
    "Frame6:  nrefB    3   0        0.4      0  4           -1   1   5  13        1   1   0   0",
    "Frame7:  B        6   0        0.33     0  3           -2   2  10            1   1   0",
    "Frame8:  nrefB    5   0        0.4      0  4           -1   1   3  11        1   1   0   0",
    "Frame9:  nrefB    7   0        0.4      0  3           -1   1   9            1   1   0",
    "Frame10: B       12   0        0.33     0  2           -4   4                1   1",
    "Frame11: B       10   0        0.33     0  3           -2   2   6            1   1   0",
    "Frame12: nrefB    9   0        0.4      0  4           -1   1   3   7        1   1   0   0",
    "Frame13: nrefB   11   0        0.4      0  3           -1   1   5            1   1   0",
    "Frame14: B       14   0        0.33     0  2           -2   2                1   1",
    "Frame15: nrefB   13   0        0.4      0  3           -1   1   3            1   1   0",
    "Frame16: nrefB   15   0        0.4      0  2           -1   1                1   1",
    NULL,
};

const char *RpsDefault_Interlace_GOPSize_1[] = {
    "Frame1:  P    1   0        0.8       0   2           -1 -2     0 1",
    NULL,
};

const char *RpsLowdelayDefault_GOPSize_1[] = {
    "Frame1:  B    1   0        0.65      0     2       -1 -2         1 1",
    NULL,
};

const char *RpsLowdelayDefault_GOPSize_2[] = {
    "Frame1:  B    1   0        0.4624    0     2       -1 -3         1 1",
    "Frame2:  B    2   0        0.578     0     2       -1 -2         1 1",
    NULL,
};

const char *RpsLowdelayDefault_GOPSize_3[] = {
    "Frame1:  B    1   0        0.4624    0     2       -1 -4         1 1",
    "Frame2:  B    2   0        0.4624    0     2       -1 -2         1 1",
    "Frame3:  B    3   0        0.578     0     2       -1 -3         1 1",
    NULL,
};

const char *RpsLowdelayDefault_GOPSize_4[] = {
    "Frame1:  B    1   0        0.4624    0     2       -1 -5         1 1",
    "Frame2:  B    2   0        0.4624    0     2       -1 -2         1 1",
    "Frame3:  B    3   0        0.4624    0     2       -1 -3         1 1",
    "Frame4:  B    4   0        0.578     0     2       -1 -4         1 1",
    NULL,
};

const char *RpsPass2_GOPSize_4[] = {
    "Frame1:  B        4   0        0.5      0     2       -4 -8      1 1",
    "Frame2:  B        2   0        0.3536   0     2       -2 2       1 1",
    "Frame3:  nrefB    1   0        0.5      0     3       -1 1 3     1 1 0",
    "Frame4:  nrefB    3   0        0.5      0     3       -1 -3 1    1 0 1",
    NULL,
};

const char *RpsPass2_GOPSize_8[] = {
    "Frame1:  B        8   0        0.442    0  2           -8 -16    1 1",
    "Frame2:  B        4   0        0.3536   0  2           -4 4      1 1",
    "Frame3:  B        2   0        0.3536   0  3           -2 2 6    1 1 0",
    "Frame4:  nrefB    1   0        0.68     0  4           -1 1 3 7  1 1 0 0",
    "Frame5:  nrefB    3   0        0.68     0  4           -1 -3 1 5 1 0 1 0",
    "Frame6:  B        6   0        0.3536   0  3           -2 -6 2   1 0 1",
    "Frame7:  nrefB    5   0        0.68     0  4           -1 -5 1 3 1 0 1 0",
    "Frame8:  nrefB    7   0        0.68     0  3           -1 -7 1   1 0 1",
    NULL,
};

const char *RpsPass2_GOPSize_2[] = {
    "Frame1:  B        2   0        0.6     0      2        -2 -4      1 1",
    "Frame2:  nrefB    1   0        0.68    0      2        -1 1       1 1",
    NULL,
};

// 2 reference frames for P
const char *Rps_2RefForP_GOPSize_1[] = {
    "Frame1:  P    1   0        0.578     0      2        -1 -2         1 1",
    NULL,
};

const char *Rps_2RefForP_H264_GOPSize_1[] = {
    "Frame1:  P    1   0        0.4     0      2        -1 -2         1 1",
    NULL,
};

const char *Rps_2RefForP_GOPSize_2[] = {
    "Frame1:  P        2   0        0.6     0      2        -2 -4      1 1",
    "Frame2:  nrefB    1   0        0.68    0      2        -1 1       1 1",
    NULL,
};

const char *Rps_2RefForP_GOPSize_3[] = {
    "Frame1:  P        3   0        0.5     0      2        -3 -6      1 1 ",
    "Frame2:  B        1   0        0.5     0      2        -1 2       1 1 ",
    "Frame3:  nrefB    2   0        0.68    0      3        -1 -2 1    1 0 1 ",
    NULL,
};

const char *Rps_2RefForP_GOPSize_4[] = {
    "Frame1:  P        4   0        0.5      0     2       -4 -8      1 1 ",
    "Frame2:  B        2   0        0.3536   0     2       -2 2       1 1",
    "Frame3:  nrefB    1   0        0.5      0     3       -1 1 3     1 1 0",
    "Frame4:  nrefB    3   0        0.5      0     3       -1 -3 1    1 0 1",
    NULL,
};

const char *Rps_2RefForP_GOPSize_5[] = {
    "Frame1:  P        5   0        0.442    0     2       -5 -10     1 1",
    "Frame2:  B        2   0        0.3536   0     2       -2 3       1 1",
    "Frame3:  nrefB    1   0        0.68     0     3       -1 1 4     1 1 0",
    "Frame4:  B        3   0        0.3536   0     3       -1 -3 2    1 0 1 ",
    "Frame5:  nrefB    4   0        0.68     0     3       -1 -4 1    1 0 1 ",
    NULL,
};

const char *Rps_2RefForP_GOPSize_6[] = {
    "Frame1:  P        6   0        0.442    0     2       -6 -12     1 1",
    "Frame2:  B        3   0        0.3536   0     2       -3 3       1 1",
    "Frame3:  B        1   0        0.3536   0     3       -1 2 5     1 1 0",
    "Frame4:  nrefB    2   0        0.68     0     4       -1 -2 1 4  1 0 1 0",
    "Frame5:  B        4   0        0.3536   0     3       -1 -4 2    1 0 1 ",
    "Frame6:  nrefB    5   0        0.68     0     3       -1 -5 1    1 0 1 ",
    NULL,
};

const char *Rps_2RefForP_GOPSize_7[] = {
    "Frame1:  P        7   0        0.442    0     2       -7 -14     1 1",
    "Frame2:  B        3   0        0.3536   0     2       -3 4       1 1",
    "Frame3:  B        1   0        0.3536   0     3       -1 2 6     1 1 0",
    "Frame4:  nrefB    2   0        0.68     0     4       -1 -2 1 5  1 0 1 0",
    "Frame5:  B        5   0        0.3536   0     3       -2 -5 2    1 0 1 ",
    "Frame6:  nrefB    4   0        0.68     0     4       -1 -4 1 3  1 0 1 0",
    "Frame7:  nrefB    6   0        0.68     0     3       -1 -6 1    1 0 1 ",
    NULL,
};

const char *Rps_2RefForP_GOPSize_8[] = {
    "Frame1:  P        8   0        0.442    0  2          -8 -16     1 1",
    "Frame2:  B        4   0        0.3536   0  2          -4 4       1 1 ",
    "Frame3:  B        2   0        0.3536   0  3          -2 2 6     1 1 0 ",
    "Frame4:  nrefB    1   0        0.68     0  4          -1 1 3 7   1 1 0 0",
    "Frame5:  nrefB    3   0        0.68     0  4          -1 -3 1 5  1 0 1 0",
    "Frame6:  B        6   0        0.3536   0  3          -2 -6 2    1 0 1",
    "Frame7:  nrefB    5   0        0.68     0  4          -1 -5 1 3  1 0 1 0",
    "Frame8:  nrefB    7   0        0.68     0  3          -1 -7 1    1 0 1",
    NULL,
};

void ff_change_to_customized_format(ESEncVidContext *options, VCEncPreProcessingCfg *pre_proc_cfg) {
#ifndef ESW_FF_ENHANCEMENT
    if ((options->format_customized_type == 0) && (options->input_format == VCENC_YUV420_PLANAR)) {
        if (IS_HEVC(options->codec_format))
            pre_proc_cfg->inputType = VCENC_YUV420_PLANAR_8BIT_DAHUA_HEVC;
        else
            pre_proc_cfg->inputType = VCENC_YUV420_PLANAR_8BIT_DAHUA_H264;
        pre_proc_cfg->origWidth = ((pre_proc_cfg->origWidth + 16 - 1) & (~(16 - 1)));
    }
#endif
    if ((options->format_customized_type == 1)
        && ((options->input_format == VCENC_YUV420_SEMIPLANAR) || (options->input_format == VCENC_YUV420_SEMIPLANAR_VU)
            || (options->input_format == VCENC_YUV420_PLANAR_10BIT_P010))) {
#ifndef ESW_FF_ENHANCEMENT
        if (options->input_format == VCENC_YUV420_SEMIPLANAR)
            pre_proc_cfg->inputType = VCENC_YUV420_SEMIPLANAR_8BIT_FB;
        else if (options->input_format == VCENC_YUV420_SEMIPLANAR_VU)
            pre_proc_cfg->inputType = VCENC_YUV420_SEMIPLANAR_VU_8BIT_FB;
        else
            pre_proc_cfg->inputType = VCENC_YUV420_PLANAR_10BIT_P010_FB;
#else
        if (options->input_format == VCENC_YUV420_SEMIPLANAR_VU)
            pre_proc_cfg->inputType = VCENC_YUV420_SEMIPLANAR_VU_8BIT_TILE_4_4;
        else
            pre_proc_cfg->inputType = VCENC_YUV420_PLANAR_10BIT_P010;
#endif
    }

    if ((options->format_customized_type == 2) && (options->input_format == VCENC_YUV420_PLANAR)) {
        pre_proc_cfg->inputType = VCENC_YUV420_SEMIPLANAR_101010;
        pre_proc_cfg->xOffset = pre_proc_cfg->xOffset / 6 * 6;
    }

    if ((options->format_customized_type == 3) && (options->input_format == VCENC_YUV420_PLANAR)) {
        pre_proc_cfg->inputType = VCENC_YUV420_8BIT_TILE_64_4;
        pre_proc_cfg->xOffset = pre_proc_cfg->xOffset / 64 * 64;
        pre_proc_cfg->yOffset = pre_proc_cfg->yOffset / 4 * 4;
    }

    if ((options->format_customized_type == 4) && (options->input_format == VCENC_YUV420_PLANAR)) {
        pre_proc_cfg->inputType = VCENC_YUV420_UV_8BIT_TILE_64_4;
        pre_proc_cfg->xOffset = pre_proc_cfg->xOffset / 64 * 64;
        pre_proc_cfg->yOffset = pre_proc_cfg->yOffset / 4 * 4;
    }

    if ((options->format_customized_type == 5) && (options->input_format == VCENC_YUV420_PLANAR)) {
        pre_proc_cfg->inputType = VCENC_YUV420_10BIT_TILE_32_4;
        pre_proc_cfg->xOffset = pre_proc_cfg->xOffset / 32 * 32;
        pre_proc_cfg->yOffset = pre_proc_cfg->yOffset / 4 * 4;
    }

    if ((options->format_customized_type == 6) && (options->input_format == VCENC_YUV420_PLANAR)) {
        pre_proc_cfg->inputType = VCENC_YUV420_10BIT_TILE_48_4;
        pre_proc_cfg->xOffset = pre_proc_cfg->xOffset / 48 * 48;
        pre_proc_cfg->yOffset = pre_proc_cfg->yOffset / 4 * 4;
    }

    if ((options->format_customized_type == 7) && (options->input_format == VCENC_YUV420_PLANAR)) {
        pre_proc_cfg->inputType = VCENC_YUV420_VU_10BIT_TILE_48_4;
        pre_proc_cfg->xOffset = pre_proc_cfg->xOffset / 48 * 48;
        pre_proc_cfg->yOffset = pre_proc_cfg->yOffset / 4 * 4;
    }

    if ((options->format_customized_type == 8) && (options->input_format == VCENC_YUV420_PLANAR)) {
        pre_proc_cfg->inputType = VCENC_YUV420_8BIT_TILE_128_2;
        pre_proc_cfg->xOffset = pre_proc_cfg->xOffset / 128 * 128;
    }

    if ((options->format_customized_type == 9) && (options->input_format == VCENC_YUV420_PLANAR)) {
        pre_proc_cfg->inputType = VCENC_YUV420_UV_8BIT_TILE_128_2;
        pre_proc_cfg->xOffset = pre_proc_cfg->xOffset / 128 * 128;
    }

    if ((options->format_customized_type == 10) && (options->input_format == VCENC_YUV420_PLANAR)) {
        pre_proc_cfg->inputType = VCENC_YUV420_10BIT_TILE_96_2;
        pre_proc_cfg->xOffset = pre_proc_cfg->xOffset / 96 * 96;
    }

    if ((options->format_customized_type == 11) && (options->input_format == VCENC_YUV420_PLANAR)) {
        pre_proc_cfg->inputType = VCENC_YUV420_VU_10BIT_TILE_96_2;
        pre_proc_cfg->xOffset = pre_proc_cfg->xOffset / 96 * 96;
    }
}

void ff_change_cml_customized_format(ESEncVidContext *options) {
    if ((options->format_customized_type == 0) && (options->input_format == VCENC_YUV420_PLANAR)) {
        if (IS_H264(options->codec_format)) {
            if (options->ver_offset_src != DEFAULT) options->ver_offset_src = options->ver_offset_src & (~(16 - 1));
            if (options->hor_offset_src != DEFAULT) options->hor_offset_src = options->hor_offset_src & (~(16 - 1));
        } else {
            if (options->ver_offset_src != DEFAULT) options->ver_offset_src = options->ver_offset_src & (~(32 - 1));
            if (options->hor_offset_src != DEFAULT) options->hor_offset_src = options->hor_offset_src & (~(32 - 1));
        }
        options->width = options->width & (~(16 - 1));
        options->height = options->height & (~(16 - 1));
    }

    if ((options->format_customized_type == 1)
        && ((options->input_format == VCENC_YUV420_SEMIPLANAR) || (options->input_format == VCENC_YUV420_SEMIPLANAR_VU)
            || (options->input_format == VCENC_YUV420_PLANAR_10BIT_P010))) {
        options->ver_offset_src = 0;
        options->hor_offset_src = 0;
        if (options->test_id == 16) options->test_id = 0;

        options->rotation = 0;
    } else if (options->format_customized_type == 1) {
        options->format_customized_type = -1;
    }

    if (((options->format_customized_type >= 2) && (options->format_customized_type <= 11))
        && (options->input_format == VCENC_YUV420_PLANAR)) {
        if (options->test_id == 16) options->test_id = 0;
        options->rotation = 0;
    } else {
        options->format_customized_type = -1;
    }
}

static void trans_yuv_to_fb_format(ESEncVidInternalContext *in_ctx, ESEncVidContext *options) {
    uint8_t *transform_buf;
    uint32_t x, y;
    VCEncIn *pEncIn = &(in_ctx->enc_in);
    uint32_t alignment;
    uint32_t byte_per_compt = 0;
    uint32_t size_lum;

#ifdef USE_OLD_DRV
    transform_buf = (uint8_t *)in_ctx->transform_mem->virtualAddress;
#endif
    alignment = (in_ctx->input_alignment == 0 ? 1 : in_ctx->input_alignment);

    if (options->input_format == VCENC_YUV420_SEMIPLANAR || options->input_format == VCENC_YUV420_SEMIPLANAR_VU)
        byte_per_compt = 1;
    else if (options->input_format == VCENC_YUV420_PLANAR_10BIT_P010)
        byte_per_compt = 2;

    if ((options->input_format == VCENC_YUV420_SEMIPLANAR) || (options->input_format == VCENC_YUV420_SEMIPLANAR_VU)
        || (options->input_format == VCENC_YUV420_PLANAR_10BIT_P010)) {
        uint32_t stride;
        stride = (options->lum_width_src * 4 * byte_per_compt + alignment - 1) & (~(alignment - 1));

        // luma
        for (x = 0; x < options->lum_width_src / 4; x++) {
            for (y = 0; y < options->lum_height_src; y++) {
                memcpy(
                    transform_buf + y % 4 * 4 * byte_per_compt + stride * (y / 4) + x * 16 * byte_per_compt,
                    in_ctx->lum + y * ((options->lum_width_src + 15) & (~15)) * byte_per_compt + x * 4 * byte_per_compt,
                    4 * byte_per_compt);
            }
        }

        transform_buf += stride * options->lum_height_src / 4;

        // chroma
        for (x = 0; x < options->lum_width_src / 4; x++) {
            for (y = 0; y < ((options->lum_height_src / 2) + 3) / 4 * 4; y++) {
                memcpy(
                    transform_buf + y % 4 * 4 * byte_per_compt + stride * (y / 4) + x * 16 * byte_per_compt,
                    in_ctx->cb + y * ((options->lum_width_src + 15) & (~15)) * byte_per_compt + x * 4 * byte_per_compt,
                    4 * byte_per_compt);
            }
        }
    }

    size_lum = ((options->lum_width_src * 4 * byte_per_compt + alignment - 1) & (~(alignment - 1)))
               * options->lum_height_src / 4;

    pEncIn->busChromaU = pEncIn->busLuma + (uint32_t)size_lum;
#ifndef ESW_FF_ENHANCEMENT
#ifndef USE_OLD_DRV
    EWLTransDataRC2EP(in_ctx->ewl, in_ctx->transform_mem, in_ctx->transform_mem, in_ctx->transform_mem->size);
#endif
#endif
}

int32_t ff_read_gmv(ESEncVidInternalContext *in_ctx, VCEncIn *p_enc_in, ESEncVidContext *options) {
    int16_t i;

    for (i = 0; i < 2; i++) {
        p_enc_in->gmv[i][0] = options->gmv[i][0];
        p_enc_in->gmv[i][1] = options->gmv[i][1];
    }

    return 0;
}

static char *next_token(char *str) {
    char *p = strchr(str, ' ');
    if (p) {
        while (*p == ' ') p++;
        if (*p == '\0') p = NULL;
    }
    return p;
}

static int parse_gop_config_string(char *line, VCEncGopConfig *gop_cfg, int frame_idx, int gop_size) {
    if (!line) return -1;

    av_log(NULL, AV_LOG_INFO, "parserGOP, %s\n", line);

    // format: FrameN Type POC QPoffset QPfactor  num_ref_pics ref_pics  used_by_cur
    int frameN, poc, num_ref_pics, i;
    char type[10];
    VCEncGopPicConfig *cfg = NULL;
    VCEncGopPicSpecialConfig *scfg = NULL;

    // frame idx
    sscanf(line, "Frame%d", &frameN);
    if ((frameN != (frame_idx + 1)) && (frameN != 0)) return -1;

    if (frameN > gop_size) return 0;

    if (0 == frameN) {
        // format: FrameN Type  QPoffset  QPfactor   TemporalId  num_ref_pics   ref_pics  used_by_cur  LTR    Offset
        // Interval
        scfg = &(gop_cfg->pGopPicSpecialCfg[gop_cfg->special_size++]);

        // frame type
        line = next_token(line);
        if (!line) return -1;
        sscanf(line, "%s", type);
        scfg->nonReference = 0;
        if (strcmp(type, "I") == 0 || strcmp(type, "i") == 0)
            scfg->codingType = VCENC_INTRA_FRAME;
        else if (strcmp(type, "P") == 0 || strcmp(type, "p") == 0)
            scfg->codingType = VCENC_PREDICTED_FRAME;
        else if (strcmp(type, "B") == 0 || strcmp(type, "b") == 0)
            scfg->codingType = VCENC_BIDIR_PREDICTED_FRAME;
        /* P frame not for reference */
        else if (strcmp(type, "nrefP") == 0) {
            scfg->codingType = VCENC_PREDICTED_FRAME;
            scfg->nonReference = 1;
        }
        /* B frame not for reference */
        else if (strcmp(type, "nrefB") == 0) {
            scfg->codingType = VCENC_BIDIR_PREDICTED_FRAME;
            scfg->nonReference = 1;
        } else
            scfg->codingType = scfg->nonReference = FRAME_TYPE_RESERVED;

        // qp offset
        line = next_token(line);
        if (!line) return -1;
        sscanf(line, "%d", &(scfg->QpOffset));

        // qp factor
        line = next_token(line);
        if (!line) return -1;
        sscanf(line, "%lf", &(scfg->QpFactor));
        scfg->QpFactor = sqrt(scfg->QpFactor);

        // temporalId factor
        line = next_token(line);
        if (!line) return -1;
        sscanf(line, "%d", &(scfg->temporalId));

        // num_ref_pics
        line = next_token(line);
        if (!line) return -1;
        sscanf(line, "%d", &num_ref_pics);
        if (num_ref_pics > VCENC_MAX_REF_FRAMES) /* NUMREFPICS_RESERVED -1 */
        {
            av_log(
                NULL, AV_LOG_ERROR, "GOP Config: Error, num_ref_pic can not be more than %d \n", VCENC_MAX_REF_FRAMES);
            return -1;
        }
        scfg->numRefPics = num_ref_pics;

        if ((scfg->codingType == VCENC_INTRA_FRAME) && (0 == num_ref_pics)) num_ref_pics = 1;
        // ref_pics
        for (i = 0; i < num_ref_pics; i++) {
            line = next_token(line);
            if (!line) return -1;
            if ((strncmp(line, "L", 1) == 0) || (strncmp(line, "l", 1) == 0)) {
                sscanf(line, "%c%d", &type[0], &(scfg->refPics[i].ref_pic));
                scfg->refPics[i].ref_pic = LONG_TERM_REF_ID2DELTAPOC(scfg->refPics[i].ref_pic - 1);
            } else {
                sscanf(line, "%d", &(scfg->refPics[i].ref_pic));
            }
        }
        if (i < num_ref_pics) return -1;

        // used_by_cur
        for (i = 0; i < num_ref_pics; i++) {
            line = next_token(line);
            if (!line) return -1;
            sscanf(line, "%u", &(scfg->refPics[i].used_by_cur));
        }
        if (i < num_ref_pics) return -1;

        // LTR
        line = next_token(line);
        if (!line) return -1;
        sscanf(line, "%d", &scfg->i32Ltr);
        if (VCENC_MAX_LT_REF_FRAMES < scfg->i32Ltr) return -1;

        // Offset
        line = next_token(line);
        if (!line) return -1;
        sscanf(line, "%d", &scfg->i32Offset);

        // Interval
        line = next_token(line);
        if (!line) return -1;
        sscanf(line, "%d", &scfg->i32Interval);

        if (0 != scfg->i32Ltr) {
            gop_cfg->u32LTR_idx[gop_cfg->ltrcnt] = LONG_TERM_REF_ID2DELTAPOC(scfg->i32Ltr - 1);
            gop_cfg->ltrcnt++;
            if (VCENC_MAX_LT_REF_FRAMES < gop_cfg->ltrcnt) return -1;
        }

        // short_change
        scfg->i32short_change = 0;
        if (0 == scfg->i32Ltr) {
            /* not long-term ref */
            scfg->i32short_change = 1;
            for (i = 0; i < num_ref_pics; i++) {
                if (IS_LONG_TERM_REF_DELTAPOC(scfg->refPics[i].ref_pic) && (0 != scfg->refPics[i].used_by_cur)) {
                    scfg->i32short_change = 0;
                    break;
                }
            }
        }
    } else {
        // format: FrameN Type  POC  QPoffset    QPfactor   TemporalId  num_ref_pics  ref_pics  used_by_cur
        cfg = &(gop_cfg->pGopPicCfg[gop_cfg->size++]);

        // frame type
        line = next_token(line);
        if (!line) return -1;
        sscanf(line, "%s", type);
        cfg->nonReference = 0;
        if (strcmp(type, "P") == 0 || strcmp(type, "p") == 0)
            cfg->codingType = VCENC_PREDICTED_FRAME;
        else if (strcmp(type, "B") == 0 || strcmp(type, "b") == 0)
            cfg->codingType = VCENC_BIDIR_PREDICTED_FRAME;
        /* P frame not for reference */
        else if (strcmp(type, "nrefP") == 0) {
            cfg->codingType = VCENC_PREDICTED_FRAME;
            cfg->nonReference = 1;
        }
        /* B frame not for reference */
        else if (strcmp(type, "nrefB") == 0) {
            cfg->codingType = VCENC_BIDIR_PREDICTED_FRAME;
            cfg->nonReference = 1;
        } else
            return -1;

        // poc
        line = next_token(line);
        if (!line) return -1;
        sscanf(line, "%d", &poc);
        if (poc < 1 || poc > gop_size) return -1;
        cfg->poc = poc;

        // qp offset
        line = next_token(line);
        if (!line) return -1;
        sscanf(line, "%d", &(cfg->QpOffset));

        // qp factor
        line = next_token(line);
        if (!line) return -1;
        sscanf(line, "%lf", &(cfg->QpFactor));
        // sqrt(QpFactor) is used in calculating lambda
        cfg->QpFactor = sqrt(cfg->QpFactor);

        // temporalId factor
        line = next_token(line);
        if (!line) return -1;
        sscanf(line, "%d", &(cfg->temporalId));

        // num_ref_pics
        line = next_token(line);
        if (!line) return -1;
        sscanf(line, "%d", &num_ref_pics);
        if (num_ref_pics < 0 || num_ref_pics > VCENC_MAX_REF_FRAMES) {
            av_log(
                NULL, AV_LOG_ERROR, "GOP Config: Error, num_ref_pic can not be more than %d \n", VCENC_MAX_REF_FRAMES);
            return -1;
        }

        // ref_pics
        for (i = 0; i < num_ref_pics; i++) {
            line = next_token(line);
            if (!line) return -1;
            if ((strncmp(line, "L", 1) == 0) || (strncmp(line, "l", 1) == 0)) {
                sscanf(line, "%c%d", &type[0], &(cfg->refPics[i].ref_pic));
                cfg->refPics[i].ref_pic = LONG_TERM_REF_ID2DELTAPOC(cfg->refPics[i].ref_pic - 1);
            } else {
                sscanf(line, "%d", &(cfg->refPics[i].ref_pic));
            }
        }
        if (i < num_ref_pics) return -1;

        // used_by_cur
        for (i = 0; i < num_ref_pics; i++) {
            line = next_token(line);
            if (!line) return -1;
            sscanf(line, "%u", &(cfg->refPics[i].used_by_cur));
        }
        if (i < num_ref_pics) return -1;

        cfg->numRefPics = num_ref_pics;
    }

    return 0;
}

static int parse_gop_config_file(int gop_size, char *fname, VCEncGopConfig *gop_cfg) {
#define MAX_LINE_LENGTH 1024
    int frame_idx = 0, line_idx = 0, add_tmp;
    char ach_parser_buffer[MAX_LINE_LENGTH];
    FILE *fIn = fopen(fname, "r");
    if (fIn == NULL) {
        av_log(NULL, AV_LOG_ERROR, "GOP Config: Error, Can Not Open File %s\n", fname);
        return -1;
    }

    while (0 == feof(fIn)) {
        char *line;
        char *s;
        if (feof(fIn)) break;
        line_idx++;
        ach_parser_buffer[0] = '\0';
        // Read one line
        line = fgets((char *)ach_parser_buffer, MAX_LINE_LENGTH, fIn);
        if (!line) break;
        // handle line end
        s = strpbrk(line, "#\n");
        if (s) *s = '\0';

        add_tmp = 1;
        line = strstr(line, "Frame");
        if (line) {
            if (0 == strncmp(line, "Frame0", 6)) add_tmp = 0;

            if (parse_gop_config_string(line, gop_cfg, frame_idx, gop_size) < 0) {
                av_log(NULL, AV_LOG_ERROR, "Invalid gop configure!\n");
                return -1;
            }

            frame_idx += add_tmp;
        }
    }

    fclose(fIn);
    if (frame_idx != gop_size) {
        av_log(NULL, AV_LOG_ERROR, "GOP Config: Error, Parsing File %s Failed at Line %d\n", fname, line_idx);
        return -1;
    }
    return 0;
}

static int read_gop_config(char *fname, char **config, VCEncGopConfig *gop_cfg, int gop_size, uint8_t *gop_cfg_offset) {
    int ret = -1;

    if (gop_cfg->size >= MAX_GOP_PIC_CONFIG_NUM) return -1;

    if (gop_cfg_offset) gop_cfg_offset[gop_size] = gop_cfg->size;
    if (fname) {
        ret = parse_gop_config_file(gop_size, fname, gop_cfg);
    } else if (config) {
        int id = 0;
        while (config[id]) {
            parse_gop_config_string(config[id], gop_cfg, id, gop_size);
            id++;
        }
        ret = 0;
    }
    return ret;
}

int ff_init_gop_configs(int gopSize,
                        ESEncVidContext *options,
                        VCEncGopConfig *gopCfg,
                        uint8_t *gopCfgOffset,
                        bool bPass2,
                        uint32_t hwId) {
    int i, pre_load_num;
    char *fname = options->gop_cfg;
    VCEncGopPicConfig *cfgStart = NULL;

    uint32_t singleRefForP =
        (options->lookahead_depth && !bPass2) || (options->num_refP == 1);  // not enable multiref for P in pass-1

    const char **rpsDefaultGop1 = singleRefForP ? RpsDefault_GOPSize_1 : Rps_2RefForP_GOPSize_1;
    if (IS_H264(options->codec_format))
        rpsDefaultGop1 = singleRefForP ? RpsDefault_H264_GOPSize_1 : Rps_2RefForP_H264_GOPSize_1;
    else if (HW_PRODUCT_SYSTEM60(hwId) || HW_PRODUCT_VC9000LE(hwId))
        rpsDefaultGop1 = RpsDefault_V60_GOPSize_1;

    const char **default_configs[16] = {options->gop_lowdelay ? RpsLowdelayDefault_GOPSize_1 : rpsDefaultGop1,
                                        options->gop_lowdelay ? RpsLowdelayDefault_GOPSize_2
                                        : singleRefForP       ? RpsDefault_GOPSize_2
                                                              : Rps_2RefForP_GOPSize_2,
                                        options->gop_lowdelay ? RpsLowdelayDefault_GOPSize_3
                                        : singleRefForP       ? RpsDefault_GOPSize_3
                                                              : Rps_2RefForP_GOPSize_3,
                                        options->gop_lowdelay ? RpsLowdelayDefault_GOPSize_4
                                        : singleRefForP       ? RpsDefault_GOPSize_4
                                                              : Rps_2RefForP_GOPSize_4,
                                        singleRefForP ? RpsDefault_GOPSize_5 : Rps_2RefForP_GOPSize_5,
                                        singleRefForP ? RpsDefault_GOPSize_6 : Rps_2RefForP_GOPSize_6,
                                        singleRefForP ? RpsDefault_GOPSize_7 : Rps_2RefForP_GOPSize_7,
                                        singleRefForP ? RpsDefault_GOPSize_8 : Rps_2RefForP_GOPSize_8,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        RpsDefault_GOPSize_16};

    if (gopSize < 0 || gopSize > MAX_GOP_SIZE
        || (gopSize > 0 && default_configs[gopSize - 1] == NULL && fname == NULL)) {
        av_log(NULL, AV_LOG_ERROR, "GOP Config: Error, Invalid GOP Size\n");
        return -1;
    }

    // use lowdelay B for pass2 only if multi-reference is not support.
    if (bPass2 && (options->num_refP == 1)) {
        default_configs[1] = RpsPass2_GOPSize_2;
        default_configs[3] = RpsPass2_GOPSize_4;
        default_configs[7] = RpsPass2_GOPSize_8;
    }

    // Handle Interlace
    if (options->interlaced_frame && gopSize == 1) {
        default_configs[0] = RpsDefault_Interlace_GOPSize_1;
    }

    // GOP size in rps array for gopSize=N
    // N<=4:      GOP1, ..., GOPN
    // 4<N<=8:    GOP1, GOP2, GOP3, GOP4, GOPN
    // N > 8:     GOP1, GOP2, GOP3, GOP4, GOPN
    // Adaptive:  GOP1, GOP2, GOP3, GOP4, GOP6, GOP8
    if (gopSize > 8)
        pre_load_num = 4;
    else if (gopSize >= 4 || gopSize == 0)
        pre_load_num = 4;
    else
        pre_load_num = gopSize;

    gopCfg->special_size = 0;
    gopCfg->ltrcnt = 0;

    for (i = 1; i <= pre_load_num; i++) {
        if (read_gop_config(gopSize == i ? fname : NULL, (char **)default_configs[i - 1], gopCfg, i, gopCfgOffset))
            return -1;
    }

    if (gopSize == 0) {
        // gop6
        if (read_gop_config(NULL, (char **)default_configs[5], gopCfg, 6, gopCfgOffset)) return -1;
        // gop8
        if (read_gop_config(NULL, (char **)default_configs[7], gopCfg, 8, gopCfgOffset)) return -1;
    } else if (gopSize > 4) {
        // gopSize
        if (read_gop_config(fname, (char **)default_configs[gopSize - 1], gopCfg, gopSize, gopCfgOffset)) return -1;
    }

    if ((DEFAULT != options->ltr_interval) && (gopCfg->special_size == 0)) {
        if (options->gop_size != 1) {
            av_log(NULL,
                   AV_LOG_ERROR,
                   "GOP Config: Error, when using --LTR configure option, the gopsize alse should be set to 1!\n");
            return -1;
        }
        gopCfg->pGopPicSpecialCfg[0].poc = 0;
        gopCfg->pGopPicSpecialCfg[0].QpOffset = options->long_term_qp_delta;
        gopCfg->pGopPicSpecialCfg[0].QpFactor = QPFACTOR_RESERVED;
        gopCfg->pGopPicSpecialCfg[0].temporalId = TEMPORALID_RESERVED;
        gopCfg->pGopPicSpecialCfg[0].codingType = FRAME_TYPE_RESERVED;
        gopCfg->pGopPicSpecialCfg[0].numRefPics = NUMREFPICS_RESERVED;
        gopCfg->pGopPicSpecialCfg[0].i32Ltr = 1;
        gopCfg->pGopPicSpecialCfg[0].i32Offset = 0;
        gopCfg->pGopPicSpecialCfg[0].i32Interval = options->ltr_interval;
        gopCfg->pGopPicSpecialCfg[0].i32short_change = 0;
        gopCfg->u32LTR_idx[0] = LONG_TERM_REF_ID2DELTAPOC(0);

        gopCfg->pGopPicSpecialCfg[1].poc = 0;
        gopCfg->pGopPicSpecialCfg[1].QpOffset = QPOFFSET_RESERVED;
        gopCfg->pGopPicSpecialCfg[1].QpFactor = QPFACTOR_RESERVED;
        gopCfg->pGopPicSpecialCfg[1].temporalId = TEMPORALID_RESERVED;
        gopCfg->pGopPicSpecialCfg[1].codingType = FRAME_TYPE_RESERVED;
        gopCfg->pGopPicSpecialCfg[1].numRefPics = 2;
        gopCfg->pGopPicSpecialCfg[1].refPics[0].ref_pic = -1;
        gopCfg->pGopPicSpecialCfg[1].refPics[0].used_by_cur = 1;
        gopCfg->pGopPicSpecialCfg[1].refPics[1].ref_pic = LONG_TERM_REF_ID2DELTAPOC(0);
        gopCfg->pGopPicSpecialCfg[1].refPics[1].used_by_cur = 1;
        gopCfg->pGopPicSpecialCfg[1].i32Ltr = 0;
        gopCfg->pGopPicSpecialCfg[1].i32Offset = options->long_term_gap_offset;
        gopCfg->pGopPicSpecialCfg[1].i32Interval = options->long_term_gap;
        gopCfg->pGopPicSpecialCfg[1].i32short_change = 0;

        gopCfg->special_size = 2;
        gopCfg->ltrcnt = 1;
    }

    CLIENT_TYPE client_type = IS_H264(options->codec_format) ? EWL_CLIENT_TYPE_H264_ENC : EWL_CLIENT_TYPE_HEVC_ENC;
    u32 hw_id = EncAsicGetAsicHWid(client_type, NULL);
    if (0) {
        for (i = 0; i < (gopSize == 0 ? gopCfg->size : gopCfgOffset[gopSize]); i++) {
            // when use long-term, change P to B in default configs (used for last gop)
            VCEncGopPicConfig *cfg = &(gopCfg->pGopPicCfg[i]);
            if (cfg->codingType == VCENC_PREDICTED_FRAME) cfg->codingType = VCENC_BIDIR_PREDICTED_FRAME;
        }
    }

    /* 6.0 software: merge */
    if (hw_id < 0x80006010 && IS_H264(options->codec_format) && gopCfg->ltrcnt > 0) {
        av_log(NULL, AV_LOG_ERROR, "GOP Config: Error, H264 LTR not supported before 6.0.10!\n");
        return -1;
    }

    // Compatible with old bFrameQpDelta setting
    if (options->b_frame_qp_delta >= 0 && fname == NULL) {
        for (i = 0; i < gopCfg->size; i++) {
            VCEncGopPicConfig *cfg = &(gopCfg->pGopPicCfg[i]);
            if (cfg->codingType == VCENC_BIDIR_PREDICTED_FRAME) cfg->QpOffset = options->b_frame_qp_delta;
        }
    }

    // lowDelay auto detection
    cfgStart = &(gopCfg->pGopPicCfg[gopCfgOffset[gopSize]]);
    if (gopSize == 1) {
        options->gop_lowdelay = 1;
    } else if ((gopSize > 1) && (options->gop_lowdelay == 0)) {
        options->gop_lowdelay = 1;
        for (i = 1; i < gopSize; i++) {
            if (cfgStart[i].poc < cfgStart[i - 1].poc) {
                options->gop_lowdelay = 0;
                break;
            }
        }
    }

#ifdef INTERNAL_TEST
    if ((options->testId == TID_POC && gopSize == 1) && !IS_H264(options->codecFormat) && !IS_AV1(options->codecFormat)
        && !IS_VP9(options->codecFormat)) {
        VCEncGopPicConfig *cfg = &(gopCfg->pGopPicCfg[0]);
        if (cfg->numRefPics == 2) cfg->refPics[1].ref_pic = -(options->intraPicRate - 1);
    }
#endif

    {
        i32 i32LtrPoc[VCENC_MAX_LT_REF_FRAMES];
        i32 i32LtrIndex = 0;

        for (i = 0; i < VCENC_MAX_LT_REF_FRAMES; i++) i32LtrPoc[i] = -1;
        for (i = 0; i < gopCfg->special_size; i++) {
            if (gopCfg->pGopPicSpecialCfg[i].i32Ltr > VCENC_MAX_LT_REF_FRAMES) {
                av_log(NULL, AV_LOG_ERROR, "GOP Config: Error, Invalid long-term index\n");
                return -1;
            }
            if (gopCfg->pGopPicSpecialCfg[i].i32Ltr > 0) {
                i32LtrPoc[i32LtrIndex] = gopCfg->pGopPicSpecialCfg[i].i32Ltr - 1;
                i32LtrIndex++;
            }
        }

        for (i = 0; i < gopCfg->ltrcnt; i++) {
            if ((0 != i32LtrPoc[0]) || (-1 == i32LtrPoc[i]) || ((i > 0) && i32LtrPoc[i] != (i32LtrPoc[i - 1] + 1))) {
                av_log(NULL, AV_LOG_ERROR, "GOP Config: Error, Invalid long-term index\n");
                return -1;
            }
        }
    }

    // For lowDelay, Handle the first few frames that miss reference frame
    if (1) {
        int nGop;
        int idx = 0;
        int maxErrFrame = 0;
        VCEncGopPicConfig *cfg;

        // Find the max frame number that will miss its reference frame defined in rps
        while ((idx - maxErrFrame) < gopSize) {
            nGop = (idx / gopSize) * gopSize;
            cfg = &(cfgStart[idx % gopSize]);

            for (i = 0; i < cfg->numRefPics; i++) {
                // POC of this reference frame
                int refPoc = cfg->refPics[i].ref_pic + cfg->poc + nGop;
                if (refPoc < 0) {
                    maxErrFrame = idx + 1;
                }
            }
            idx++;
        }

        // Try to config a new rps for each "error" frame by modifying its original rps
        for (idx = 0; idx < maxErrFrame; idx++) {
            int j, iRef, nRefsUsedByCur, nPoc;
            VCEncGopPicConfig *cfgCopy;

            if (gopCfg->size >= MAX_GOP_PIC_CONFIG_NUM) break;

            // Add to array end
            cfg = &(gopCfg->pGopPicCfg[gopCfg->size]);
            cfgCopy = &(cfgStart[idx % gopSize]);
            memcpy(cfg, cfgCopy, sizeof(VCEncGopPicConfig));
            gopCfg->size++;

            // Copy reference pictures
            nRefsUsedByCur = iRef = 0;
            nPoc = cfgCopy->poc + ((idx / gopSize) * gopSize);
            for (i = 0; i < cfgCopy->numRefPics; i++) {
                int newRef = 1;
                int used_by_cur = cfgCopy->refPics[i].used_by_cur;
                int ref_pic = cfgCopy->refPics[i].ref_pic;
                // Clip the reference POC
                if ((cfgCopy->refPics[i].ref_pic + nPoc) < 0) ref_pic = 0 - (nPoc);

                // Check if already have this reference
                for (j = 0; j < iRef; j++) {
                    if (cfg->refPics[j].ref_pic == ref_pic) {
                        newRef = 0;
                        if (used_by_cur) cfg->refPics[j].used_by_cur = used_by_cur;
                        break;
                    }
                }

                // Copy this reference
                if (newRef) {
                    cfg->refPics[iRef].ref_pic = ref_pic;
                    cfg->refPics[iRef].used_by_cur = used_by_cur;
                    iRef++;
                }
            }
            cfg->numRefPics = iRef;
            // If only one reference frame, set P type.
            for (i = 0; i < cfg->numRefPics; i++) {
                if (cfg->refPics[i].used_by_cur) nRefsUsedByCur++;
            }
            if (nRefsUsedByCur == 1) cfg->codingType = VCENC_PREDICTED_FRAME;
        }
    }

#if 1
    // print for debug
    int idx;
    av_log(NULL, AV_LOG_INFO, "====== REF PICTURE SETS from %s ======\n", fname ? fname : "DEFAULT");
    for (idx = 0; idx < gopCfg->size; idx++) {
        int i;
        VCEncGopPicConfig *cfg = &(gopCfg->pGopPicCfg[idx]);
        char type = cfg->codingType == VCENC_PREDICTED_FRAME ? 'P' : cfg->codingType == VCENC_INTRA_FRAME ? 'I' : 'B';
        av_log(NULL,
               AV_LOG_INFO,
               " FRAME%2d:  %c %d %d %f %d",
               idx,
               type,
               cfg->poc,
               cfg->QpOffset,
               cfg->QpFactor,
               cfg->numRefPics);
        for (i = 0; i < cfg->numRefPics; i++) av_log(NULL, AV_LOG_INFO, " %d", cfg->refPics[i].ref_pic);
        for (i = 0; i < cfg->numRefPics; i++) av_log(NULL, AV_LOG_INFO, " %d", cfg->refPics[i].used_by_cur);
        av_log(NULL, AV_LOG_INFO, "\n");
    }
    av_log(NULL, AV_LOG_INFO, "===========================================\n");
#endif
    return 0;
}

/*------------------------------------------------------------------------------

 read_user_data
 Read user data from file and pass to encoder

 Params:
 name - name of file in which user data is located

 Returns:
 NULL - when user data reading failed
 pointer - allocated buffer containing user data

 ------------------------------------------------------------------------------*/
uint8_t *ff_read_user_data(VCEncInst encoder, char *name) {
    FILE *file = NULL;
    int32_t byte_cnt;
    uint8_t *data;

    if (name == NULL) return NULL;

    if (strcmp("0", name) == 0) return NULL;

    /* Get user data length from FILE */
    file = fopen(name, "rb");
    if (file == NULL) {
        av_log(NULL, AV_LOG_ERROR, "Unable to open User Data file: %s\n", name);
        return NULL;
    }
    fseeko(file, 0L, SEEK_END);
    byte_cnt = ftell(file);
    rewind(file);

    /* Minimum size of user data */
    if (byte_cnt < 16) byte_cnt = 16;

    /* Maximum size of user data */
    if (byte_cnt > 2048) byte_cnt = 2048;
    /* Allocate memory for user data */
    if ((data = (uint8_t *)malloc(sizeof(uint8_t) * byte_cnt)) == NULL) {
        fclose(file);
        av_log(NULL, AV_LOG_ERROR, "Unable to alloc User Data memory\n");
        return NULL;
    }

    /* Read user data from FILE */
    fread(data, sizeof(uint8_t), byte_cnt, file);
    fclose(file);

    av_log(NULL, AV_LOG_ERROR, "User data: %d bytes [%d %d %d %d ...]\n", byte_cnt, data[0], data[1], data[2], data[3]);
    /* Pass the data buffer to encoder
     * The encoder reads the buffer during following VCEncStrmEncode() calls.
     * User data writing must be disabled (with VCEncSetSeiUserData(enc, 0, 0)) */
    VCEncSetSeiUserData(encoder, data, byte_cnt);

    return data;
}

/*------------------------------------------------------------------------------
 Calculate average bitrate of moving window
 ------------------------------------------------------------------------------*/
int32_t ff_ma(Ma *ma) {
    int32_t i;
    unsigned long long sum = 0; /* Using 64-bits to avoid overflow */

    for (i = 0; i < ma->count; i++) sum += ma->frame[i];

    if (!ma->frame_rate_denom) return 0;

    sum = sum / ma->count;

    return sum * (ma->frame_rate_numer + ma->frame_rate_denom - 1) / ma->frame_rate_denom;
}

void ff_ma_add_frame(Ma *ma, int32_t frame_size_bits) {
    ma->frame[ma->pos++] = frame_size_bits;

    if (ma->pos == ma->length) ma->pos = 0;

    if (ma->count < ma->length) ma->count++;
}

static int32_t adaptive_gop_decision(
    ESEncVidInternalContext *in_ctx, VCEncIn *p_enc_in, VCEncInst encoder, int32_t *next_gop_size, AdapGopCtr *agop) {
    int32_t gop_size = -1;

    struct vcenc_instance *vcenc_instance = (struct vcenc_instance *)encoder;
    unsigned int ui_intra_cu8_num = vcenc_instance->asic.regs.intraCu8Num;
    unsigned int ui_skip_cu8_num = vcenc_instance->asic.regs.skipCu8Num;
    unsigned int ui_pb_frame_cost = vcenc_instance->asic.regs.PBFrame4NRdCost;
    double d_intra_vs_inter_skip = (double)ui_intra_cu8_num / (double)((in_ctx->width / 8) * (in_ctx->height / 8));
    double d_skip_vs_inter_skip = (double)ui_skip_cu8_num / (double)((in_ctx->width / 8) * (in_ctx->height / 8));

    agop->gop_frm_num++;
    agop->sum_intra_vs_interskip += d_intra_vs_inter_skip;
    agop->sum_skip_vs_interskip += d_skip_vs_inter_skip;
    agop->sum_costP += (p_enc_in->codingType == VCENC_PREDICTED_FRAME) ? ui_pb_frame_cost : 0;
    agop->sum_costB += (p_enc_in->codingType == VCENC_BIDIR_PREDICTED_FRAME) ? ui_pb_frame_cost : 0;
    agop->sum_intra_vs_interskipP += (p_enc_in->codingType == VCENC_PREDICTED_FRAME) ? d_intra_vs_inter_skip : 0;
    agop->sum_intra_vs_interskipB += (p_enc_in->codingType == VCENC_BIDIR_PREDICTED_FRAME) ? d_intra_vs_inter_skip : 0;

    if (p_enc_in->gopPicIdx
        == p_enc_in->gopSize - 1) {  // last frame of the current gop. decide the gopsize of next gop.
        d_intra_vs_inter_skip = agop->sum_intra_vs_interskip / agop->gop_frm_num;
        d_skip_vs_inter_skip = agop->sum_skip_vs_interskip / agop->gop_frm_num;
        agop->sum_costB = (agop->gop_frm_num > 1) ? (agop->sum_costB / (agop->gop_frm_num - 1)) : 0xFFFFFFF;
        agop->sum_intra_vs_interskipB =
            (agop->gop_frm_num > 1) ? (agop->sum_intra_vs_interskipB / (agop->gop_frm_num - 1)) : 0xFFFFFFF;
        // Enabled adaptive GOP size for large resolution
        if (((in_ctx->width * in_ctx->height) >= (1280 * 720))
            || ((MAX_ADAPTIVE_GOP_SIZE > 3) && ((in_ctx->width * in_ctx->height) >= (416 * 240)))) {
            if ((((double)agop->sum_costP / (double)agop->sum_costB) < 1.1) && (d_skip_vs_inter_skip >= 0.95)) {
                agop->last_gop_size = gop_size = 1;
            } else if (((double)agop->sum_costP / (double)agop->sum_costB) > 5) {
                gop_size = agop->last_gop_size;
            } else {
                if (((agop->sum_intra_vs_interskipP > 0.40) && (agop->sum_intra_vs_interskipP < 0.70)
                     && (agop->sum_intra_vs_interskipB < 0.10))) {
                    agop->last_gop_size++;
                    if (agop->last_gop_size == 5 || agop->last_gop_size == 7) agop->last_gop_size++;
                    agop->last_gop_size = MIN(agop->last_gop_size, MAX_ADAPTIVE_GOP_SIZE);
                    gop_size = agop->last_gop_size;  //
                } else if (d_intra_vs_inter_skip >= 0.30) {
                    agop->last_gop_size = gop_size = 1;  // No B
                } else if (d_intra_vs_inter_skip >= 0.20) {
                    agop->last_gop_size = gop_size = 2;  // One B
                } else if (d_intra_vs_inter_skip >= 0.10) {
                    agop->last_gop_size--;
                    if (agop->last_gop_size == 5 || agop->last_gop_size == 7) agop->last_gop_size--;
                    agop->last_gop_size = MAX(agop->last_gop_size, 3);
                    gop_size = agop->last_gop_size;  //
                } else {
                    agop->last_gop_size++;
                    if (agop->last_gop_size == 5 || agop->last_gop_size == 7) agop->last_gop_size++;
                    agop->last_gop_size = MIN(agop->last_gop_size, MAX_ADAPTIVE_GOP_SIZE);
                    gop_size = agop->last_gop_size;  //
                }
            }
        } else {
            gop_size = 3;
        }
        agop->gop_frm_num = 0;
        agop->sum_intra_vs_interskip = 0;
        agop->sum_skip_vs_interskip = 0;
        agop->sum_costP = 0;
        agop->sum_costB = 0;
        agop->sum_intra_vs_interskipP = 0;
        agop->sum_intra_vs_interskipB = 0;

        gop_size = MIN(gop_size, MAX_ADAPTIVE_GOP_SIZE);
    }

    if (gop_size != -1) *next_gop_size = gop_size;

    return gop_size;
}

int32_t ff_get_next_gop_size(
    ESEncVidInternalContext *in_ctx, VCEncIn *p_enc_in, VCEncInst encoder, int32_t *next_gop_size, AdapGopCtr *agop) {
    struct vcenc_instance *vcenc_instance = (struct vcenc_instance *)encoder;
#ifndef ESW_FF_ENHANCEMENT
    if (vcenc_instance->lookaheadDepth) {
        int32_t upd_gop = getPass1UpdatedGopSize(vcenc_instance->lookahead.priv_inst);
        if (upd_gop) *next_gop_size = upd_gop;
    } else if (p_enc_in->codingType != VCENC_INTRA_FRAME) {
        adaptive_gop_decision(in_ctx, p_enc_in, encoder, next_gop_size, agop);
    }
#endif
    return *next_gop_size;
}

#if defined(SUPPORT_DEC400) || defined(SUPPORT_TCACHE)
#ifndef ESW_FF_ENHANCEMENT
#include "fb_ips.h"
#include "dec400_f2_api.h"
#include "tcache_api.h"
#include "trans_edma_api.h"
#include "dtrc_api.h"
#include "l2cache_api.h"
#endif
#define VCE_HEIGHT_ALIGNMENT 64
#define VCE_INPUT_ALIGNMENT 32
#endif

int32_t ff_change_format_for_fb(ESEncVidInternalContext *in_ctx,
                                ESEncVidContext *options,
                                VCEncPreProcessingCfg *pre_proc_cfg) {
    switch (options->input_format) {
#ifdef SUPPORT_DEC400
        // for dec400
#ifndef ESW_FF_ENHANCEMENT
        case INPUT_FORMAT_YUV420_SEMIPLANAR_8BIT_COMPRESSED_FB:
            pre_proc_cfg->inputType = VCENC_YUV420_SEMIPLANAR_8BIT_FB;
            pre_proc_cfg->input_alignment = 1024;
            break;
        case INPUT_FORMAT_YUV420_SEMIPLANAR_VU_8BIT_COMPRESSED_FB:
            pre_proc_cfg->inputType = VCENC_YUV420_SEMIPLANAR_VU_8BIT_FB;
            pre_proc_cfg->input_alignment = 1024;
            break;
        case INPUT_FORMAT_YUV420_PLANAR_10BIT_P010_COMPRESSED_FB:
            pre_proc_cfg->inputType = VCENC_YUV420_PLANAR_10BIT_P010_FB;
            pre_proc_cfg->input_alignment = 1024;
            break;
#endif
#endif
#ifdef SUPPORT_TCACHE
        // for tcache
        case VCENC_YUV420_PLANAR:
        case VCENC_YUV420_SEMIPLANAR:
        case VCENC_YUV420_SEMIPLANAR_VU:
        case INPUT_FORMAT_YUV422P:
        case INPUT_FORMAT_YUV444P:
        case INPUT_FORMAT_RFC_8BIT_COMPRESSED_FB:
            pre_proc_cfg->inputType = VCENC_YUV420_SEMIPLANAR;
            pre_proc_cfg->input_alignment = 32;
            break;
        case VCENC_YUV420_PLANAR_10BIT_P010: /*this is semiplaner P010LE*/
        case INPUT_FORMAT_YUV420_SEMIPLANAR_10BIT_P010BE:
        case INPUT_FORMAT_YUV420_PLANAR_10BIT_P010BE:
        case INPUT_FORMAT_YUV420_PLANAR_10BIT_P010LE:
        case INPUT_FORMAT_YUV422P10LE:
        case INPUT_FORMAT_YUV422P10BE:
        case INPUT_FORMAT_RFC_10BIT_COMPRESSED_FB:
            pre_proc_cfg->inputType = VCENC_YUV420_PLANAR_10BIT_P010;
            pre_proc_cfg->input_alignment = 32;
            break;
        case INPUT_FORMAT_ARGB_FB:
        case INPUT_FORMAT_ABGR_FB:
        case INPUT_FORMAT_BGRA_FB:
        case INPUT_FORMAT_RGBA_FB:
        case INPUT_FORMAT_BGR24_FB:
        case INPUT_FORMAT_RGB24_FB:
            pre_proc_cfg->inputType =
                (options->bit_depth_luma == 10) ? VCENC_YUV420_PLANAR_10BIT_P010 : VCENC_YUV420_SEMIPLANAR;
            pre_proc_cfg->input_alignment = 32;
            break;
#else
        case VCENC_YUV420_SEMIPLANAR:
        case VCENC_YUV420_PLANAR_10BIT_P010:
            // don't change
            break;
#endif
#ifndef ESW_FF_ENHANCEMENT
        case INPUT_FORMAT_PP_YUV420_SEMIPLANNAR:
        case INPUT_FORMAT_PP_YUV420_SEMIPLANNAR_VU:
            pre_proc_cfg->inputType = VCENC_YUV420_SEMIPLANAR;
            // pre_proc_cfg->input_alignment = 32; //depend on the options option
            break;
        case INPUT_FORMAT_PP_YUV420_PLANAR_10BIT_P010:
            pre_proc_cfg->inputType = VCENC_YUV420_PLANAR_10BIT_P010;
            pre_proc_cfg->input_alignment = 32;
            break;
#endif
        default:
            break;
    }
    av_log(NULL,
           AV_LOG_DEBUG,
           "::::: format %d -> %d, input alignment change to %d\n",
           options->input_format,
           pre_proc_cfg->inputType,
           pre_proc_cfg->input_alignment);
    return 0;
}

FILE *ff_format_customized_yuv(ESEncVidInternalContext *in_ctx, ESEncVidContext *options) {
    if ((options->format_customized_type == 1)
        && ((options->input_format == VCENC_YUV420_SEMIPLANAR) || (options->input_format == VCENC_YUV420_SEMIPLANAR_VU)
            || (options->input_format == VCENC_YUV420_PLANAR_10BIT_P010)))
        trans_yuv_to_fb_format(in_ctx, options);
    return NULL;
}

void ff_init_slice_ctl(ESEncVidInternalContext *in_ctx, ESEncVidContext *options) {
    int i;
    for (i = 0; i < MAX_CORE_NUM; i++) {
        in_ctx->slice_ctl_factory[i].multislice_encoding =
            (options->slice_size != 0 && (((options->height + 63) / 64) > options->slice_size)) ? 1 : 0;
        in_ctx->slice_ctl_factory[i].output_byte_stream = options->byte_stream ? 1 : 0;
        in_ctx->slice_ctl_factory[i].out_stream_file = in_ctx->out;
        in_ctx->slice_ctl_factory[i].stream_pos = 0;
    }
}

void ff_init_stream_segment_ctl(ESEncVidInternalContext *in_ctx, ESEncVidContext *options) {
    in_ctx->stream_seg_ctl.stream_rd_counter = 0;
    in_ctx->stream_seg_ctl.stream_multi_seg_en = options->stream_multi_segment_mode != 0;
#ifdef USE_OLD_DRV
    in_ctx->stream_seg_ctl.stream_base = (uint8_t *)in_ctx->outbuf_mem_factory[0][0].virtualAddress;
#else
    in_ctx->stream_seg_ctl.stream_base = (uint8_t *)in_ctx->outbuf_mem_factory[0][0].virtualAddress;
#endif

    if (in_ctx->stream_seg_ctl.stream_multi_seg_en) {
        in_ctx->stream_seg_ctl.segment_size =
            in_ctx->outbuf_mem_factory[0][0].size / options->stream_multi_segment_amount;
        in_ctx->stream_seg_ctl.segment_size =
            ((in_ctx->stream_seg_ctl.segment_size + 16 - 1) & (~(16 - 1)));  // segment size must be aligned to 16byte
        in_ctx->stream_seg_ctl.segment_amount = options->stream_multi_segment_amount;
    }
    in_ctx->stream_seg_ctl.start_code_done = 0;
    in_ctx->stream_seg_ctl.output_byte_stream = in_ctx->byte_stream;
    in_ctx->stream_seg_ctl.out_stream_file = in_ctx->out;
}

void ff_setup_slice_ctl(ESEncVidInternalContext *in_ctx) {
    // find transform buffer of multi-cores
    in_ctx->slice_ctl = &(in_ctx->slice_ctl_factory[in_ctx->picture_enc_cnt % in_ctx->parallel_core_num]);
    in_ctx->slice_ctl_out = &(in_ctx->slice_ctl_factory[(in_ctx->picture_enc_cnt + 1) % in_ctx->parallel_core_num]);
}

// Helper function to calculate time diffs.
unsigned int ff_utime_diff(struct timeval end, struct timeval start) {
    return (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
}
