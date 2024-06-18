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
#include "libavutil/opt.h"
#include "hwconfig.h"
#include "esenc_vid.h"
#include "codec_internal.h"

#define OFFSET(x) offsetof(ESEncVidContext, x)
#define FLAGS (AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_EXPORT)

// define common options for h264 and h265 encoder
#define ESENC_H26X_OPTS                                                                                                \
        {"stream_type",                                                                                                \
        "Set stream byte. 0: NAL UNIT, 1: byte stream",                                                                \
        OFFSET(byte_stream),                                                                                           \
        AV_OPT_TYPE_INT,                                                                                               \
        {.i64 = 1},                                                                                                    \
        0,                                                                                                             \
        1,                                                                                                             \
        FLAGS},                                                                                                        \
        {"idr_interval", "Intra frame interval", OFFSET(intra_pic_rate), AV_OPT_TYPE_INT, {.i64 = 30}, 0, 120, FLAGS}, \
        {"bitdepth",                                                                                                   \
         "Bitdepth. 8=8-bit, 10=10-bit.",                                                                              \
         OFFSET(bitdepth),                                                                                             \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = 8},                                                                                                   \
         8,                                                                                                            \
         10,                                                                                                           \
         FLAGS,                                                                                                        \
         "bitdepth"},                                                                                                  \
        {"rotation",                                                                                                   \
         "pre-processor, rotation. 0=0 degree, 1=right 90 degree, 2=left 90 degree, 3=right 180 degree.",              \
         OFFSET(rotation),                                                                                             \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = 0},                                                                                                   \
         0,                                                                                                            \
         3,                                                                                                            \
         FLAGS,                                                                                                        \
         "rotation"},                                                                                                  \
        {"0", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_ROTATE_0}, 0, 0, FLAGS, "rotation"},                             \
        {"90R", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_ROTATE_90R}, 0, 0, FLAGS, "rotation"},                         \
        {"90L", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_ROTATE_90L}, 0, 0, FLAGS, "rotation"},                         \
        {"180R", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_ROTATE_180R}, 0, 0, FLAGS, "rotation"},                       \
        {"crop",                                                                                                       \
         "crop 'cx:N,cy:N,cw:N,ch:N',mean crop xoffset,yoffset,out_width,out_heigh",                                   \
         OFFSET(crop_str),                                                                                             \
         AV_OPT_TYPE_STRING,                                                                                           \
         {.str = "cx:0,cy:0,cw:0,ch:0"},                                                                               \
         0,                                                                                                            \
         0,                                                                                                            \
         FLAGS},                                                                                                       \
        {"rc_mode",                                                                                                    \
         "Set RC mode, CVBR: 0, CBR:1, VBR:2, ABR:3, CRF:4, CQP:5",                                                    \
         OFFSET(rc_mode),                                                                                              \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = 0},                                                                                                   \
         0,                                                                                                            \
         5,                                                                                                            \
         FLAGS,                                                                                                        \
         "RC"},                                                                                                        \
        {"cvbr", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCE_RC_CVBR}, 0, 0, FLAGS, "RC"},                                   \
        {"cbr", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCE_RC_CBR}, 0, 0, FLAGS, "RC"},                                     \
        {"vbr", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCE_RC_VBR}, 0, 0, FLAGS, "RC"},                                     \
        {"abr", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCE_RC_ABR}, 0, 0, FLAGS, "RC"},                                     \
        {"crf", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCE_RC_CRF}, 0, 0, FLAGS, "RC"},                                     \
        {"cqp", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCE_RC_CQP}, 0, 0, FLAGS, "RC"},                                     \
        {"rc_window",                                                                                                  \
         "Bitrate window length in frames [1..300]",                                                                   \
         OFFSET(bitrate_window),                                                                                       \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = 100},                                                                                                 \
         1,                                                                                                            \
         300,                                                                                                          \
         FLAGS},                                                                                                       \
        {"fixed_qp_I",                                                                                                 \
         "the QP for I frame while rc mode is CQP",                                                                    \
         OFFSET(fixed_qp_I),                                                                                           \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = 30},                                                                                                  \
         0,                                                                                                            \
         51,                                                                                                           \
         FLAGS},                                                                                                       \
        {"fixed_qp_B",                                                                                                 \
         "the QP for B frame while rc mode is CQP",                                                                    \
         OFFSET(fixed_qp_B),                                                                                           \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = 32},                                                                                                  \
         0,                                                                                                            \
         51,                                                                                                           \
         FLAGS},                                                                                                       \
        {"fixed_qp_P",                                                                                                 \
         "the QP for P frame while rc mode is CQP",                                                                    \
         OFFSET(fixed_qp_P),                                                                                           \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = 32},                                                                                                  \
         0,                                                                                                            \
         51,                                                                                                           \
         FLAGS},                                                                                                       \
        {"slice_size",                                                                                                 \
         "slice size in number of CTU rows. (default [0], 0..height/ctu_size)",                                        \
         OFFSET(slice_size),                                                                                           \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = 0},                                                                                                   \
         0,                                                                                                            \
         INT_MAX,                                                                                                      \
         FLAGS},                                                                                                       \
        {"enable_sei",                                                                                                 \
         "Enable SEI message, 0: disable, 1:enable",                                                                   \
         OFFSET(enable_sei),                                                                                           \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = 0},                                                                                                   \
         0,                                                                                                            \
         1,                                                                                                            \
         FLAGS},                                                                                                       \
        {"enable_deblock",                                                                                             \
         "Enable deblock filter",                                                                                      \
         OFFSET(enable_deblocking),                                                                                    \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = 1},                                                                                                   \
         0,                                                                                                            \
         1,                                                                                                            \
         FLAGS},                                                                                                       \
        {"roi",                                                                                                        \
         "roi area config, 'rN:enable(0/1):isAbsQp(0/1):qp value:x:y:width:height;' N: which roi area 0~7, "           \
         "multiple rois are separated by ';'",                                                                         \
         OFFSET(roi_str),                                                                                              \
         AV_OPT_TYPE_STRING,                                                                                           \
         {.str = "r0:0:1:30:0:0:63:63;r1:0:0:-30:64:0:63:63"},                                                         \
         0,                                                                                                            \
         1,                                                                                                            \
         FLAGS},                                                                                                       \
        {"enable_vui",                                                                                                 \
         "Enable Write VUI timing info in SPS",                                                                        \
         OFFSET(enable_vui),                                                                                           \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = 1},                                                                                                   \
         0,                                                                                                            \
         1,                                                                                                            \
         FLAGS},                                                                                                       \
        {"svct_maxlayer", "set SVC-T max layers", OFFSET(max_TLayers), AV_OPT_TYPE_INT, {.i64 = 1}, 1, 5, FLAGS},      \
        {"b_nums", "set number of B frame between P-P", OFFSET(b_nums), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 4, FLAGS},     \
        {"insert_aud", "insert AUD nal type. 1:enable", OFFSET(insert_AUD), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, FLAGS}, \
        {"insert_sps_pps",                                                                                             \
         "force insert sps pps nal.1:enable",                                                                          \
         OFFSET(insert_SPS_PPS),                                                                                       \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = 0},                                                                                                   \
         0,                                                                                                            \
         1,                                                                                                            \
         FLAGS},                                                                                                       \
        {"stride_align",                                                                                               \
         "set the stride alignment of input frame, multiple of 16",                                                    \
         OFFSET(exp_of_input_alignment),                                                                               \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = 64},                                                                                                  \
         64,                                                                                                           \
         4096,                                                                                                         \
         FLAGS},                                                                                                       \
        {"hdr10_display",                                                                                              \
         "hdr10display config, 'enable:dx0:dy0:dx1:dy1:dx2:dy2:wx:wy:max:min'.",                                       \
         OFFSET(hdr_display_str),                                                                                      \
         AV_OPT_TYPE_STRING,                                                                                           \
         {.str = "h0:0:0:0:0:0:0:0:0:0:0"},                                                                            \
         0,                                                                                                            \
         1,                                                                                                            \
         FLAGS},                                                                                                       \
        {"hdr10_light",                                                                                                \
         "hdr10light config, 'enable:maxlevel:avglevel'.",                                                             \
         OFFSET(hdr_light_str),                                                                                        \
         AV_OPT_TYPE_STRING,                                                                                           \
         {.str = "h0:0:0"},                                                                                            \
         0,                                                                                                            \
         1,                                                                                                            \
         FLAGS},                                                                                                       \
        {"hdr10_color",                                                                                                \
         "hdr10color config, 'enable:primary:matrix:transfer'.",                                                       \
         OFFSET(hdr_color_str),                                                                                        \
         AV_OPT_TYPE_STRING,                                                                                           \
         {.str = "h0:9:9:0"},                                                                                          \
         0,                                                                                                            \
         1,                                                                                                            \
         FLAGS},                                                                                                       \
        {"skip_cumap_enable",                                                                                          \
         "enable skip CU map,0: disable,1: enable",                                                                    \
         OFFSET(skip_map_enable),                                                                                      \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = 0},                                                                                                   \
         0,                                                                                                            \
         1,                                                                                                            \
         FLAGS},                                                                                                       \
        {"roi_qpmap_enable",                                                                                           \
         "enable roi qp map,0: disable,1: enable",                                                                     \
         OFFSET(roi_map_delta_qp_enable),                                                                              \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = 0},                                                                                                   \
         0,                                                                                                            \
         1,                                                                                                            \
         FLAGS},                                                                                                       \
        {"enable_const_chroma",                                                                                        \
         "enable setting chroma a constant pixel value.",                                                              \
         OFFSET(const_chroma_en),                                                                                      \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = 0},                                                                                                   \
         0,                                                                                                            \
         1,                                                                                                            \
         FLAGS},                                                                                                       \
        {"const_cb",                                                                                                   \
         "The constant pixel value for Cb. for 8bit default [0], 0..255, for 10bit default [0], 0..1023)",             \
         OFFSET(const_cb),                                                                                             \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = 0},                                                                                                   \
         0,                                                                                                            \
         1023,                                                                                                         \
         FLAGS},                                                                                                       \
        {"const_cr",                                                                                                   \
         "The constant pixel value for Cr. for 8bit 0..255, for 10bit 0..1023)",                                       \
         OFFSET(const_cr),                                                                                             \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = 0},                                                                                                   \
         0,                                                                                                            \
         1023,                                                                                                         \
         FLAGS},                                                                                                       \
        {"rdo_level",                                                                                                  \
         "Programable hardware RDO Level.Lower value means lower quality but better performance, "                     \
         "Higher value means higher quality but worse performance.",                                                   \
         OFFSET(rdo_level),                                                                                            \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = 1},                                                                                                   \
         1,                                                                                                            \
         3,                                                                                                            \
         FLAGS},                                                                                                       \
        {"ipcm",                                                                                                       \
         "ipcm area config, 'iN:enable(0/1):x:y:width:height;' N: which ipcm area 0~1, "                               \
         "multiple ipcm are separated by ';'",                                                                         \
         OFFSET(ipcm_str),                                                                                             \
         AV_OPT_TYPE_STRING,                                                                                           \
         {.str = "i0:0:0:0:63:63;i1:0:64:0:63:63"},                                                                    \
         0,                                                                                                            \
         1,                                                                                                            \
         FLAGS},                                                                                                       \
        {"linebuf_mode",                                                                                               \
         "The line buf mode.0 = Disable input line buffer.1 = Enable SW handshaking Loop-back enabled.2 = Enable HW "  \
         "handshaking Loop-back enabled.3 = Enable SW handshaking Loop-back disabled.4 = Enable HW handshaking "       \
         "Loop-back disabled.",                                                                                        \
         OFFSET(input_line_buf_mode),                                                                                  \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = 0},                                                                                                   \
         0,                                                                                                            \
         4,                                                                                                            \
         FLAGS},                                                                                                       \
        {"linebuf_depth",                                                                                              \
         "The number of CTB/MB rows to control loop-back and handshaking",                                             \
         OFFSET(input_line_buf_depth),                                                                                 \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = 1},                                                                                                   \
         0,                                                                                                            \
         511,                                                                                                          \
         FLAGS},                                                                                                       \
        {"linebuf_amount",                                                                                             \
         "Handshake sync amount for every loop-back.",                                                                 \
         OFFSET(amount_per_loop_back),                                                                                 \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = 1},                                                                                                   \
         0,                                                                                                            \
         1023,                                                                                                         \
         FLAGS},                                                                                                       \
        {"dump_path", "dump directory", OFFSET(dump_path), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS},            \
        {"frame_dump", "frame dump enable", OFFSET(dump_frame_enable), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, FLAGS},      \
        {"packet_dump", "packet dump enable", OFFSET(dump_pkt_enable), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, FLAGS},      \
        {"frame_dump_time",                                                                                            \
         "the time length of dumpping frame",                                                                          \
         OFFSET(dump_frame_time),                                                                                      \
         AV_OPT_TYPE_INT,                                                                                              \
         {.i64 = 0},                                                                                                   \
         0,                                                                                                            \
         INT_MAX,                                                                                                      \
         FLAGS},                                                                                                       \
    {                                                                                                                  \
        "packet_dump_time", "the time length of dumpping packet", OFFSET(dump_pkt_time), AV_OPT_TYPE_INT, {.i64 = 0},  \
            0, INT_MAX, FLAGS                                                                                          \
    }

