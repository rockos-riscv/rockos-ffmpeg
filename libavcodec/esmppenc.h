#ifndef AVCODEC_ESMPPENC_H
#define AVCODEC_ESMPPENC_H

#include <es_mpp.h>
#include <mpp_venc_cfg.h>
#include <mpp_frame.h>
#include <es_venc_def.h>
#include <es_mpp_rc.h>

#include "esmpp_comm.h"

#include "codec_internal.h"
#include "encode.h"
#include "hwconfig.h"
#include "internal.h"

#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#define H26X_HEADER_SIZE 1024
#define H26X_ASYNC_FRAMES 5  // current max gop size 4
#define MJPEG_ASYNC_FRAMES 8
#define ALIGN_DOWN(a, b) ((a) & ~((b) - 1))

typedef struct ESMPPEncFrame {
    AVFrame *frame;
    MppFramePtr mpp_frame;
    struct ESMPPEncFrame *next;
    int queued;
} ESMPPEncFrame;

typedef struct ESMPPEncContext {
    AVClass *class;

    MppCtxPtr mctx;

    MppEncCfgPtr mcfg;
    int cfg_init;
    enum AVPixelFormat pix_fmt;

    ESMPPEncFrame *frame_list;
    int async_frames;  // cache
    int sent_frm_cnt;
    int got_pkt_cnt;

    // common setting
    int profile;
    int tier;
    int level;
    int coder;
    int stride_align;
    int v_stride_align;
    int bitdepth;
    int enable_cabac;

    // preprocessing setting
    int rotation;
    char *crop_str;

    // rc setting
    int rc_mode;
    int stat_time;  // [1, 60]; the rate statistic time,  unit is sec
    int cpb_size;
    int iqp;
    int pqp;
    int bqp;

    int iprop;
    int qp_max;
    int qp_min;
    int qp_max_i;
    int qp_min_i;
    // mjpeg
    int qfactor;
    int qfactor_max;
    int qfactor_min;

    // gop setting
    int gop_mode;
    int ip_qp_delta;
    int sb_interval;
    int sp_qp_delta;
    int bg_interval;
    int bg_qp_delta;
    int vi_qp_delta;
    int b_frm_num;
    int b_qp_delta;
    int i_qp_delta;

    // protocal
    int enable_deblocking;
    char *mastering_display;
    char *content_light;

    // dump
    char *dump_path;
    int32_t dump_frame_enable;
    int32_t dump_frame_time;
    DumpHandle *dump_frame_hnd;
    int32_t dump_frame_count;
    int32_t dump_pkt_enable;
    int32_t dump_pkt_time;
    DumpHandle *dump_pkt_hnd;
    int32_t dump_pkt_count;
    time_t dump_start_time;

    MppBufferGroupPtr frame_grp;
} ESMPPEncContext;

static const AVRational mpp_tb = {1, 1000000};

#define PTS_TO_MPP_PTS(pts, pts_tb) ((pts_tb.num && pts_tb.den) ? av_rescale_q(pts, pts_tb, mpp_tb) : pts)

#define MPP_PTS_TO_PTS(mpp_pts, pts_tb) ((pts_tb.num && pts_tb.den) ? av_rescale_q(mpp_pts, mpp_tb, pts_tb) : mpp_pts)

#define OFFSET(x) offsetof(ESMPPEncContext, x)
#define VE (AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

