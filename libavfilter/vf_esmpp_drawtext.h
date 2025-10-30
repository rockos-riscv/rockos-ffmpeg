#ifndef AVCODEC_VF_ESMPP_DRAWTEXT_H
#define AVCODEC_VF_ESMPP_DRAWTEXT_H

#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/hwcontext.h"
#include "libavcodec/defs.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "libavutil/imgutils.h"
#include "mpp_buffer.h"
#include "mpp_type.h"
#include "es_roi_def.h"

typedef struct MppDrawTextInfo
{
    enum AVPixelFormat in_fmt;
    int die_idx;
    int device_idx;
    ES_BOOL use_hwaccel;
} MppDrawTextInfo;

typedef struct MppDrawTextContext
{
    const AVClass *class;
    MppRoiCtxPtr mctx;
    AVBufferRef *out_hw_device_ref;
    AVBufferRef *out_hw_frm_ref;
    MppBufferGroupPtr buf_grp;

    enum AVPixelFormat out_fmt;
    enum AVPixelFormat in_fmt;

    ES_BOOL mpp_init;
    int frame_cnt;

    int die_idx;
    int device_idx;

    uint32_t rectangle_color;
    uint32_t rectangle_thickness;
    uint32_t bg_color;
    uint32_t font_color;
    uint32_t font_width;
    uint32_t font_height;
    char *ttf_path;
    MppCtxPtr draw_mctx;
    ES_BOOL draw_init;
    // define 0: get info from ahead filter to draw,
    // 1: draw text and rectangle, 2: draw text only, 3: draw rectangle only
    uint32_t mode;

    int hw_type;
} MppDrawTextContext;

#endif