static const AVOption h264_es_options[] = {
    // encoder common setting
    ESENC_H26X_OPTS,
    {"profile",
     "Set encode profile.[9, 12]: 9-Baseline, 10-Main, 11-High, 12-High 10",
     OFFSET(profile),
     AV_OPT_TYPE_INT,
     {.i64 = 11},
     9,
     12,
     FLAGS,
     "profile"},
    {"baseline", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_H264_BASE_PROFILE}, 0, 0, FLAGS, "profile"},
    {"main", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_H264_MAIN_PROFILE}, 0, 0, FLAGS, "profile"},
    {"high", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_H264_HIGH_PROFILE}, 0, 0, FLAGS, "profile"},
    {"high10", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_H264_HIGH_10_PROFILE}, 0, 0, FLAGS, "profile"},
    {"level", "Set encode level.[10, 99]", OFFSET(level), AV_OPT_TYPE_INT, {.i64 = 51}, 10, 99, FLAGS, "level"},
    {"1", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_H264_LEVEL_1}, 0, 0, FLAGS, "level"},
    {"1.b", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_H264_LEVEL_1_b}, 0, 0, FLAGS, "level"},
    {"1.1", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_H264_LEVEL_1_1}, 0, 0, FLAGS, "level"},
    {"1.2", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_H264_LEVEL_1_2}, 0, 0, FLAGS, "level"},
    {"1.3", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_H264_LEVEL_1_3}, 0, 0, FLAGS, "level"},
    {"2", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_H264_LEVEL_2}, 0, 0, FLAGS, "level"},
    {"2.1", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_H264_LEVEL_2_1}, 0, 0, FLAGS, "level"},
    {"2.2", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_H264_LEVEL_2_2}, 0, 0, FLAGS, "level"},
    {"3", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_H264_LEVEL_3}, 0, 0, FLAGS, "level"},
    {"3.1", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_H264_LEVEL_3_1}, 0, 0, FLAGS, "level"},
    {"3.2", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_H264_LEVEL_3_2}, 0, 0, FLAGS, "level"},
    {"4", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_H264_LEVEL_4}, 0, 0, FLAGS, "level"},
    {"4.1", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_H264_LEVEL_4_1}, 0, 0, FLAGS, "level"},
    {"4.2", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_H264_LEVEL_4_2}, 0, 0, FLAGS, "level"},
    {"5", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_H264_LEVEL_5}, 0, 0, FLAGS, "level"},
    {"5.1", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_H264_LEVEL_5_1}, 0, 0, FLAGS, "level"},
    {"5.2", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_H264_LEVEL_5_2}, 0, 0, FLAGS, "level"},
    {"6", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_H264_LEVEL_6}, 0, 0, FLAGS, "level"},
    {"6.1", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_H264_LEVEL_6_1}, 0, 0, FLAGS, "level"},
    {"6.2", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_H264_LEVEL_6_2}, 0, 0, FLAGS, "level"},
    {"enable_cabac",
     "entropy coding mode, 0:CAVLC, 1:CABAC",
     OFFSET(enable_cabac),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     1,
     FLAGS},
    {NULL},
};