#define ESMPP_ENC_COMMON_OPTS                                                                                         \
    {"stride_align",                                                                                                  \
     "set the stride alignment of input frame, multiple of 16",                                                       \
     OFFSET(stride_align),                                                                                            \
     AV_OPT_TYPE_INT,                                                                                                 \
     {.i64 = -1},                                                                                                     \
     -1,                                                                                                              \
     4096,                                                                                                            \
     VE},                                                                                                             \
        {"v_stride_align",                                                                                            \
         "set the vertical stride alignment of input frame, multiple of 2",                                           \
         OFFSET(v_stride_align),                                                                                      \
         AV_OPT_TYPE_INT,                                                                                             \
         {.i64 = -1},                                                                                                 \
         -1,                                                                                                          \
         4096,                                                                                                        \
         VE},                                                                                                         \
        {"rotation",                                                                                                  \
         "Rotation. 0:0 1:90° 2:180° 3:270°.",                                                                        \
         OFFSET(rotation),                                                                                            \
         AV_OPT_TYPE_INT,                                                                                             \
         {.i64 = -1},                                                                                                 \
         -1,                                                                                                          \
         3,                                                                                                           \
         VE,                                                                                                          \
         "rotation"},                                                                                                 \
        {"crop",                                                                                                      \
         "crop 'cx:N,cy:N,cw:N,ch:N',mean crop xoffset,yoffset,out_width,out_heigh",                                  \
         OFFSET(crop_str),                                                                                            \
         AV_OPT_TYPE_STRING,                                                                                          \
         {.str = NULL},                                                                                               \
         0,                                                                                                           \
         0,                                                                                                           \
         VE,                                                                                                          \
         "crop"},                                                                                                     \
        {"rc_mode",                                                                                                   \
         "Set rc mode, 0:CBR, 1:VBR, 2:CQP",                                                                          \
         OFFSET(rc_mode),                                                                                             \
         AV_OPT_TYPE_INT,                                                                                             \
         {.i64 = 1},                                                                                                  \
         0,                                                                                                           \
         2,                                                                                                           \
         VE,                                                                                                          \
         "rc_mode"},                                                                                                  \
        {"CBR", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 0}, 0, 0, VE, "rc_mode"},                                         \
        {"VBR", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 1}, 0, 0, VE, "rc_mode"},                                         \
        {"CQP", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 2}, 0, 0, VE, "rc_mode"},                                         \
        {"stat_time", "[1, 60]", OFFSET(stat_time), AV_OPT_TYPE_INT, {.i64 = 1}, 1, 60, VE, "stat_time"},             \
        {"dump_path", "dump directory", OFFSET(dump_path), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, VE},              \
        {"frame_dump", "frame dump enable", OFFSET(dump_frame_enable), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, VE},        \
        {"packet_dump", "packet dump enable", OFFSET(dump_pkt_enable), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, VE},        \
        {"frame_dump_time",                                                                                           \
         "the time length of dumpping frame",                                                                         \
         OFFSET(dump_frame_time),                                                                                     \
         AV_OPT_TYPE_INT,                                                                                             \
         {.i64 = 0},                                                                                                  \
         0,                                                                                                           \
         INT_MAX,                                                                                                     \
         VE},                                                                                                         \
    {                                                                                                                 \
        "packet_dump_time", "the time length of dumpping packet", OFFSET(dump_pkt_time), AV_OPT_TYPE_INT, {.i64 = 0}, \
            0, INT_MAX, VE                                                                                            \
    }

#define ESMPP_ENC_VIDEO_COMMON_OPTS                                                                       \
    {"bitdepth",                                                                                          \
     "Bitdepth. 8=8-bit, 10=10-bit.",                                                                     \
     OFFSET(bitdepth),                                                                                    \
     AV_OPT_TYPE_INT,                                                                                     \
     {.i64 = -1},                                                                                         \
     -1,                                                                                                  \
     BIT_DEPTH_10BIT,                                                                                     \
     VE,                                                                                                  \
     "bitdepth"},                                                                                         \
        {"enable_deblock",                                                                                \
         "Enable deblock filter",                                                                         \
         OFFSET(enable_deblocking),                                                                       \
         AV_OPT_TYPE_INT,                                                                                 \
         {.i64 = -1},                                                                                     \
         -1,                                                                                              \
         1,                                                                                               \
         VE},                                                                                             \
        {"mastering_display",                                                                             \
         "mastering_display 'R(x,y)G(x,y)B(x,y)WP(x,y)L(x,y)'",                                           \
         OFFSET(mastering_display),                                                                       \
         AV_OPT_TYPE_STRING,                                                                              \
         {.str = NULL},                                                                                   \
         0,                                                                                               \
         0,                                                                                               \
         VE},                                                                                             \
    {                                                                                                     \
        "content_light", "content_light 'maxcll:N,maxfall:N'", OFFSET(content_light), AV_OPT_TYPE_STRING, \
            {.str = NULL}, 0, 0, VE                                                                       \
    }