static const AVOption h265_es_options[] = {
    // encoder common setting
    ESENC_H26X_OPTS,
    {"profile",
     "Set encode profile.[0, 3]: 0-Main, 1-Main Still, 2-Main 10, 3-Mainrext",
     OFFSET(profile),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     3,
     FLAGS,
     "profile"},
    {"main", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_HEVC_MAIN_PROFILE}, 0, 0, FLAGS, "profile"},
    {"main_still", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_HEVC_MAIN_STILL_PICTURE_PROFILE}, 0, 0, FLAGS, "profile"},
    {"main10", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_HEVC_MAIN_10_PROFILE}, 0, 0, FLAGS, "profile"},
    {"main_rext", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_HEVC_MAINREXT}, 0, 0, FLAGS, "profile"},
    {"level", "Set encode level.[30, 186]", OFFSET(level), AV_OPT_TYPE_INT, {.i64 = 180}, 30, 186, FLAGS, "level"},
    {"1", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_HEVC_LEVEL_1}, 0, 0, FLAGS, "level"},
    {"2", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_HEVC_LEVEL_2}, 0, 0, FLAGS, "level"},
    {"2.1", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_HEVC_LEVEL_2_1}, 0, 0, FLAGS, "level"},
    {"3", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_HEVC_LEVEL_3}, 0, 0, FLAGS, "level"},
    {"3.1", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_HEVC_LEVEL_3_1}, 0, 0, FLAGS, "level"},
    {"4", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_HEVC_LEVEL_4}, 0, 0, FLAGS, "level"},
    {"4.1", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_HEVC_LEVEL_4_1}, 0, 0, FLAGS, "level"},
    {"5", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_HEVC_LEVEL_5}, 0, 0, FLAGS, "level"},
    {"5.1", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_HEVC_LEVEL_5_1}, 0, 0, FLAGS, "level"},
    {"5.2", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_HEVC_LEVEL_5_2}, 0, 0, FLAGS, "level"},
    {"6", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_HEVC_LEVEL_6}, 0, 0, FLAGS, "level"},
    {"6.1", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_HEVC_LEVEL_6_1}, 0, 0, FLAGS, "level"},
    {"6.2", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_HEVC_LEVEL_6_2}, 0, 0, FLAGS, "level"},
    {"tier",
     "Set tier.[0, 1]: 0-Main tier, 1-High tier)",
     OFFSET(tier),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     1,
     FLAGS,
     "tier"},
    {"main", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_HEVC_MAIN_TIER}, 0, 0, FLAGS, "tier"},
    {"high", "", 0, AV_OPT_TYPE_CONST, {.i64 = VCENC_HEVC_HIGH_TIER}, 0, 0, FLAGS, "tier"},
    {"enable_sao", "enable SAO. 0: disable, 1: enable.", OFFSET(enable_sao), AV_OPT_TYPE_INT, {.i64 = 1}, 0, 1, FLAGS},
    {NULL},
};