#define ESMPP_ENC_VIDEO_GOP_OPTS                                                                                     \
    {"gop_mode",                                                                                                     \
     "normalP, dualP, smartref, advsmartref, bipredB, lowdelayB",                                                    \
     OFFSET(gop_mode),                                                                                               \
     AV_OPT_TYPE_INT,                                                                                                \
     {.i64 = VENC_GOPMODE_NORMALP},                                                                                  \
     VENC_GOPMODE_NORMALP,                                                                                           \
     VENC_GOPMODE_BUTT,                                                                                              \
     VE,                                                                                                             \
     "gop_mode"},                                                                                                    \
        {"ip_qp_delta",                                                                                              \
         "[-51,51]; QP variance between P frame and I frame.",                                                       \
         OFFSET(ip_qp_delta),                                                                                        \
         AV_OPT_TYPE_INT,                                                                                            \
         {.i64 = 2},                                                                                                 \
         -51,                                                                                                        \
         51,                                                                                                         \
         VE,                                                                                                         \
         "ip_qp_delta"},                                                                                             \
        {"sb_interval",                                                                                              \
         "[0, 65536]; Interval of the special B frames.",                                                            \
         OFFSET(sb_interval),                                                                                        \
         AV_OPT_TYPE_INT,                                                                                            \
         {.i64 = 0},                                                                                                 \
         0,                                                                                                          \
         65536,                                                                                                      \
         VE,                                                                                                         \
         "sb_interval"},                                                                                             \
        {"sp_qp_delta",                                                                                              \
         "[-51,51]; QP variance between general P/B frame and special B frame.",                                     \
         OFFSET(sp_qp_delta),                                                                                        \
         AV_OPT_TYPE_INT,                                                                                            \
         {.i64 = 0},                                                                                                 \
         -51,                                                                                                        \
         51,                                                                                                         \
         VE,                                                                                                         \
         "sp_qp_delta"},                                                                                             \
        {"bg_interval",                                                                                              \
         "Interval of the long-term reference frame, can not be less than gop.",                                     \
         OFFSET(bg_interval),                                                                                        \
         AV_OPT_TYPE_INT,                                                                                            \
         {.i64 = -1},                                                                                                \
         -1,                                                                                                         \
         65536,                                                                                                      \
         VE,                                                                                                         \
         "bg_interval"},                                                                                             \
        {"bg_qp_delta",                                                                                              \
         "[-51,51]; QP variance between P frame and Bg frame.",                                                      \
         OFFSET(bg_qp_delta),                                                                                        \
         AV_OPT_TYPE_INT,                                                                                            \
         {.i64 = 5},                                                                                                 \
         -51,                                                                                                        \
         51,                                                                                                         \
         VE,                                                                                                         \
         "bg_qp_delta"},                                                                                             \
        {"vi_qp_delta",                                                                                              \
         "[-51,51]; QP variance between P frame and virtual I frame.",                                               \
         OFFSET(vi_qp_delta),                                                                                        \
         AV_OPT_TYPE_INT,                                                                                            \
         {.i64 = 3},                                                                                                 \
         -51,                                                                                                        \
         51,                                                                                                         \
         VE,                                                                                                         \
         "vi_qp_delta"},                                                                                             \
        {"b_frm_num",                                                                                                \
         "[1,3]; Number of B frames.",                                                                               \
         OFFSET(b_frm_num),                                                                                          \
         AV_OPT_TYPE_INT,                                                                                            \
         {.i64 = 2},                                                                                                 \
         1,                                                                                                          \
         3,                                                                                                          \
         VE,                                                                                                         \
         "b_frm_num"},                                                                                               \
        {"b_qp_delta",                                                                                               \
         "[-51,51]; QP variance between P frame and B frame.",                                                       \
         OFFSET(b_qp_delta),                                                                                         \
         AV_OPT_TYPE_INT,                                                                                            \
         {.i64 = 0},                                                                                                 \
         -51,                                                                                                        \
         51,                                                                                                         \
         VE,                                                                                                         \
         "b_qp_delta"},                                                                                              \
    {                                                                                                                \
        "i_qp_delta", "[-51,51]; QP variance between other frame and I frame.", OFFSET(i_qp_delta), AV_OPT_TYPE_INT, \
            {.i64 = 2}, -51, 51, VE, "i_qp_delta"                                                                    \
    }