static const AVCodecHWConfigInternal *esenc_h26x_hw_configs[] = {
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

static const enum AVPixelFormat esenc_h26x_support_pixfmts[] = {AV_PIX_FMT_ES,
                                                                AV_PIX_FMT_YUV420P,
                                                                AV_PIX_FMT_NV12,
                                                                AV_PIX_FMT_NV21,
                                                                AV_PIX_FMT_UYVY422,
                                                                AV_PIX_FMT_YUYV422,
                                                                AV_PIX_FMT_YUV420P10LE,
                                                                AV_PIX_FMT_P010LE,
                                                                AV_PIX_FMT_NONE};

#define ESENC_H26X_ENCODER(codectype, CODECTYPE)                           \
    static const AVClass codectype##_es_encoder_class = {                  \
        .class_name = #codectype "_es_encoder",                            \
        .item_name = av_default_item_name,                                 \
        .option = codectype##_es_options,                                  \
        .version = LIBAVUTIL_VERSION_INT,                                  \
    };                                                                     \
    FFCodec ff_##codectype##_es_encoder = {                                \
        .p.name = #codectype "_es_encoder",                                  \
        .p.long_name = NULL_IF_CONFIG_SMALL("ESWIN " #codectype " Encoder"), \
        .p.type = AVMEDIA_TYPE_VIDEO,                                        \
        .p.id = AV_CODEC_ID_##CODECTYPE,                                     \
        .priv_data_size = sizeof(ESEncVidContext),                         \
        .init = &ff_vsv_h26x_encode_init,                                  \
        .close = &ff_vsv_h26x_encode_close,                                \
        FF_CODEC_RECEIVE_PACKET_CB(ff_vsv_h26x_encode_receive_packet),    \
        .p.priv_class = &codectype##_es_encoder_class,                       \
        .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE,        \
        .p.pix_fmts = esenc_h26x_support_pixfmts,                            \
        .hw_configs = esenc_h26x_hw_configs,                               \
        .p.wrapper_name = "es",                                              \
    };

ESENC_H26X_ENCODER(h264, H264)
ESENC_H26X_ENCODER(h265, H265)

#undef ESENC_H26X_ENCODER