#define ESMPP_ENC_VIDEO_RC_OPTS                                                                                      \
    {"cpb_size", "[10, 800000]", OFFSET(cpb_size), AV_OPT_TYPE_INT, {.i64 = -1}, -1, 800000, VE, "cpb_size"},        \
        {"iqp", "[0, 51]", OFFSET(iqp), AV_OPT_TYPE_INT, {.i64 = 30}, 0, 51, VE, "iqp"},                             \
        {"pqp", "[0, 51]", OFFSET(pqp), AV_OPT_TYPE_INT, {.i64 = 32}, 0, 51, VE, "pqp"},                             \
        {"bqp", "[0, 51]", OFFSET(bqp), AV_OPT_TYPE_INT, {.i64 = 32}, 0, 51, VE, "bqp"},                             \
        {"iprop",                                                                                                    \
         "[50, 100]: Set ratio of I-frames to P-frames",                                                             \
         OFFSET(iprop),                                                                                              \
         AV_OPT_TYPE_INT,                                                                                            \
         {.i64 = -1},                                                                                                \
         -1,                                                                                                         \
         100,                                                                                                        \
         VE,                                                                                                         \
         "iprop"},                                                                                                   \
        {"qp_max",                                                                                                   \
         "[0, 51]: Set the max QP value for P and B frame",                                                          \
         OFFSET(qp_max),                                                                                             \
         AV_OPT_TYPE_INT,                                                                                            \
         {.i64 = -1},                                                                                                \
         -1,                                                                                                         \
         51,                                                                                                         \
         VE,                                                                                                         \
         "qp_max"},                                                                                                  \
        {"qp_min",                                                                                                   \
         "[0, 51]:Set the min QP value for P and B frame",                                                           \
         OFFSET(qp_min),                                                                                             \
         AV_OPT_TYPE_INT,                                                                                            \
         {.i64 = -1},                                                                                                \
         -1,                                                                                                         \
         51,                                                                                                         \
         VE,                                                                                                         \
         "qp_min"},                                                                                                  \
        {"qp_max_i",                                                                                                 \
         "[0, 51]: Set the max QP value for I frame",                                                                \
         OFFSET(qp_max_i),                                                                                           \
         AV_OPT_TYPE_INT,                                                                                            \
         {.i64 = -1},                                                                                                \
         -1,                                                                                                         \
         51,                                                                                                         \
         VE,                                                                                                         \
         "qp_max_i"},                                                                                                \
    {                                                                                                                \
        "qp_min_i", "[0, 51]: Set the min QP value for I frame", OFFSET(qp_min_i), AV_OPT_TYPE_INT, {.i64 = -1}, -1, \
            51, VE, "qp_min_i"                                                                                       \
    }

static const AVOption h264_options[] = {
    ESMPP_ENC_VIDEO_RC_OPTS,
    ESMPP_ENC_VIDEO_GOP_OPTS,
    ESMPP_ENC_COMMON_OPTS,
    ESMPP_ENC_VIDEO_COMMON_OPTS,
    {"profile",
     "Set the encoding profile",
     OFFSET(profile),
     AV_OPT_TYPE_INT,
     {.i64 = PROFILE_H264_HIGH},
     -1,
     PROFILE_H264_HIGH10,
     VE,
     "profile"},
    {"baseline", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = PROFILE_H264_BASELINE}, INT_MIN, INT_MAX, VE, "profile"},
    {"main", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = PROFILE_H264_MAIN}, INT_MIN, INT_MAX, VE, "profile"},
    {"high", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = PROFILE_H264_HIGH}, INT_MIN, INT_MAX, VE, "profile"},
    {"high10", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = PROFILE_H264_HIGH10}, INT_MIN, INT_MAX, VE, "profile"},
    {"level",
     "Set the encoding level",
     OFFSET(level),
     AV_OPT_TYPE_INT,
     {.i64 = ES_H264_LEVEL_5_1},
     ES_LEVEL_UNKNOWN,
     ES_H264_LEVEL_6_2,
     VE,
     "level"},
    {"1", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_H264_LEVEL_1}, 0, 0, VE, "level"},
    {"1.b", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_H264_LEVEL_1_b}, 0, 0, VE, "level"},
    {"1.1", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_H264_LEVEL_1_1}, 0, 0, VE, "level"},
    {"1.2", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_H264_LEVEL_1_2}, 0, 0, VE, "level"},
    {"1.3", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_H264_LEVEL_1_3}, 0, 0, VE, "level"},
    {"2", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_H264_LEVEL_2}, 0, 0, VE, "level"},
    {"2.1", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_H264_LEVEL_2_1}, 0, 0, VE, "level"},
    {"2.2", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_H264_LEVEL_2_2}, 0, 0, VE, "level"},
    {"3", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_H264_LEVEL_3}, 0, 0, VE, "level"},
    {"3.1", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_H264_LEVEL_3_1}, 0, 0, VE, "level"},
    {"3.2", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_H264_LEVEL_3_2}, 0, 0, VE, "level"},
    {"4", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_H264_LEVEL_4}, 0, 0, VE, "level"},
    {"4.1", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_H264_LEVEL_4_1}, 0, 0, VE, "level"},
    {"4.2", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_H264_LEVEL_4_2}, 0, 0, VE, "level"},
    {"5", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_H264_LEVEL_5}, 0, 0, VE, "level"},
    {"5.1", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_H264_LEVEL_5_1}, 0, 0, VE, "level"},
    {"5.2", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_H264_LEVEL_5_2}, 0, 0, VE, "level"},
    {"6", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_H264_LEVEL_6}, 0, 0, VE, "level"},
    {"6.1", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_H264_LEVEL_6_1}, 0, 0, VE, "level"},
    {"6.2", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_H264_LEVEL_6_2}, 0, 0, VE, "level"},
    {"enable_cabac", "entropy enable cabac", OFFSET(enable_cabac), AV_OPT_TYPE_INT, {.i64 = -1}, -1, 1, VE, "cabac"},
    {NULL}};

static const AVOption hevc_options[] = {
    ESMPP_ENC_VIDEO_RC_OPTS,
    ESMPP_ENC_VIDEO_GOP_OPTS,
    ESMPP_ENC_COMMON_OPTS,
    ESMPP_ENC_VIDEO_COMMON_OPTS,
    {"profile",
     "Set the encoding profile",
     OFFSET(profile),
     AV_OPT_TYPE_INT,
     {.i64 = PROFILE_H265_MAIN},
     PROFILE_H265_MAIN,
     PROFILE_H265_MAIN_STILL_PICTURE,
     VE,
     "profile"},
    {"main", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = PROFILE_H265_MAIN}, INT_MIN, INT_MAX, VE, "profile"},
    {"main10", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = PROFILE_H265_MAIN10}, INT_MIN, INT_MAX, VE, "profile"},
    {"main_still",
     NULL,
     0,
     AV_OPT_TYPE_CONST,
     {.i64 = PROFILE_H265_MAIN_STILL_PICTURE},
     INT_MIN,
     INT_MAX,
     VE,
     "profile"},
    {"tier", "Set the encoding profile tier", OFFSET(tier), AV_OPT_TYPE_INT, {.i64 = -1}, -1, 1, VE, "tier"},
    {"main", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 0}, INT_MIN, INT_MAX, VE, "tier"},
    {"high", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 1}, INT_MIN, INT_MAX, VE, "tier"},
    {"level",
     "Set the encoding level",
     OFFSET(level),
     AV_OPT_TYPE_INT,
     {.i64 = ES_HEVC_LEVEL_6},
     ES_LEVEL_UNKNOWN,
     ES_HEVC_LEVEL_6_2,
     VE,
     "level"},
    {"1", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_HEVC_LEVEL_1}, 0, 0, VE, "level"},
    {"2", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_HEVC_LEVEL_2}, 0, 0, VE, "level"},
    {"2.1", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_HEVC_LEVEL_2_1}, 0, 0, VE, "level"},
    {"3", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_HEVC_LEVEL_3}, 0, 0, VE, "level"},
    {"3.1", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_HEVC_LEVEL_3_1}, 0, 0, VE, "level"},
    {"4", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_HEVC_LEVEL_4}, 0, 0, VE, "level"},
    {"4.1", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_HEVC_LEVEL_4_1}, 0, 0, VE, "level"},
    {"5", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_HEVC_LEVEL_5}, 0, 0, VE, "level"},
    {"5.1", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_HEVC_LEVEL_5_1}, 0, 0, VE, "level"},
    {"5.2", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_HEVC_LEVEL_5_2}, 0, 0, VE, "level"},
    {"6", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_HEVC_LEVEL_6}, 0, 0, VE, "level"},
    {"6.1", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_HEVC_LEVEL_6_1}, 0, 0, VE, "level"},
    {"6.2", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = ES_HEVC_LEVEL_6_2}, 0, 0, VE, "level"},
    {NULL}};

static const AVOption mjpeg_options[] = {ESMPP_ENC_COMMON_OPTS,
                                         {"qfactor",
                                          "Set the value of a quantization factor [1, 99]",
                                          OFFSET(qfactor),
                                          AV_OPT_TYPE_INT,
                                          {.i64 = -1},
                                          -1,
                                          99,
                                          VE,
                                          "qfactor"},
                                         {"qfactor_max",
                                          "Set the max Q_Factor value [1, 99]",
                                          OFFSET(qfactor_max),
                                          AV_OPT_TYPE_INT,
                                          {.i64 = -1},
                                          -1,
                                          99,
                                          VE,
                                          "qfactor_max"},
                                         {"qfactor_min",
                                          "Set the min Q_Factor value [1, 99], less than qfactor_max",
                                          OFFSET(qfactor_min),
                                          AV_OPT_TYPE_INT,
                                          {.i64 = -1},
                                          -1,
                                          99,
                                          VE,
                                          "qfactor_min"},
                                         {NULL}};

static const enum AVPixelFormat esmpp_enc_pix_fmts[] = {
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_NV21,
    AV_PIX_FMT_YUYV422,
    AV_PIX_FMT_UYVY422,
    AV_PIX_FMT_YUV420P10LE,
    AV_PIX_FMT_P010LE,
    AV_PIX_FMT_DRM_PRIME,
    AV_PIX_FMT_NONE,
};

static const AVCodecHWConfigInternal *const esmpp_enc_hw_configs[] = {
    HW_CONFIG_ENCODER_DEVICE(DRM_PRIME, ESMPP),
    HW_CONFIG_ENCODER_FRAMES(DRM_PRIME, ESMPP),
    NULL,
};

static const FFCodecDefault esmpp_enc_defaults[] = {{"b", "2M"}, {"g", "75"}, {NULL}};

#define DEFINE_ESMPP_ENCODER(x, X, xx)                                                 \
    static const AVClass x##_esmpp_encoder_class = {                                   \
        .class_name = #x "_esmpp_encoder",                                             \
        .item_name = av_default_item_name,                                             \
        .option = x##_options,                                                         \
        .version = LIBAVUTIL_VERSION_INT,                                              \
    };                                                                                 \
    const FFCodec ff_##x##_esmpp_encoder = {                                           \
        .p.name = #x "_esmpp_encoder",                                                 \
        CODEC_LONG_NAME("ESW MPP (Media Process Platform) " #X " encoder"),            \
        .p.type = AVMEDIA_TYPE_VIDEO,                                                  \
        .p.id = AV_CODEC_ID_##X,                                                       \
        .priv_data_size = sizeof(ESMPPEncContext),                                     \
        .p.priv_class = &x##_esmpp_encoder_class,                                      \
        .init = esmpp_encode_init,                                                     \
        .close = esmpp_encode_close,                                                   \
        FF_CODEC_ENCODE_CB(esmpp_encode_frame),                                        \
        .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE,                  \
        .caps_internal = FF_CODEC_CAP_NOT_INIT_THREADSAFE | FF_CODEC_CAP_INIT_CLEANUP, \
        .p.pix_fmts = esmpp_enc_pix_fmts,                                              \
        .hw_configs = esmpp_enc_hw_configs,                                            \
        .defaults = esmpp_enc_defaults,                                                \
        .p.wrapper_name = "esmpp",                                                     \
    };

#endif
